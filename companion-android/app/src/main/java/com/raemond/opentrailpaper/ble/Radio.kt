package com.raemond.opentrailpaper.ble

import android.annotation.SuppressLint
import android.app.Application
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.ParcelUuid
import java.util.UUID

/**
 * The radio as BleManager sees it: the adapter, scanning, devices and their
 * bonds, and one GATT client connection.
 *
 * It exists so the connection state machine in BleManager — connect, bond,
 * refusal, barren connects, reconnect — runs unchanged against something
 * other than Android's stack: [AndroidRadio] is BluetoothAdapter/BluetoothGatt
 * one-for-one, and the debug-only SimRadio speaks the firmware simulator's
 * simble protocol (sim/PROTOCOL.md) while reporting what Android would — the
 * same callbacks, the same status codes. Anything BleManager decides from a
 * status code it decides here exactly as on a phone.
 *
 * Characteristics and services stay android.bluetooth objects: they are plain
 * value holders any implementation can build.
 */
interface Radio {
    /** There is a Bluetooth adapter at all (false: UNAVAILABLE). */
    val available: Boolean
    val isEnabled: Boolean
    /** A scan could start now (Android: the adapter has an LE scanner, i.e. it is on). */
    val canScan: Boolean

    /** Adapter on/off and bond-state changes, delivered on any thread. */
    fun setListener(listener: Listener)

    /** The device at [address] — any valid address, seen or not (getRemoteDevice). */
    fun remoteDevice(address: String): RadioDevice?

    /** Scan for devices advertising [service]. */
    fun startScan(service: UUID, callback: ScanListener)
    fun stopScan(callback: ScanListener)

    interface Listener {
        fun onAdapterStateChanged()
        /** [bond] is a BluetoothDevice.BOND_* value. */
        fun onBondStateChanged(address: String?, bond: Int)
    }

    interface ScanListener {
        /** [mfgFFFF]: manufacturer data for company 0xFFFF, after the company id. */
        fun onResult(device: RadioDevice, mfgFFFF: ByteArray?)
        fun onFailed(errorCode: Int)
    }
}

interface RadioDevice {
    val address: String
    val name: String?
    /** BluetoothDevice.BOND_NONE / BOND_BONDING / BOND_BONDED. */
    val bondState: Int
    fun createBond(): Boolean
    fun removeBond()
    fun connectGatt(autoConnect: Boolean, callback: RadioGattCallback): RadioGatt?
}

/** One GATT client connection (BluetoothGatt). Every op answers through [RadioGattCallback]. */
interface RadioGatt {
    val device: RadioDevice
    fun disconnect()
    fun close()
    fun discoverServices(): Boolean
    fun getService(uuid: UUID): BluetoothGattService?
    fun requestMtu(mtu: Int): Boolean
    fun readCharacteristic(ch: BluetoothGattCharacteristic): Boolean
    /** [writeType]: BluetoothGattCharacteristic.WRITE_TYPE_*. */
    fun writeCharacteristic(ch: BluetoothGattCharacteristic, value: ByteArray, writeType: Int): Boolean
    /** Route notifications locally and write the CCCD; answers with onDescriptorWrite. */
    fun setNotify(ch: BluetoothGattCharacteristic, enable: Boolean): Boolean
    fun requestConnectionPriority(priority: Int): Boolean
}

/** BluetoothGattCallback's shape, with statuses exactly as Android reports them. */
interface RadioGattCallback {
    fun onConnectionStateChange(g: RadioGatt, status: Int, newState: Int)
    fun onMtuChanged(g: RadioGatt, mtu: Int, status: Int)
    fun onServicesDiscovered(g: RadioGatt, status: Int)
    fun onCharacteristicWrite(g: RadioGatt, ch: BluetoothGattCharacteristic, status: Int)
    fun onDescriptorWrite(g: RadioGatt, d: BluetoothGattDescriptor, status: Int)
    fun onCharacteristicRead(g: RadioGatt, ch: BluetoothGattCharacteristic, value: ByteArray, status: Int)
    fun onCharacteristicChanged(g: RadioGatt, ch: BluetoothGattCharacteristic, value: ByteArray)
    /** The device's GATT table changed (Service Changed; Android 12+ reports it). */
    fun onServiceChanged(g: RadioGatt) {}
}

// ---- Android's stack -----------------------------------------------------------------------

/** The real radio: a thin pass-through to BluetoothAdapter / BluetoothGatt. */
@SuppressLint("MissingPermission")
class AndroidRadio(private val app: Application) : Radio {
    private val btManager = app.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager?
    private val adapter: BluetoothAdapter? get() = btManager?.adapter
    private val scans = HashMap<Radio.ScanListener, ScanCallback>()

    override val available: Boolean get() = adapter != null
    override val isEnabled: Boolean get() = adapter?.isEnabled == true
    override val canScan: Boolean get() = adapter?.bluetoothLeScanner != null

