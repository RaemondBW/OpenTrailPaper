package com.raemond.opentrailpaper.ble

import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothProfile
import android.content.SharedPreferences
import java.io.DataInputStream
import java.io.EOFException
import java.io.IOException
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * A [Radio] whose one device is the firmware simulator (sim/build/otp-sim),
 * reached over its simble TCP port (sim/PROTOCOL.md). Debug builds and tests
 * only.
 *
 * It plays Android's Bluetooth stack, not a better-behaved one: BleManager
 * must see what it would see on a phone, so its connect / bond / refusal /
 * reconnect logic is what gets exercised.
 *
 *  - The bond store is [store] (SharedPreferences): this "phone's" identity
 *    address and which devices it holds a bond with. Clear it to be a phone
 *    that forgot the device; a second store is a second phone.
 *  - createBond() pairs over the open link (simble SECURE) and broadcasts
 *    BONDING then BONDED / NONE, as ACTION_BOND_STATE_CHANGED does. The
 *    passkey is confirmed automatically (the rider tapping Pair).
 *  - A bonded phone encrypts on connect (SECURE right after CONNECTED).
 *  - Disconnect statuses are the HCI reason Android reports: the device's
 *    DISCONNECTED reason (0x05 refusal -> 5, 0x13 -> 19, ...); a socket that
 *    just closes is a lost link, 8 (supervision timeout) — or 61 (MIC
 *    failure, 0x3D) when encryption with our keys had failed; our own
 *    disconnect() reports 0.
 *  - A connect that can't reach the device (socket refused) never reports
 *    STATE_CONNECTED: a direct connect fails with [Options.connectFailStatus]
 *    (133, GATT_ERROR) after [Options.directConnectTimeoutMs]; an autoConnect
 *    keeps trying, and — when [Options.autoConnectFailMs] > 0 — reports the
 *    same failure after that long, as several phones' stacks do for a
 *    background connect.
 *  - ATT error statuses (5, 15, ...) pass through unchanged, as on Android.
 *  - Scanning polls the sim's advertisement (simble ADV) on a short-lived
 *    socket that never sends CONNECT.
 */
class SimRadio(
    private val host: String,
    private val port: Int,
    private val store: SharedPreferences,
    val options: Options = Options(),
) : Radio {

    data class Options(
        val phoneName: String = "Sim Android",
        val mtu: Int = 517,
        val connectFailStatus: Int = 133,
        val directConnectTimeoutMs: Long = 2_000,
        val autoConnectFailMs: Long = 0,
        val scanIntervalMs: Long = 400,
        /** Log lines (tests print them). */
        val log: (String) -> Unit = {},
    )

    /** The sim's device has no MAC; this stands in for it (stable per port). */
    val deviceAddress: String = "5E:00:00:00:%02X:%02X".format((port shr 8) and 0xFF, port and 0xFF)

    @Volatile private var listener: Radio.Listener? = null
    @Volatile private var enabled = true
    @Volatile private var advName: String? = null
    private val bonding = HashSet<String>()
    private val gatts = ArrayList<SimGatt>()
    private val scans = HashMap<Radio.ScanListener, Thread>()

    /** connectGatt calls so far, and links that came up (tests count reconnect loops). */
    val connectAttempts = java.util.concurrent.atomic.AtomicInteger()
    val linksUp = java.util.concurrent.atomic.AtomicInteger()

    /** This phone's identity address, kept in its bond store. */
    val identity: String = store.getString(KEY_ID, null) ?: randomIdentity().also {
        store.edit().putString(KEY_ID, it).commit()
    }

    override val available: Boolean get() = true
    override val isEnabled: Boolean get() = enabled
    override val canScan: Boolean get() = enabled

    override fun setListener(listener: Radio.Listener) { this.listener = listener }

    /** Bluetooth switched off/on in the shade (tests). */
    fun setEnabled(on: Boolean) {
        enabled = on
        if (!on) synchronized(gatts) { gatts.toList() }.forEach { it.linkLost(LINK_LOST) }
        listener?.onAdapterStateChanged()
    }

    /** The phone's bond with the device is gone (Settings > Forget), as the rider would. */
    fun forgetBonds() {
        store.edit().clear().putString(KEY_ID, identity).commit()
    }

    fun holdsBond(address: String): Boolean = store.getBoolean(KEY_BOND + address.uppercase(), false)

    override fun remoteDevice(address: String): RadioDevice? =
        if (BluetoothAdapterAddress.valid(address)) SimDevice(address.uppercase()) else null

    // ---- scanning ------------------------------------------------------------------------------

    override fun startScan(service: UUID, callback: Radio.ScanListener) {
        val t = Thread({
            while (!Thread.currentThread().isInterrupted) {
                val adv = runCatching { probeAdvertisement() }.getOrNull()
                if (adv != null && adv.advertising && service in adv.uuids && enabled) {
                    advName = adv.name
                    callback.onResult(SimDevice(deviceAddress), adv.mfgFFFF)
                }
                try { Thread.sleep(options.scanIntervalMs) } catch (_: InterruptedException) { return@Thread }
            }
        }, "simradio-scan").apply { isDaemon = true }
        synchronized(scans) { scans.put(callback, t)?.interrupt() }
        t.start()
    }

    override fun stopScan(callback: Radio.ScanListener) {
        synchronized(scans) { scans.remove(callback) }?.interrupt()
    }

    private class Adv(val advertising: Boolean, val name: String, val mfgFFFF: ByteArray?, val uuids: List<UUID>)

    private fun probeAdvertisement(): Adv? {
        Socket().use { s ->
            s.connect(InetSocketAddress(host, port), 300)
            s.soTimeout = 1000
            s.tcpNoDelay = true
            return readAdvertisement(s)
        }
    }

    /** Ask for the advertisement on [s] (no CONNECT sent) and parse it. */
    private fun readAdvertisement(s: Socket): Adv {
        writeFrame(s.getOutputStream(), P_ADV, ByteArray(0))
        val input = DataInputStream(s.getInputStream())
        while (true) {
            val (type, b) = readFrame(input)
            if (type != D_ADV) continue
            var i = 0
            val on = b[i++].toInt() != 0
            val nameLen = b[i++].toInt() and 0xFF
            val name = String(b, i, nameLen, Charsets.UTF_8); i += nameLen
            val mfgLen = b[i++].toInt() and 0xFF
            val mfg = b.copyOfRange(i, i + mfgLen); i += mfgLen
            val n = b[i++].toInt() and 0xFF
            val uuids = (0 until n).map { k -> uuidAt(b, i + 16 * k) }
            // Manufacturer data is [company id LE][payload]; Android hands
            // the app the payload for one company.
            val ffff = if (mfg.size >= 2 && mfg[0] == 0xFF.toByte() && mfg[1] == 0xFF.toByte()) {
                mfg.copyOfRange(2, mfg.size)
            } else {
                null
            }
            return Adv(on, name, ffff, uuids)
        }
    }

    // ---- devices and bonds ---------------------------------------------------------------------

    private fun setBond(address: String, state: Int) {
        val a = address.uppercase()
        synchronized(bonding) {
            if (state == BluetoothDevice.BOND_BONDING) bonding.add(a) else bonding.remove(a)
        }
        when (state) {
            BluetoothDevice.BOND_BONDED -> store.edit().putBoolean(KEY_BOND + a, true).commit()
            BluetoothDevice.BOND_NONE -> store.edit().remove(KEY_BOND + a).commit()
        }
        log("bond $a -> ${bondName(state)}")
        listener?.onBondStateChanged(a, state)
    }

    private fun isBonding(address: String) = synchronized(bonding) { address.uppercase() in bonding }

    private inner class SimDevice(override val address: String) : RadioDevice {
        override val name: String? get() = advName
        override val bondState: Int
            get() = when {
                isBonding(address) -> BluetoothDevice.BOND_BONDING
                holdsBond(address) -> BluetoothDevice.BOND_BONDED
                else -> BluetoothDevice.BOND_NONE
            }

        override fun createBond(): Boolean {
            if (bondState != BluetoothDevice.BOND_NONE) return false
            val g = synchronized(gatts) { gatts.firstOrNull { it.device.address == address && it.isUp } }
                ?: return false
            setBond(address, BluetoothDevice.BOND_BONDING)
            g.secure()
            return true
        }

        override fun removeBond() {
            if (!holdsBond(address)) return
            setBond(address, BluetoothDevice.BOND_NONE)
        }

        override fun connectGatt(autoConnect: Boolean, callback: RadioGattCallback): RadioGatt =
            SimGatt(this, autoConnect, callback).also {
                connectAttempts.incrementAndGet()
                log("connectGatt auto=$autoConnect")
                synchronized(gatts) { gatts.add(it) }
                it.start()
            }

        override fun equals(other: Any?) = other is SimDevice && other.address == address
        override fun hashCode() = address.hashCode()
    }

    // ---- one connection ------------------------------------------------------------------------

    private inner class SimGatt(
        override val device: SimDevice,
        private val autoConnect: Boolean,
        private val cb: RadioGattCallback,
    ) : RadioGatt {
        private val events: ExecutorService = Executors.newSingleThreadExecutor { r ->
            Thread(r, "simradio-cb").apply { isDaemon = true }
        }
        @Volatile private var socket: Socket? = null
        @Volatile private var out: OutputStream? = null
        @Volatile private var closed = false
        @Volatile private var cancelled = false
        @Volatile private var localDisconnect = false
        @Volatile var isUp = false; private set
        @Volatile private var deviceReason: Int? = null
        @Volatile private var encryptionFailed = false
        @Volatile private var sentBonded = false
        private var attMtu = 23
        @Volatile private var services: List<BluetoothGattService> = emptyList()
        private val notifying = HashSet<UUID>()
        private val writeLock = Any()

        /** Every callback goes through one thread, in order; none after close(). */
        private fun emit(what: String, block: () -> Unit) {
            if (closed) return
            runCatching {
                events.execute {
                    if (!closed) {
                        log(what)
                        block()
                    }
                }
            }
        }

        fun start() {
            Thread(::run, "simradio-link").apply { isDaemon = true }.start()
        }

        private fun run() {
            val began = System.currentTimeMillis()
            val s = connectSocket(began) ?: return
            socket = s
            out = s.getOutputStream()
            sentBonded = holdsBond(device.address)
            val flags = (if (sentBonded) 1 else 0) or 2 or 4   // bonded?, pair if asked, confirm passkey
            val name = options.phoneName.toByteArray(Charsets.UTF_8)
            val body = ByteBuffer.allocate(4 + 6 + 1 + name.size).order(ByteOrder.LITTLE_ENDIAN)
                .put(1).putShort(options.mtu.toShort()).put(flags.toByte())
                .put(addressBytes(identity)).put(name.size.toByte()).put(name).array()
            if (!send(P_CONNECT, body)) { lost(); return }
            val input = DataInputStream(s.getInputStream())
            try {
                while (true) {
                    val (type, b) = readFrame(input)
                    frame(type, b)
                }
            } catch (_: IOException) {
            }
            lost()
        }

        /** The socket, or null once the attempt is over (failed, cancelled, closed). */
        private fun connectSocket(began: Long): Socket? {
            while (!closed && !cancelled) {
                if (enabled) {
                    try {
                        val s = Socket()
                        s.tcpNoDelay = true
                        s.connect(InetSocketAddress(host, port), 500)
                        // A central can only reach a device that is advertising
                        // (connectable). The sim also accepts sockets before the
                        // firmware's GATT server exists, and a link made then
                        // isn't one a phone could have made.
                        s.soTimeout = 1000
                        val adv = runCatching { readAdvertisement(s) }.getOrNull()
                        s.soTimeout = 0
                        if (closed || cancelled) { s.close(); return null }
                        if (adv?.advertising == true) {
                            advName = adv.name
                            return s
                        }
                        s.close()
                    } catch (_: IOException) {
                    }
                }
                val elapsed = System.currentTimeMillis() - began
                val limit = if (autoConnect) options.autoConnectFailMs else options.directConnectTimeoutMs
                if (limit in 1..elapsed) {
                    // Never reached the device: no STATE_CONNECTED, just the failure.
                    log("connect failed (auto=$autoConnect) after ${elapsed}ms")
                    emit("state DISCONNECTED status=${options.connectFailStatus} (never connected)") {
                        cb.onConnectionStateChange(this, options.connectFailStatus, BluetoothProfile.STATE_DISCONNECTED)
                    }
                    return null
                }
                try { Thread.sleep(200) } catch (_: InterruptedException) { return null }
            }
            return null
        }

        private fun frame(type: Int, b: ByteArray) {
            when (type) {
                D_CONNECTED -> {
                    // Cancelled while it was coming up: drop it unreported.
                    if (cancelled) { runCatching { socket?.close() }; return }
                    attMtu = u16(b, 2)
                    isUp = true
                    linksUp.incrementAndGet()
                    emit("state CONNECTED mtu=$attMtu bonded=$sentBonded") {
                        cb.onConnectionStateChange(this, BluetoothGatt.GATT_SUCCESS, BluetoothProfile.STATE_CONNECTED)
                    }
                    // Android encrypts a bonded link by itself as it comes up.
                    if (sentBonded) send(P_SECURE, ByteArray(0))
                }
                D_SECURITY -> {
                    val encrypted = b[0].toInt() != 0
                    val bonded = b[1].toInt() != 0
                    log("security encrypted=$encrypted bonded=$bonded")
                    if (isBonding(device.address)) {
                        val ok = encrypted && bonded
                        // Broadcast whether or not this gatt is still open, as Android does.
                        setBond(device.address, if (ok) BluetoothDevice.BOND_BONDED else BluetoothDevice.BOND_NONE)
                    } else if (sentBonded && !encrypted) {
                        // Our keys don't match the device's: Android sees the
                        // encryption fail (MIC failure / key missing).
                        encryptionFailed = true
                    }
                }
                D_DISCOVERED -> {
                    val list = LinkedHashMap<UUID, BluetoothGattService>()
                    var i = 0
                    while (i + 36 <= b.size) {
                        val svc = uuidAt(b, i)
                        val chr = uuidAt(b, i + 16)
                        val props = u32(b, i + 32)
                        val s = list.getOrPut(svc) {
                            BluetoothGattService(svc, BluetoothGattService.SERVICE_TYPE_PRIMARY)
                        }
                        // NimBLE's low property bits are the ATT ones Android uses.
                        val c = BluetoothGattCharacteristic(chr, props and 0xFF, 0)
                        if (props and 0x30 != 0) {
                            c.addDescriptor(BluetoothGattDescriptor(BikeUuid.cccd, 0))
                        }
                        s.addCharacteristic(c)
                        i += 36
                    }
                    services = list.values.toList()
                    emit("services discovered (${list.size})") {
                        cb.onServicesDiscovered(this, BluetoothGatt.GATT_SUCCESS)
                    }
                }
                D_READ_RSP -> {
                    val ch = find(uuidAt(b, 0)) ?: return
                    val st = b[16].toInt() and 0xFF
                    val v = b.copyOfRange(17, b.size)
                    emit("read ${ch.uuid} status=$st") { cb.onCharacteristicRead(this, ch, v, st) }
                }
                D_WRITE_RSP -> {
                    val ch = find(uuidAt(b, 0)) ?: return
                    val st = b[16].toInt() and 0xFF
                    emit("write ${ch.uuid} status=$st") { cb.onCharacteristicWrite(this, ch, st) }
                }
                D_SUBSCRIBE_RSP -> {
                    val ch = find(uuidAt(b, 0)) ?: return
                    val d = ch.getDescriptor(BikeUuid.cccd) ?: return
                    val st = b[16].toInt() and 0xFF
                    emit("cccd ${ch.uuid} status=$st") { cb.onDescriptorWrite(this, d, st) }
                }
                D_NOTIFY -> {
                    val u = uuidAt(b, 0)
                    if (synchronized(notifying) { u !in notifying }) return
                    val ch = find(u) ?: return
                    val v = b.copyOfRange(16, b.size)
                    emit("") { cb.onCharacteristicChanged(this, ch, v) }
                }
                D_DISCONNECTED -> {
                    deviceReason = u16(b, 0)
                    log("device disconnects, reason 0x%02x".format(deviceReason))
                }
                D_PEER_READ -> {
                    // The device reading this phone's GATT server: the GAP name only.
                    val req = b.copyOfRange(0, 4)
                    val chr = uuidAt(b, 20)
                    val gapName = UUID.fromString("00002a00-0000-1000-8000-00805f9b34fb")
                    val rsp = if (chr == gapName) {
                        req + byteArrayOf(0) + options.phoneName.toByteArray(Charsets.UTF_8)
                    } else {
                        req + byteArrayOf(0x0A)
                    }
                    send(P_PEER_READ_RSP, rsp)
                }
                D_PASSKEY_REQ -> log("passkey requested (unexpected: CONNECT asked for auto-confirm)")
                else -> Unit   // SERVICE_CHANGED, ADV, PEER_SUBSCRIBE, PEER_WRITE
            }
        }

        private fun find(u: UUID): BluetoothGattCharacteristic? {
            for (s in services) s.getCharacteristic(u)?.let { return it }
            return null
        }

        /** The socket is gone: report what Android would, once. */
        private fun lost() {
            val wasUp = isUp
            isUp = false
            runCatching { socket?.close() }
            synchronized(gatts) { gatts.remove(this) }
            // Pairing can't finish without the link.
            if (isBonding(device.address)) setBond(device.address, BluetoothDevice.BOND_NONE)
            if (!wasUp) return
            val status = when {
                localDisconnect -> BluetoothGatt.GATT_SUCCESS
                deviceReason != null -> deviceReason!!
                encryptionFailed -> 0x3D
                else -> LINK_LOST
            }
            emit("state DISCONNECTED status=$status") {
                cb.onConnectionStateChange(this, status, BluetoothProfile.STATE_DISCONNECTED)
            }
        }

        /** The radio link went (adapter off): the device never says why. */
        fun linkLost(status: Int) {
            if (!isUp) { cancelled = true; return }
            runCatching { socket?.close() }
        }

        fun secure() { send(P_SECURE, ByteArray(0)) }

        private fun send(type: Int, body: ByteArray): Boolean {
            val o = out ?: return false
            return try {
                synchronized(writeLock) { writeFrame(o, type, body) }
                true
            } catch (_: IOException) {
                false
            }
        }

        // ---- RadioGatt ----

        override fun disconnect() {
            if (!isUp) {
                // A pending connect is cancelled; Android reports nothing.
                cancelled = true
                return
            }
            localDisconnect = true
            send(P_DISCONNECT, byteArrayOf(0x13))
            runCatching { socket?.shutdownOutput() }
        }

        override fun close() {
            closed = true
            cancelled = true
            runCatching { socket?.close() }
            synchronized(gatts) { gatts.remove(this) }
            events.shutdown()
        }

        override fun discoverServices(): Boolean = isUp && send(P_DISCOVER, ByteArray(0))

        override fun getService(uuid: UUID): BluetoothGattService? = services.firstOrNull { it.uuid == uuid }

        override fun requestMtu(mtu: Int): Boolean {
            if (!isUp) return false
            val m = attMtu
            emit("mtu $m") { cb.onMtuChanged(this, m, BluetoothGatt.GATT_SUCCESS) }
            return true
        }

        override fun readCharacteristic(ch: BluetoothGattCharacteristic): Boolean =
            isUp && send(P_READ, uuidBytes(ch.uuid))

        override fun writeCharacteristic(ch: BluetoothGattCharacteristic, value: ByteArray, writeType: Int): Boolean {
            if (!isUp) return false
            val rsp = writeType != BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            if (!send(P_WRITE, uuidBytes(ch.uuid) + byteArrayOf(if (rsp) 1 else 0) + value)) return false
            // Without response the stack completes the write once it has taken it.
            if (!rsp) emit("") { cb.onCharacteristicWrite(this, ch, BluetoothGatt.GATT_SUCCESS) }
            return true
        }

        override fun setNotify(ch: BluetoothGattCharacteristic, enable: Boolean): Boolean {
            if (!isUp) return false
            synchronized(notifying) { if (enable) notifying.add(ch.uuid) else notifying.remove(ch.uuid) }
            ch.getDescriptor(BikeUuid.cccd) ?: return false
            val cccd = if (enable) 1 else 0
            return send(P_SUBSCRIBE, uuidBytes(ch.uuid) + byteArrayOf(cccd.toByte(), 0))
        }

        override fun requestConnectionPriority(priority: Int): Boolean = isUp
    }

    private fun log(s: String) { if (s.isNotEmpty()) options.log("[simradio $identity] $s") }

    companion object {
        private const val KEY_ID = "identity"
        private const val KEY_BOND = "bond:"

        /** HCI 0x08, connection (supervision) timeout: a link that just went. */
        const val LINK_LOST = 0x08

        private const val P_CONNECT = 0x01
        private const val P_DISCOVER = 0x02
        private const val P_READ = 0x03
        private const val P_WRITE = 0x04
        private const val P_SUBSCRIBE = 0x05
        private const val P_SECURE = 0x06
        private const val P_DISCONNECT = 0x07
        private const val P_ADV = 0x0A
        private const val P_PEER_READ_RSP = 0x10
        private const val D_CONNECTED = 0x81
        private const val D_DISCOVERED = 0x82
        private const val D_READ_RSP = 0x83
        private const val D_WRITE_RSP = 0x84
        private const val D_SUBSCRIBE_RSP = 0x85
        private const val D_NOTIFY = 0x86
        private const val D_DISCONNECTED = 0x87
        private const val D_PASSKEY_REQ = 0x88
        private const val D_SECURITY = 0x8A
        private const val D_ADV = 0x8B
        private const val D_PEER_READ = 0x90

        private fun bondName(s: Int) = when (s) {
            BluetoothDevice.BOND_BONDED -> "BONDED"
            BluetoothDevice.BOND_BONDING -> "BONDING"
            else -> "NONE"
        }

        private fun randomIdentity(): String {
            val r = java.security.SecureRandom()
            val b = ByteArray(6).also(r::nextBytes)
            b[0] = (b[0].toInt() or 0xC0).toByte()   // static random address
            return b.joinToString(":") { "%02X".format(it) }
        }

        fun addressBytes(a: String): ByteArray = a.split(":").map { it.toInt(16).toByte() }.toByteArray()

        fun writeFrame(o: OutputStream, type: Int, body: ByteArray) {
            val len = body.size + 1
            val f = ByteArray(3 + body.size)
            f[0] = (len and 0xFF).toByte()
            f[1] = ((len shr 8) and 0xFF).toByte()
            f[2] = type.toByte()
            System.arraycopy(body, 0, f, 3, body.size)
            o.write(f)
            o.flush()
        }

        fun readFrame(input: DataInputStream): Pair<Int, ByteArray> {
            val lo = input.read()
            val hi = input.read()
            if (lo < 0 || hi < 0) throw EOFException()
            val len = lo or (hi shl 8)
            if (len == 0) return readFrame(input)
            val type = input.readUnsignedByte()
            val body = ByteArray(len - 1)
            input.readFully(body)
            return type to body
        }

        /** UUIDs go over simble as 16 bytes in string order. */
        fun uuidBytes(u: UUID): ByteArray =
            ByteBuffer.allocate(16).putLong(u.mostSignificantBits).putLong(u.leastSignificantBits).array()

        fun uuidAt(b: ByteArray, i: Int): UUID {
            val bb = ByteBuffer.wrap(b, i, 16)
            return UUID(bb.long, bb.long)
        }

        private fun u16(b: ByteArray, i: Int) = (b[i].toInt() and 0xFF) or ((b[i + 1].toInt() and 0xFF) shl 8)
        private fun u32(b: ByteArray, i: Int) = u16(b, i) or (u16(b, i + 2) shl 16)
    }
}

/** BluetoothAdapter.checkBluetoothAddress without the adapter. */
private object BluetoothAdapterAddress {
    private val re = Regex("^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$")
    fun valid(a: String) = re.matches(a)
}