    override fun setListener(listener: Radio.Listener) {
        app.registerReceiver(
            object : BroadcastReceiver() {
                override fun onReceive(context: Context?, intent: Intent?) {
                    if (intent?.action != BluetoothAdapter.ACTION_STATE_CHANGED) return
                    listener.onAdapterStateChanged()
                }
            },
            IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED),
        )
        app.registerReceiver(
            object : BroadcastReceiver() {
                override fun onReceive(context: Context?, intent: Intent?) {
                    if (intent?.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return
                    val dev: BluetoothDevice? = if (Build.VERSION.SDK_INT >= 33) {
                        intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                    } else {
                        @Suppress("DEPRECATION")
                        intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
                    }
                    val bond = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR)
                    listener.onBondStateChanged(dev?.address, bond)
                }
            },
            IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED),
        )
    }

    override fun remoteDevice(address: String): RadioDevice? =
        runCatching { adapter?.getRemoteDevice(address) }.getOrNull()?.let { AndroidDevice(app, it) }

    override fun startScan(service: UUID, callback: Radio.ScanListener) {
        val scanner = adapter?.bluetoothLeScanner ?: return
        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                callback.onResult(
                    AndroidDevice(app, result.device),
                    result.scanRecord?.getManufacturerSpecificData(0xFFFF),
                )
            }

            override fun onScanFailed(errorCode: Int) = callback.onFailed(errorCode)
        }
        synchronized(scans) { scans[callback] = cb }
        scanner.startScan(
            listOf(ScanFilter.Builder().setServiceUuid(ParcelUuid(service)).build()),
            ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build(),
            cb,
        )
    }

    override fun stopScan(callback: Radio.ScanListener) {
        val cb = synchronized(scans) { scans.remove(callback) } ?: return
        adapter?.bluetoothLeScanner?.stopScan(cb)
    }
}

@SuppressLint("MissingPermission")
private class AndroidDevice(private val app: Application, val dev: BluetoothDevice) : RadioDevice {
    override val address: String get() = dev.address
    override val name: String? get() = dev.name
    override val bondState: Int get() = dev.bondState
    override fun createBond(): Boolean = dev.createBond()

    /** BluetoothDevice.removeBond() is hidden API but has been stable since
     *  API 19 and is what every BLE accessory app uses; best effort. */
    override fun removeBond() {
        runCatching { dev.javaClass.getMethod("removeBond").invoke(dev) }
    }

    override fun connectGatt(autoConnect: Boolean, callback: RadioGattCallback): RadioGatt? {
        val wrapper = AndroidGatt(this)
        val inner = dev.connectGatt(app, autoConnect, wrapper.adapt(callback), BluetoothDevice.TRANSPORT_LE)
            ?: return null
        wrapper.bind(inner)
        return wrapper
    }
}

/**
 * One BluetoothGatt. Callbacks name the wrapper, so BleManager's identity
 * checks (`gatt === g`) hold exactly as they did on the BluetoothGatt.
 */
@SuppressLint("MissingPermission")
private class AndroidGatt(override val device: AndroidDevice) : RadioGatt {
    @Volatile private var gatt: BluetoothGatt? = null

    fun bind(g: BluetoothGatt) { if (gatt == null) gatt = g }

    override fun disconnect() { gatt?.disconnect() }
    override fun close() { gatt?.close() }
    override fun discoverServices(): Boolean = gatt?.discoverServices() == true
    override fun getService(uuid: UUID): BluetoothGattService? = gatt?.getService(uuid)
    override fun requestMtu(mtu: Int): Boolean = gatt?.requestMtu(mtu) == true
    override fun readCharacteristic(ch: BluetoothGattCharacteristic): Boolean =
        gatt?.readCharacteristic(ch) == true
    override fun requestConnectionPriority(priority: Int): Boolean =
        gatt?.requestConnectionPriority(priority) == true

    @Suppress("DEPRECATION")
    override fun writeCharacteristic(ch: BluetoothGattCharacteristic, value: ByteArray, writeType: Int): Boolean {
        val g = gatt ?: return false
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(ch, value, writeType) == BluetoothGatt.GATT_SUCCESS
        } else {
            ch.writeType = writeType
            ch.value = value
            g.writeCharacteristic(ch)
        }
    }

    @Suppress("DEPRECATION")
    override fun setNotify(ch: BluetoothGattCharacteristic, enable: Boolean): Boolean {
        val g = gatt ?: return false
        if (!g.setCharacteristicNotification(ch, enable)) return false
        // The local flag above only routes callbacks; the device is only told to
        // send anything once its CCCD is written, and THAT write is what this
        // operation waits on.
        val cccd = ch.getDescriptor(BikeUuid.cccd) ?: return false
        val value = if (enable) {
            BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        } else {
            BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
        }
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeDescriptor(cccd, value) == BluetoothGatt.GATT_SUCCESS
        } else {
            cccd.value = value
            g.writeDescriptor(cccd)
        }
    }

    /** The BluetoothGattCallback for this connection, forwarding to [cb]. */
    fun adapt(cb: RadioGattCallback): BluetoothGattCallback = object : BluetoothGattCallback() {
        private fun w(g: BluetoothGatt): RadioGatt { bind(g); return this@AndroidGatt }

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) =
            cb.onConnectionStateChange(w(g), status, newState)

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) = cb.onMtuChanged(w(g), mtu, status)

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) = cb.onServicesDiscovered(w(g), status)

        override fun onCharacteristicWrite(g: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int) =
            cb.onCharacteristicWrite(w(g), ch, status)

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) =
            cb.onDescriptorWrite(w(g), d, status)

        // Pre-33 stacks call the 3-argument read/changed callbacks, 33+ the
        // 4-argument ones (whose default implementations call the old ones),
        // so both are overridden, as BleManager always did.
        @Suppress("DEPRECATION")
        override fun onCharacteristicRead(g: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int) =
            cb.onCharacteristicRead(w(g), ch, ch.value?.copyOf() ?: ByteArray(0), status)

        override fun onCharacteristicRead(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int,
        ) = cb.onCharacteristicRead(w(g), ch, value.copyOf(), status)

        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            val value = ch.value?.copyOf() ?: return
            cb.onCharacteristicChanged(w(g), ch, value)
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray) =
            cb.onCharacteristicChanged(w(g), ch, value.copyOf())

        override fun onServiceChanged(g: BluetoothGatt) = cb.onServiceChanged(w(g))
    }
}
