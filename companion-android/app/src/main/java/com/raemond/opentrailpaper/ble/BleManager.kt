package com.raemond.opentrailpaper.ble

import android.Manifest
import android.annotation.SuppressLint
import android.app.Application
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.core.content.ContextCompat
import com.raemond.opentrailpaper.data.DashLayout
import com.raemond.opentrailpaper.data.FirmwareRelease
import com.raemond.opentrailpaper.data.Prefs
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.io.File
import java.security.MessageDigest
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.roundToLong

/**
 * The link to the head unit — a port of companion-ios/Sources/BLEManager.swift.
 *
 * State is exposed as Compose [androidx.compose.runtime.MutableState] rather
 * than flows, which is the closest analogue of the iOS `@Published` fields the
 * views were written against: a screen reads `ble.status` and recomposes when
 * the device pushes a new one, with no subscription to wire up.
 *
 * Every GATT callback lands on a binder thread, so each one hops to the main
 * thread before touching that state. See [GattQueue] for why writes are
 * serialized rather than fired freely as they are on iOS.
 */
@SuppressLint("MissingPermission")
class BleManager(private val app: Application) {

    enum class ConnState { IDLE, SCANNING, CONNECTING, CONNECTED, POWERED_OFF }

    /** Explicit phases so the UI can show exactly what's happening. */
    enum class OtaPhase { IDLE, SENDING, SAVING, INSTALLING, VERIFYING, DONE, FAILED }

    // MARK: published state

    var state by mutableStateOf(ConnState.IDLE); private set
    var status by mutableStateOf(DeviceStatus()); private set

    var ftpWatts by mutableStateOf(250); private set
    var tzMinutes by mutableStateOf(-420); private set
    var backlight by mutableStateOf(2); private set     // 0 off .. 3 bright (mirrors device)
    var clock24h by mutableStateOf(true); private set   // device status-bar clock format
    var usbDrive by mutableStateOf(true); private set   // expose device SD as a USB drive
    var useMiles by mutableStateOf(false); private set  // mirrored into Prefs

    var lastUploadProgress by mutableStateOf<Double?>(null); private set  // 0..1 while sending
    var routeSent by mutableStateOf(false)                               // writes were queued
    var routeReceived by mutableStateOf(false)                           // device confirmed
    var lastMessage by mutableStateOf<String?>(null)

    // Permission state, surfaced so onboarding can reflect what's been granted.
    var locationAuthorized by mutableStateOf(false); private set
    var bluetoothReady by mutableStateOf(false); private set

    var locationPermission by mutableStateOf(PermissionState.NOT_DETERMINED); private set
    var bluetoothPermission by mutableStateOf(PermissionState.NOT_DETERMINED); private set
    var bluetoothPoweredOn by mutableStateOf(false); private set

    // Ride download
    var rides by mutableStateOf<List<RideFile>>(emptyList()); private set
    var loadingRides by mutableStateOf(false); private set
    var downloadingName by mutableStateOf<String?>(null); private set
    var downloadProgress by mutableStateOf(0.0); private set
    var downloadedFile by mutableStateOf<File?>(null)

    // Firmware / OTA
    var deviceFirmware by mutableStateOf(""); private set
    var otaInProgress by mutableStateOf(false); private set
    var otaPhase by mutableStateOf(OtaPhase.IDLE); private set
    var otaProgress by mutableStateOf(0.0); private set
    var otaMessage by mutableStateOf<String?>(null); private set

    var logFile by mutableStateOf<File?>(null)
    var deviceLogs by mutableStateOf<List<LogFile>>(emptyList()); private set
    var loadingLogs by mutableStateOf(false); private set
    var downloadingLog by mutableStateOf(false); private set

    // Saved routes on the device
    var deviceRoutes by mutableStateOf<List<String>>(emptyList()); private set
    var loadingRoutes by mutableStateOf(false); private set

    // Cycling sensors known to the head unit (managed from the app)
    var sensors by mutableStateOf<List<BikeSensor>>(emptyList()); private set
    var scanningSensors by mutableStateOf(false); private set

    // Vector-map upload (phone -> device)
    var mapUploading by mutableStateOf(false); private set
    var mapProgress by mutableStateOf(0.0); private set
    var mapMessage by mutableStateOf<String?>(null); private set
    var deviceMaps by mutableStateOf<List<DeviceMap>>(emptyList()); private set

    // H3 tile streaming (many small tiles sent one at a time over CHR_MAP)
    var tilesUploading by mutableStateOf(false); private set
    var tilesTotal by mutableStateOf(0); private set
    var tilesDone by mutableStateOf(0); private set
    var tileMessage by mutableStateOf<String?>(null); private set
    var deviceTileIds by mutableStateOf<Set<String>>(emptySet()); private set

    /**
     * The device's dashboard layout, as the text of /config/dashboard.cfg.
     * null until the device has been read, which is how the editor knows to show
     * "connect to edit" rather than an invented default that would overwrite the
     * rider's real one the moment they touched a control.
     */
    var dashLayout by mutableStateOf<DashLayout?>(null); private set

    /** Whether a transfer wants the screen kept awake; MainActivity applies it. */
    var keepScreenOn by mutableStateOf(false); private set

    var lastAidingSent by mutableStateOf<Long?>(null); private set

    // MARK: plumbing

    private val main = Handler(Looper.getMainLooper())
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    private val btManager =
        app.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager?
    private val adapter: BluetoothAdapter? get() = btManager?.adapter

    private var gatt: BluetoothGatt? = null
    private var device: BluetoothDevice? = null
    private val queue = GattQueue()

    private var settingsChar: BluetoothGattCharacteristic? = null
    private var statusChar: BluetoothGattCharacteristic? = null
    private var routeChar: BluetoothGattCharacteristic? = null
    private var ridesChar: BluetoothGattCharacteristic? = null
    private var otaChar: BluetoothGattCharacteristic? = null
    private var sensorsChar: BluetoothGattCharacteristic? = null
    private var mapChar: BluetoothGattCharacteristic? = null
    private var dashChar: BluetoothGattCharacteristic? = null

    /** Negotiated ATT payload; one byte of every packet is the opcode. */
    private var mtu = 23
    private val maxPayload: Int get() = max(20, mtu - 3)
    private val chunkSize: Int get() = maxPayload - 1

    private var scanning = false

    // Transfer state
    private var otaData: ByteArray = ByteArray(0)
    private var otaSentBytes = 0
    private var otaCommitSent = false
    private var otaTargetVersion = ""
    private var otaWatchdog: Job? = null

    private var mapData: ByteArray = ByteArray(0)
    private var mapSentBytes = 0
    private var mapEndSent = false

    private var tileQueue = ArrayDeque<Pair<String, ByteArray>>()
    private var currentTileId: String? = null
    private var tileJobFailed = false
    private var tilesMoreComing = false
    private var tileIdsBuilding = mutableListOf<String>()

    private var deviceMapsBuilding = mutableListOf<DeviceMap>()
    private var sensorsBuilding = mutableListOf<BikeSensor>()
    private var logsBuilding = mutableListOf<LogFile>()

    // Download reassembly
    private var dlBuffer = java.io.ByteArrayOutputStream()
    private var dlExpected = 0
    private var dlName = ""
    private var dlNextSeq = 0

    // Location aiding
    private val locationManager =
        app.getSystemService(Context.LOCATION_SERVICE) as LocationManager?
    private var wantsAiding = false
    private var sawStatusSinceConnect = false
    private var fixStableJob: Job? = null

    /** Most recent phone fix, so the map screens can frame the user. */
    var lastLocation by mutableStateOf<Location?>(null); private set

    private val adapterReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != BluetoothAdapter.ACTION_STATE_CHANGED) return
            main.post { onAdapterStateChanged() }
        }
    }

    init {
        useMiles = Prefs.useMiles
        // Show last-known on-device tiles immediately; a refresh confirms them.
        deviceTileIds = Prefs.deviceTileIds
        refreshPermissions()
        app.registerReceiver(
            adapterReceiver,
            IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED),
        )
        // Returning users get Bluetooth up immediately so the device auto-
        // connects. First-run users start it from the tutorial's Bluetooth step,
        // so the system prompt lands on that screen.
        if (Prefs.onboarded) startCentral()
    }

    private fun onAdapterStateChanged() {
        val on = adapter?.isEnabled == true
        bluetoothPoweredOn = on
        bluetoothReady = on && bluetoothPermission.isGranted
        if (on) {
            if (bluetoothPermission.isGranted && gatt == null) startScan()
        } else {
            state = ConnState.POWERED_OFF
            teardownConnection(reconnect = false)
        }
    }

    // MARK: permissions

    private fun granted(perm: String) =
        ContextCompat.checkSelfPermission(app, perm) == PackageManager.PERMISSION_GRANTED

    /**
     * Re-read both permissions. Called when the app comes back to the
     * foreground, because the user may have just changed them in system Settings
     * and nothing reports that while we're backgrounded.
     *
     * [permanentlyDenied] comes from the Activity: Android, unlike iOS, has no
     * API that distinguishes "never asked" from "asked and refused forever", so
     * the caller pairs a remembered "we have asked" flag with
     * `shouldShowRequestPermissionRationale`. Getting this wrong is what turns a
     * fixable refusal into a screen with no way forward.
     */
    fun refreshPermissions(permanentlyDenied: Set<String> = emptySet()) {
        val fine = Manifest.permission.ACCESS_FINE_LOCATION
        locationAuthorized = granted(fine)
        locationPermission = when {
            locationAuthorized -> PermissionState.GRANTED
            fine in permanentlyDenied -> PermissionState.DENIED
            else -> PermissionState.NOT_DETERMINED
        }

        bluetoothPermission = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val connect = Manifest.permission.BLUETOOTH_CONNECT
            val scan = Manifest.permission.BLUETOOTH_SCAN
            when {
                granted(connect) && granted(scan) -> PermissionState.GRANTED
                connect in permanentlyDenied || scan in permanentlyDenied -> PermissionState.DENIED
                else -> PermissionState.NOT_DETERMINED
            }
        } else {
            // Pre-12 the Bluetooth permissions are install-time, but a scan is
            // refused without location — so that IS the Bluetooth permission
            // here, and reporting it as granted would leave a scan that silently
            // returns nothing with nothing on screen to explain it.
            if (adapter == null) PermissionState.UNAVAILABLE else locationPermission
        }

        bluetoothPoweredOn = adapter?.isEnabled == true
        bluetoothReady = bluetoothPoweredOn && bluetoothPermission.isGranted
        // Permission granted while we were away: bring the radio up now, so the
        // device connects without the user having to hunt for a button.
        if (bluetoothPermission.isGranted) startCentral()
    }

    /** Onboarding / Settings: bring Bluetooth up once it's allowed. */
    fun startCentral() {
        if (!bluetoothPermission.isGranted || adapter?.isEnabled != true) return
        if (gatt == null && !scanning) startScan()
    }

    // MARK: scanning + connection

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            main.post {
                if (gatt == null) {
                    stopScan()
                    connect(result.device)
                }
            }
        }

        override fun onScanFailed(errorCode: Int) {
            main.post {
                scanning = false
                state = ConnState.IDLE
            }
        }
    }

    fun startScan() {
        val scanner = adapter?.bluetoothLeScanner ?: return
        if (!bluetoothPermission.isGranted || adapter?.isEnabled != true) return
        if (scanning || gatt != null) return
        scanning = true
        state = ConnState.SCANNING
        scanner.startScan(
            listOf(ScanFilter.Builder().setServiceUuid(ParcelUuid(BikeUuid.service)).build()),
            ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build(),
            scanCallback,
        )
    }

    private fun stopScan() {
        if (!scanning) return
        scanning = false
        runCatching { adapter?.bluetoothLeScanner?.stopScan(scanCallback) }
    }

    private fun connect(target: BluetoothDevice) {
        device = target
        state = ConnState.CONNECTING
        gatt = target.connectGatt(app, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        queue.attach(gatt)
    }

    fun disconnect() {
        gatt?.disconnect()
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, statusCode: Int, newState: Int) {
            main.post {
                when (newState) {
                    BluetoothProfile.STATE_CONNECTED -> {
                        // Bigger ATT payload first: every transfer's chunk size
                        // is derived from it, and negotiating after the pumps
                        // start would change the size mid-stream.
                        queue.attach(g)
                        queue.enqueue(GattQueue.Op.Mtu(517))
                        g.discoverServices()
                    }

                    BluetoothProfile.STATE_DISCONNECTED -> handleDisconnect()
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, statusCode: Int) {
            main.post {
                if (statusCode == BluetoothGatt.GATT_SUCCESS) mtu = newMtu
                queue.complete(statusCode == BluetoothGatt.GATT_SUCCESS)
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, statusCode: Int) {
            main.post { bindCharacteristics(g) }
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic,
            statusCode: Int,
        ) {
            main.post { queue.complete(statusCode == BluetoothGatt.GATT_SUCCESS) }
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt,
            d: android.bluetooth.BluetoothGattDescriptor,
            statusCode: Int,
        ) {
            main.post {
                queue.complete(statusCode == BluetoothGatt.GATT_SUCCESS)
                onNotifyEnabled(d.characteristic.uuid)
            }
        }

        @Suppress("DEPRECATION")
        override fun onCharacteristicRead(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic,
            statusCode: Int,
        ) {
            val value = ch.value?.copyOf() ?: ByteArray(0)
            main.post {
                queue.complete(statusCode == BluetoothGatt.GATT_SUCCESS)
                if (statusCode == BluetoothGatt.GATT_SUCCESS) dispatch(ch.uuid, value)
            }
        }

        override fun onCharacteristicRead(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic,
            value: ByteArray,
            statusCode: Int,
        ) {
            val copy = value.copyOf()
            main.post {
                queue.complete(statusCode == BluetoothGatt.GATT_SUCCESS)
                if (statusCode == BluetoothGatt.GATT_SUCCESS) dispatch(ch.uuid, copy)
            }
        }

        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            val value = ch.value?.copyOf() ?: return
            main.post { dispatch(ch.uuid, value) }
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            val copy = value.copyOf()
            main.post { dispatch(ch.uuid, copy) }
        }
    }

    private fun bindCharacteristics(g: BluetoothGatt) {
        val service = g.getService(BikeUuid.service) ?: run {
            state = ConnState.IDLE
            return
        }
        settingsChar = service.getCharacteristic(BikeUuid.settings)
        statusChar = service.getCharacteristic(BikeUuid.status)
        routeChar = service.getCharacteristic(BikeUuid.route)
        ridesChar = service.getCharacteristic(BikeUuid.rides)
        otaChar = service.getCharacteristic(BikeUuid.ota)
        sensorsChar = service.getCharacteristic(BikeUuid.sensors)
        mapChar = service.getCharacteristic(BikeUuid.map)
        dashChar = service.getCharacteristic(BikeUuid.dash)

        // Subscribe to everything that notifies, then read the two the device
        // holds authoritative copies of.
        listOfNotNull(
            settingsChar, statusChar, routeChar, ridesChar,
            otaChar, sensorsChar, mapChar, dashChar,
        ).forEach { queue.enqueue(GattQueue.Op.Notify(it, true)) }

        settingsChar?.let { queue.enqueue(GattQueue.Op.Read(it)) }
        dashChar?.let { queue.enqueue(GattQueue.Op.Read(it)) }

        state = ConnState.CONNECTED
        startLocationStream()   // warm-start + live fallback position
    }

    /**
     * The version and sensor replies come back as NOTIFICATIONS, so they can
     * only be asked for once that characteristic's CCCD write has landed —
     * otherwise the reply is dropped and the app never learns the firmware
     * version. Same ordering constraint as iOS's didUpdateNotificationStateFor.
     */
    private fun onNotifyEnabled(uuid: java.util.UUID) {
        when (uuid) {
            BikeUuid.ota -> queryDeviceFirmware()
            BikeUuid.sensors -> refreshSensors()   // one snapshot
        }
    }

    private fun dispatch(uuid: java.util.UUID, data: ByteArray) {
        when (uuid) {
            BikeUuid.status -> parseStatus(data)
            BikeUuid.settings -> parseSettings(data)
            BikeUuid.rides -> handleRidesNotify(data)
            BikeUuid.route -> handleRouteNotify(data)
            BikeUuid.ota -> handleOtaNotify(data)
            BikeUuid.sensors -> handleSensorsNotify(data)
            BikeUuid.map -> handleMapNotify(data)
            BikeUuid.dash -> dashLayout = DashLayout.parse(String(data, Charsets.UTF_8))
        }
    }

    private fun handleDisconnect() {
        settingsChar = null; statusChar = null; routeChar = null; ridesChar = null
        sensorsChar = null; mapChar = null; otaChar = null; dashChar = null
        stopLocationStream()   // no device to send the phone's position to

        // If we drop mid-update: after the data is sent + commit requested, a
        // disconnect is EXPECTED (the device reboots into the new firmware) —
        // show "installing" and let the reconnect confirm the version. Dropping
        // mid-transfer means nothing was flashed.
        if (otaInProgress) {
            keepAwake(false)
            if (otaCommitSent) {
                otaPhase = OtaPhase.INSTALLING
                otaMessage = "Installing — the device is restarting…"
                armOtaWatchdog(150, INSTALL_WATCHDOG_MSG)
            } else {
                otaFinish(
                    OtaPhase.FAILED,
                    "Connection dropped mid-transfer. The device is unchanged — try again.",
                )
            }
        }
        if (mapUploading && currentTileId == null) {
            mapUploading = false
            keepAwake(false)
            mapMessage = "Upload interrupted — try again"
        }
        if (tilesUploading) {
            tileQueue.clear()
            finishTileJob("Interrupted — reconnect to resume")
        }

        status = DeviceStatus()
        sawStatusSinceConnect = false
        rides = emptyList(); loadingRides = false; downloadingName = null
        deviceRoutes = emptyList(); loadingRoutes = false
        lastUploadProgress = null; routeSent = false; routeReceived = false
        sensors = emptyList(); scanningSensors = false

        teardownConnection(reconnect = true)
    }

    private fun teardownConnection(reconnect: Boolean) {
        queue.clear()
        queue.attach(null)
        gatt?.close()
        gatt = null
        if (!reconnect || adapter?.isEnabled != true || !bluetoothPermission.isGranted) {
            state = if (adapter?.isEnabled == true) ConnState.IDLE else ConnState.POWERED_OFF
            return
        }
        // Hand the device straight back to the stack instead of dropping it and
        // re-scanning. autoConnect has no timeout — Android reconnects the
        // instant the head unit is reachable again, without a
        // scan/discover/connect round trip. Re-scanning meant every brief drop
        // cost seconds and a visible "connecting…" flap, and made log downloads
        // and map sends fail: they abort on disconnect and the retry raced the
        // rescan.
        val known = device
        if (known != null) {
            state = ConnState.CONNECTING
            gatt = known.connectGatt(app, true, gattCallback, BluetoothDevice.TRANSPORT_LE)
            queue.attach(gatt)
        } else {
            startScan()
        }
    }

    private fun writeChar(
        ch: BluetoothGattCharacteristic?,
        value: ByteArray,
        withResponse: Boolean = true,
        onWritten: ((Boolean) -> Unit)? = null,
    ) {
        val c = ch ?: return
        queue.enqueue(
            GattQueue.Op.Write(
                c,
                value,
                if (withResponse) {
                    BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                } else {
                    BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                },
                onWritten,
            ),
        )
    }

    // MARK: settings

    // Auto-sync: any app-side settings edit goes through these, which update the
    // mirror and immediately push to the device. Values arriving FROM the device
    // (parseSettings) set the fields directly, so they never echo back.
    //
    // Named `updateX` rather than `setX`: a `var x` already compiles to a JVM
    // `setX`, so the obvious name collides with the property's own setter.
    fun updateFtp(v: Int) { ftpWatts = v; pushSettings() }
    fun updateTz(v: Int) { tzMinutes = v; pushSettings() }
    fun updateBacklight(v: Int) { backlight = v; pushSettings() }
    fun updateClock24h(v: Boolean) { clock24h = v; pushSettings() }
    fun updateUsbDrive(v: Boolean) { usbDrive = v; pushSettings() }

    fun updateUseMiles(v: Boolean) {
        useMiles = v
        Prefs.useMiles = v
        pushSettings()
    }

    fun pushSettings() {
        val out = Packet(8)
        out.i16(ftpWatts)
        out.i16(tzMinutes)
        out.u8(if (useMiles) 1 else 0)
        out.u8(backlight.coerceIn(0, 255))
        out.u8(if (clock24h) 1 else 0)
        out.u8(if (usbDrive) 1 else 0)
        writeChar(settingsChar, out.bytes())
    }

    // Values pushed from the device (on connect, or when edited on the unit).
    private fun parseSettings(d: ByteArray) {
        if (d.size < 4) return
        ftpWatts = le16(d, 0).toShort().toInt()
        tzMinutes = le16(d, 2).toShort().toInt()
        if (d.size >= 6) {
            useMiles = d[4].toInt() != 0
            Prefs.useMiles = useMiles
            backlight = d[5].toInt() and 0xFF
        }
        if (d.size >= 7) clock24h = d[6].toInt() != 0
        if (d.size >= 8) usbDrive = d[7].toInt() != 0
    }

    private fun parseStatus(d: ByteArray) {
        if (d.size < 10) return
        val flags = d[0].toInt() and 0xFF
        val power = le16(d, 4)
        status = DeviceStatus(
            gpsFix = flags and 1 != 0,
            recording = flags and 2 != 0,
            hasRoute = flags and 4 != 0,
            battery = d[1].toInt() and 0xFF,
            sats = d[2].toInt() and 0xFF,
            heartRate = (d[3].toInt() and 0xFF).takeIf { it != 0xFF },
            power = power.takeIf { it != 0 },
            speedKmh = le16(d, 6) / 10.0,
            remainingKm = le16(d, 8) / 10.0,
        )
        syncLocationStreamToDeviceFix()
        updateBackgroundLocationMode()
    }

    // MARK: dashboard layout

    /**
     * Push a layout to the device. It writes the file, applies it to the panel,
     * and notifies back what it actually stored — so a rejected layout corrects
     * the editor instead of leaving it out of step.
     */
    fun sendDashLayout(layout: DashLayout) {
        writeChar(dashChar, layout.configText.toByteArray(Charsets.UTF_8))
    }

    // MARK: rides (device -> phone)

    fun refreshRides() {
        if (ridesChar == null) return
        rides = emptyList()
        loadingRides = true
        writeChar(ridesChar, byteArrayOf(0x01))
    }

    fun deleteRide(name: String) {
        rides = rides.filterNot { it.name == name }
        cachedRideFile(name).delete()
        // Offline: just drop the local cache. Connected: also delete on device.
        if (ridesChar == null) return
        writeChar(ridesChar, byteArrayOf(0x03) + name.toByteArray())
    }

    fun downloadRide(name: String) {
        if (ridesChar == null) return
        dlBuffer = java.io.ByteArrayOutputStream()
        dlExpected = 0
        dlName = name
        downloadingName = name
        downloadProgress = 0.0
        downloadedFile = null
        writeChar(ridesChar, byteArrayOf(0x02) + name.toByteArray())
    }

    /** Pull today's log off the device (reuses the reliable ride-transfer path). */
    fun downloadLog() {
        if (ridesChar == null) { lastMessage = "Not connected"; return }
        beginLogDownload("diag")
        writeChar(ridesChar, byteArrayOf(0x05))
    }

    /** List the per-day log files on the device (reply parsed via 0x30/0x31). */
    fun requestLogList() {
        if (ridesChar == null) return
        logsBuilding = mutableListOf()
        loadingLogs = true
        writeChar(ridesChar, byteArrayOf(0x06))
    }

    /** Download one specific day's log file. */
    fun downloadLogFile(name: String) {
        if (ridesChar == null) { lastMessage = "Not connected"; return }
        beginLogDownload(name)
        writeChar(ridesChar, byteArrayOf(0x07) + name.toByteArray())
    }

    private fun beginLogDownload(name: String) {
        dlBuffer = java.io.ByteArrayOutputStream()
        dlExpected = 0
        downloadingLog = true
        downloadingName = name
        downloadProgress = 0.0
        logFile = null
    }

    private fun handleRidesNotify(d: ByteArray) {
        when (d.firstOrNull()?.toInt()?.and(0xFF) ?: return) {
            0x01 -> {   // list entry: [u32 size][name]
                if (d.size <= 5) return
                rides = rides + RideFile(
                    name = String(d, 5, d.size - 5, Charsets.UTF_8),
                    size = le32(d, 1),
                )
            }

            0x03 -> {   // list done
                loadingRides = false
                rides = rides.sortedByDescending { it.name }   // newest first
            }

            0x30 -> {   // log-list entry: [u32 size][name]
                if (d.size <= 5) return
                logsBuilding.add(
                    LogFile(String(d, 5, d.size - 5, Charsets.UTF_8), le32(d, 1)),
                )
            }

            0x31 -> {   // log-list done
                loadingLogs = false
                deviceLogs = logsBuilding.sortedByDescending { it.name }   // newest day first
            }

            0x10 -> {   // download start: [u32 total]
                dlExpected = le32(d, 1)
                dlBuffer = java.io.ByteArrayOutputStream(max(dlExpected, 1024))
                dlNextSeq = 0
            }

            0x11 -> {   // chunk: [u16 seq][payload]
                if (d.size <= 3) return
                val seq = le16(d, 1)
                // Strict in-order: only append the chunk we're expecting next.
                // Anything else (a duplicate from a resend, or a chunk that
                // arrived after a gap) is dropped; the device resends from
                // dlNextSeq.
                if (seq == dlNextSeq) {
                    dlBuffer.write(d, 3, d.size - 3)
                    dlNextSeq = (dlNextSeq + 1) and 0xFFFF
                    if (dlExpected > 0) {
                        downloadProgress = min(1.0, dlBuffer.size().toDouble() / dlExpected)
                    }
                }
            }

            0x14 -> {   // window end — tell the device the next seq we need
                writeChar(
                    ridesChar,
                    byteArrayOf(0x04, (dlNextSeq and 0xFF).toByte(), (dlNextSeq shr 8).toByte()),
                )
            }

            0x12 -> finishDownload()
            0x13 -> Unit   // delete ack (already removed locally)

            0x1F -> {   // error (e.g. recording in progress)
                // Only surface a toast if the user was actively downloading; the
                // routine list-refresh fails silently mid-ride (the Rides tab
                // shows an "in progress" banner + the already-synced rides).
                val wasDownloading = downloadingName != null
                downloadingName = null
                loadingRides = false
                if (wasDownloading) lastMessage = "Device busy — stop the ride first"
            }
        }
    }

    private fun finishDownload() {
        val bytes = dlBuffer.toByteArray()
        // A short transfer means packets were lost — surface it instead of
        // handing a truncated file to the parser.
        if (dlExpected > 0 && bytes.size != dlExpected) {
            lastMessage = "Transfer incomplete (${bytes.size}/$dlExpected bytes) — try again"
            downloadingName = null
            downloadingLog = false
            return
        }
        if (downloadingLog) {
            downloadingLog = false
            var fname = (downloadingName ?: "diag").replace("/", "_")
            downloadingName = null
            if (!fname.endsWith(".log")) fname += ".log"
            runCatching {
                val dir = File(app.cacheDir, "logs").apply { mkdirs() }
                val out = File(dir, "opentrailpaper-$fname")
                out.writeBytes(bytes)
                logFile = out
            }.onFailure { lastMessage = "Log save failed: ${it.message}" }
            return
        }
        // Persist to the cache so it's available offline and never re-fetched.
        runCatching {
            val out = cachedRideFile(dlName)
            out.writeBytes(bytes)
            downloadedFile = out
            lastMessage = "$dlName ready"
        }.onFailure { lastMessage = "Save failed: ${it.message}" }
        downloadingName = null
    }

    // Downloaded rides are cached here so they never need re-downloading.
    fun ridesCacheDir(): File = File(app.filesDir, "rides").apply { mkdirs() }
    fun cachedRideFile(name: String): File = File(ridesCacheDir(), name)
    fun isCached(name: String): Boolean = cachedRideFile(name).exists()

    /** Rides already downloaded to this phone — available offline. */
    fun cachedRides(): List<RideFile> =
        (ridesCacheDir().listFiles() ?: emptyArray())
            .filter { it.extension.lowercase() == "fit" }
            .map { RideFile(it.name, it.length().toInt()) }
            .sortedByDescending { it.name }   // newest first

    // MARK: routes

    fun refreshRoutes() {
        if (routeChar == null) return
        deviceRoutes = emptyList()
        loadingRoutes = true
        writeChar(routeChar, byteArrayOf(0x06))
    }

    fun deleteRoute(name: String) {
        if (routeChar == null) return
        deviceRoutes = deviceRoutes.filterNot { it == name }
        writeChar(routeChar, byteArrayOf(0x07) + name.toByteArray())
    }

    private fun handleRouteNotify(d: ByteArray) {
        when (d.firstOrNull()?.toInt()?.and(0xFF) ?: return) {
            0x20 -> deviceRoutes = deviceRoutes + String(d, 1, d.size - 1, Charsets.UTF_8)
            0x21 -> { loadingRoutes = false; deviceRoutes = deviceRoutes.sorted() }
            0x22 -> Unit   // delete ack
            0x23 -> {      // device received + parsed the route
                lastUploadProgress = null
                routeSent = true
                routeReceived = true
            }

            0x24 -> {      // device got the upload but couldn't read the route
                lastUploadProgress = null
                routeSent = false
                routeReceived = false
                lastMessage = "Device couldn’t read the route — try sending again"
            }
        }
    }

    /**
     * Send a route as GPX plus its turn cues.
     *
     * The packets are enqueued in one go and paced by [GattQueue] — each write
     * waits for the device's ACK before the next is issued, which is both the
     * flow control iOS gets from its 12 ms sleep and stricter than it.
     */
    fun uploadRoute(name: String, gpx: String, maneuvers: List<Maneuver> = emptyList()) {
        if (routeChar == null) { lastMessage = "Not connected"; return }
        val data = gpx.toByteArray(Charsets.UTF_8)
        val maxLen = maxPayload - 1

        val packets = mutableListOf<ByteArray>()
        packets += byteArrayOf(0x01) + name.toByteArray().copyOf(min(name.toByteArray().size, 38))
        var i = 0
        while (i < data.size) {                                   // 0x02 data
            val end = min(i + maxLen, data.size)
            packets += byteArrayOf(0x02) + data.copyOfRange(i, end)
            i = end
        }
        packets += byteArrayOf(0x03)                              // end track

        for (m in maneuvers) {                                    // 0x04 turns
            val p = Packet(maxLen + 1)
            p.u8(0x04)
            p.i32((m.lat * 1e7).roundToLong().toInt())
            p.i32((m.lon * 1e7).roundToLong().toInt())
            val text = m.text.toByteArray(Charsets.UTF_8)
            p.raw(text.copyOf(min(text.size, maxLen - 8)))
            packets += p.bytes()
        }
        packets += byteArrayOf(0x05)                              // end nav

        lastUploadProgress = 0.0
        routeSent = false
        routeReceived = false
        var written = 0
        val total = packets.size
        val ch = routeChar
        packets.forEach { pkt ->
            writeChar(ch, pkt) {
                written += 1
                lastUploadProgress = written.toDouble() / total
                if (written == total) {
                    lastUploadProgress = null
                    // The writes have landed; the button now shows "Sent".
                    // Firmware that notifies back (0x23/0x24) upgrades that to a
                    // real "Received by device" — see handleRouteNotify.
                    routeSent = true
                    lastMessage = "Route “$name” sent — ${maneuvers.size} turns"
                }
            }
        }
    }

    // MARK: firmware / OTA

    /** True when the device is running something other than the newest release. */
    val updateAvailable: Boolean
        get() {
            val tag = FirmwareRelease.latest?.tag ?: return false
            return deviceFirmware.isNotEmpty() && deviceFirmware != tag
        }

    fun queryDeviceFirmware() {
        writeChar(otaChar, byteArrayOf(0x05))
    }

    private fun keepAwake(on: Boolean) { keepScreenOn = on }

    fun startFirmwareUpdate() {
        if (otaChar == null) { otaMessage = "Not connected"; return }
        val release = FirmwareRelease.latest ?: run {
            otaMessage = "No release found — check for updates first"
            return
        }
        keepAwake(true)
        otaMessage = "Downloading ${release.tag}…"
        scope.launch {
            runCatching { FirmwareRelease.image(release) }
                .onSuccess { startFirmwareUpload(it, release.tag) }
                .onFailure {
                    keepAwake(false)
                    otaMessage = "Download failed — check your connection"
                }
        }
    }

    private fun startFirmwareUpload(data: ByteArray, tag: String) {
        if (otaChar == null) {
            keepAwake(false); otaMessage = "Not connected"; return
        }
        otaTargetVersion = tag
        otaData = data
        otaSentBytes = 0
        otaCommitSent = false
        otaInProgress = true
        otaPhase = OtaPhase.SENDING
        otaProgress = 0.0
        otaMessage = "Preparing…"
        otaWatchdog?.cancel(); otaWatchdog = null

        // begin: [0x01][u32 size][32-char md5 hex]
        val md5 = MessageDigest.getInstance("MD5").digest(data)
            .joinToString("") { "%02x".format(it) }
        val cmd = Packet(37)
        cmd.u8(0x01)
        cmd.u32(data.size)
        cmd.raw(md5.toByteArray())
        writeChar(otaChar, cmd.bytes())
    }

    /**
     * Hand the whole remaining image to the queue. Where iOS pumps chunks as
     * CoreBluetooth reports the pipe drained, the queue here already issues one
     * packet per completion callback, so enqueuing everything at once IS the
     * pump — and the commit lands last simply by being enqueued last.
     */
    private fun pumpOtaChunks() {
        if (!otaInProgress || otaChar == null || otaCommitSent) return
        val size = chunkSize
        var offset = 0
        val total = otaData.size
        while (offset < total) {
            val end = min(offset + size, total)
            val pkt = byteArrayOf(0x02) + otaData.copyOfRange(offset, end)
            val payload = end - offset
            writeChar(otaChar, pkt, withResponse = false) {
                otaSentBytes += payload
                otaProgress = otaSentBytes.toDouble() / max(total, 1)
            }
            offset = end
        }
        otaCommitSent = true
        writeChar(otaChar, byteArrayOf(0x03)) {                   // commit
            otaPhase = OtaPhase.SAVING
            otaMessage = "Saving to the device…"
        }
    }

    /** Fail the update if the device doesn't reach the new version in time. */
    private fun armOtaWatchdog(seconds: Long, failMessage: String) {
        otaWatchdog?.cancel()
        otaWatchdog = scope.launch {
            delay(seconds * 1000)
            if (otaInProgress) otaFinish(OtaPhase.FAILED, failMessage)
        }
    }

    private fun otaFinish(phase: OtaPhase, msg: String) {
        otaWatchdog?.cancel(); otaWatchdog = null
        otaInProgress = false
        otaPhase = phase
        otaMessage = msg
        keepAwake(false)
    }

    private fun handleOtaNotify(d: ByteArray) {
        when (d.firstOrNull()?.toInt()?.and(0xFF) ?: return) {
            0xA3 -> {   // running version
                deviceFirmware = String(d, 1, d.size - 1, Charsets.UTF_8)
                if (otaTargetVersion.isNotEmpty() && deviceFirmware == otaTargetVersion) {
                    // Now on the new version — success (this also clears an
                    // earlier transient failure, e.g. a reconnect seen mid-flash).
                    if (otaInProgress) {
                        otaProgress = 1.0
                        otaFinish(OtaPhase.DONE, "Updated to $deviceFirmware 🎉")
                    } else if (otaPhase == OtaPhase.FAILED) {
                        otaWatchdog?.cancel()
                        otaPhase = OtaPhase.IDLE
                        otaMessage = null
                    }
                } else if (otaInProgress && otaCommitSent) {
                    // Reconnected but STILL on the old version. The device
                    // flashes from SD and reboots ONCE MORE, so don't fail yet —
                    // give it a short grace for that second reboot to land.
                    otaPhase = OtaPhase.VERIFYING
                    otaMessage = "Installing — flashing from the SD card…"
                    armOtaWatchdog(
                        60,
                        "Device restarted but is still on $deviceFirmware. The install " +
                            "didn't take — try again, or use the SD-card method.",
                    )
                }
            }

            0xA0 -> {
                otaPhase = OtaPhase.SENDING
                otaMessage = "Sending firmware…"
                pumpOtaChunks()
            }

            0xA1 -> {   // received + saved to SD
                // The device reboots and flashes from SD now; wait for the
                // reconnect version-check to confirm. Arm the watchdog in case it
                // never returns.
                otaProgress = 1.0
                otaPhase = OtaPhase.INSTALLING
                otaMessage = "Installing — the device is restarting…"
                keepAwake(false)
                armOtaWatchdog(150, INSTALL_WATCHDOG_MSG)
            }

            0xA2 -> otaFinish(OtaPhase.FAILED, "Update canceled.")

            0xAF -> {   // error
                val code = if (d.size > 1) d[1].toInt() and 0xFF else -1
                otaFinish(
                    OtaPhase.FAILED,
                    "Transfer failed (error $code). The device is unchanged — tap to try again.",
                )
            }
        }
    }

    // MARK: sensor management (the device's HR / power / cadence sensors)

    fun startSensorScan() {
        if (sensorsChar == null) return
        scanningSensors = true
        writeChar(sensorsChar, byteArrayOf(0x01))
    }

    fun stopSensorScan() {
        if (sensorsChar == null) return
        scanningSensors = false
        writeChar(sensorsChar, byteArrayOf(0x02))
    }

    /** One snapshot of the device's sensors + status, without starting a scan. */
    fun refreshSensors() {
        writeChar(sensorsChar, byteArrayOf(0x05))
    }

    fun pairSensor(addr: String) = writeChar(sensorsChar, byteArrayOf(0x03) + addr.toByteArray())
    fun forgetSensor(addr: String) = writeChar(sensorsChar, byteArrayOf(0x04) + addr.toByteArray())

    /** Connected sensors whose kind mask includes [bit] (1 HR, 2 power, 4 cadence). */
    fun connectedSensor(bit: Int): BikeSensor? =
        sensors.firstOrNull { it.connected && it.kindsMask and bit != 0 }

    private fun handleSensorsNotify(d: ByteArray) {
        when (d.firstOrNull()?.toInt()?.and(0xFF) ?: return) {
            0x10 -> sensorsBuilding = mutableListOf()             // list begin

            0x11 -> {                                             // entry
                if (d.size < 5) return
                val mask = d[1].toInt() and 0xFF
                val flags = d[2].toInt() and 0xFF
                val rssi = d[3].toInt()                           // already signed
                val nameLen = d[4].toInt() and 0xFF
                if (d.size < 5 + nameLen) return
                val name = String(d, 5, nameLen, Charsets.UTF_8)
                val addr = String(d, 5 + nameLen, d.size - 5 - nameLen, Charsets.UTF_8)
                if (addr.isEmpty()) return
                sensorsBuilding.add(
                    BikeSensor(
                        addr = addr,
                        name = name.ifEmpty { addr },
                        kindsMask = mask,
                        connected = flags and 1 != 0,
                        paired = flags and 2 != 0,
                        rssi = rssi,
                    ),
                )
            }

            0x12 -> sensors = sensorsBuilding.sortedWith(         // list end — publish
                compareByDescending<BikeSensor> { it.connected }
                    .thenByDescending { it.paired }
                    .thenByDescending { it.rssi },
            )
        }
    }

    // MARK: vector-map upload (phone -> device SD, mirrors the OTA transfer)

    val canUploadMap: Boolean get() = mapChar != null

    fun uploadMap(ebm: ByteArray, name: String) {
        if (mapChar == null) { mapMessage = "Not connected"; return }
        mapData = ebm
        mapSentBytes = 0
        mapEndSent = false
        mapUploading = true
        mapProgress = 0.0
        mapMessage = "Sending map…"
        keepAwake(true)

        val cmd = Packet(5 + name.length)
        cmd.u8(0x01)
        cmd.u32(ebm.size)
        cmd.raw(name.toByteArray())
        writeChar(mapChar, cmd.bytes())
    }

    fun cancelMapUpload() {
        queue.clear()
        writeChar(mapChar, byteArrayOf(0x04))
        mapUploading = false
        keepAwake(false)
        mapMessage = "Canceled"
    }

    /** Ask the device which map areas it already has (streamed back via notify). */
    fun refreshDeviceMaps() = writeChar(mapChar, byteArrayOf(0x05))

    /**
     * Ask the device which H3 tile ids are already on its SD, so the caller can
     * skip re-sending them (replied via 0xD0/0xD1/0xD2 notifies).
     */
    fun refreshDeviceTiles() = writeChar(mapChar, byteArrayOf(0x07))

    // Streaming upload: the app produces tiles batch-by-batch while download +
    // vectorization runs, and sends them in parallel. Call startTileStream()
    // once, enqueueTiles() per batch as they're built, finishTileStream() when
    // the last batch has been produced.
    fun startTileStream() {
        if (mapChar == null) { tileMessage = "Not connected"; return }
        tileQueue = ArrayDeque()
        tilesTotal = 0
        tilesDone = 0
        tileJobFailed = false
        tilesMoreComing = true
        tilesUploading = true
        tileMessage = "Sending tiles…"
        keepAwake(true)
    }

    /**
     * Add freshly-built tiles to the send queue; starts pumping if idle. Skips
     * tiles already on the device or already queued.
     */
    fun enqueueTiles(newOnes: List<Pair<String, ByteArray>>) {
        if (!tilesUploading) return
        val queued = tileQueue.map { it.first }.toSet()
        val fresh = newOnes.filter {
            it.first !in deviceTileIds && it.first !in queued && it.first != currentTileId
        }
        if (fresh.isEmpty()) return
        tileQueue.addAll(fresh)
        tilesTotal += fresh.size
        if (currentTileId == null) sendNextTile()   // pump if idle
    }

    /** No more batches coming; let the queue drain and finish. */
    fun finishTileStream() {
        tilesMoreComing = false
        if (currentTileId == null) sendNextTile()   // trigger drain-finish if idle
    }

    fun cancelTileUpload() {
        tileQueue.clear()
        tilesMoreComing = false
        queue.clear()
        if (currentTileId != null) writeChar(mapChar, byteArrayOf(0x04))
        finishTileJob("Canceled")
    }

    private fun finishTileJob(message: String?) {
        tilesUploading = false
        tilesMoreComing = false
        mapUploading = false
        currentTileId = null
        keepAwake(false)
        if (message != null) tileMessage = message
    }

    private fun sendNextTile() {
        if (!tilesUploading || mapChar == null) return
        val tile = tileQueue.removeFirstOrNull() ?: run {   // queue drained
            if (tilesMoreComing) return                     // more still building — idle, wait
            finishTileJob(if (tileJobFailed) "Some tiles failed" else "Tiles installed")
            refreshDeviceTiles()
            return
        }
        currentTileId = tile.first
        mapData = tile.second
        mapSentBytes = 0
        mapEndSent = false
        mapUploading = true            // reuse the CHR_MAP chunk pump
        tileMessage = "Sending tile ${tilesDone + 1}/$tilesTotal…"

        val cmd = Packet(5 + tile.first.length)
        cmd.u8(0x06)                   // begin-tile
        cmd.u32(tile.second.size)
        cmd.raw(tile.first.toByteArray())
        writeChar(mapChar, cmd.bytes())
    }

    private fun pumpMapChunks() {
        if (!mapUploading || mapChar == null || mapEndSent) return
        val size = chunkSize
        val total = mapData.size
        var offset = 0
        while (offset < total) {
            val end = min(offset + size, total)
            val pkt = byteArrayOf(0x02) + mapData.copyOfRange(offset, end)
            val payload = end - offset
            writeChar(mapChar, pkt, withResponse = false) {
                mapSentBytes += payload
                mapProgress = mapSentBytes.toDouble() / max(total, 1)
            }
            offset = end
        }
        mapEndSent = true
        writeChar(mapChar, byteArrayOf(0x03)) {                   // end -> save
            if (currentTileId == null) mapMessage = "Saving to device…"
        }
    }

    private fun handleMapNotify(d: ByteArray) {
        when (d.firstOrNull()?.toInt()?.and(0xFF) ?: return) {
            0xB0 -> pumpMapChunks()                               // device ready (map or tile)

            0xB1 -> {                                             // saved + active
                val id = currentTileId
                if (id != null) {                                 // a tile finished
                    deviceTileIds = deviceTileIds + id
                    tilesDone += 1
                    currentTileId = null
                    sendNextTile()
                } else {
                    mapUploading = false
                    mapProgress = 1.0
                    mapMessage = "Map installed"
                    keepAwake(false)
                    refreshDeviceMaps()
                }
            }

            0xBF -> {
                val code = if (d.size > 1) d[1].toInt() and 0xFF else -1
                if (currentTileId != null) {                      // skip it, keep going
                    tileJobFailed = true
                    currentTileId = null
                    sendNextTile()
                } else {
                    mapUploading = false
                    keepAwake(false)
                    mapMessage = "Map upload failed ($code)"
                }
            }

            0xD0 -> tileIdsBuilding = mutableListOf()              // tile-list begin

            0xD1 -> {                                             // comma-separated ids
                if (d.size <= 1) return
                String(d, 1, d.size - 1, Charsets.UTF_8)
                    .split(',')
                    .filter { it.isNotEmpty() }
                    .forEach { tileIdsBuilding.add(it) }
            }

            0xD2 -> {                                             // tile-list end
                deviceTileIds = tileIdsBuilding.toSet()
                Prefs.deviceTileIds = deviceTileIds
            }

            0xC0 -> deviceMapsBuilding = mutableListOf()           // map-list begin

            0xC1 -> {                                             // entry: 4x f64 + flag
                if (d.size < 34) return
                fun f64(i: Int): Double {
                    var bits = 0L
                    for (k in 0 until 8) {
                        bits = bits or ((d[1 + i * 8 + k].toLong() and 0xFF) shl (8 * k))
                    }
                    return Double.fromBits(bits)
                }
                deviceMapsBuilding.add(
                    DeviceMap(f64(0), f64(1), f64(2), f64(3), d[33].toInt() != 0),
                )
            }

            0xC2 -> deviceMaps = deviceMapsBuilding.toList()       // map-list end
            // 0xB4 progress is device-side; nothing to do here.
        }
    }

    // MARK: GPS aiding + live phone position

    // Stream the phone's location to the device: warm-starts its GPS (AGNSS
    // seed), and serves as a fallback position + altitude source when the
    // device's own GPS has no fix. Started on connect, stopped on disconnect.

    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            main.post {
                lastLocation = location
                if (wantsAiding) transmitAiding(location)
            }
        }

        @Deprecated("Required by the pre-30 interface")
        override fun onStatusChanged(provider: String?, statusCode: Int, extras: Bundle?) = Unit

        override fun onProviderEnabled(provider: String) = Unit
        override fun onProviderDisabled(provider: String) = Unit
    }

    /** Begin streaming the phone's location to the device. */
    fun startLocationStream() {
        if (routeChar == null) return
        if (!locationAuthorized) { wantsAiding = false; return }
        wantsAiding = true
        beginLocationUpdates()
    }

    private fun beginLocationUpdates() {
        val lm = locationManager ?: return
        if (!locationAuthorized) return
        // While a ride is recording these fixes are the head unit's fallback
        // TRACK whenever its own receiver has nothing, and a track sampled every
        // few seconds cuts corners — so ask for 1 s then, and a lazy 3 s
        // otherwise (seeding the receiver only needs a rough position).
        val interval = if (status.recording) 1000L else 3000L
        runCatching {
            lm.removeUpdates(locationListener)
            for (provider in listOf(LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER)) {
                if (lm.isProviderEnabled(provider)) {
                    lm.requestLocationUpdates(provider, interval, 0f, locationListener, Looper.getMainLooper())
                }
            }
            lm.getLastKnownLocation(LocationManager.GPS_PROVIDER)
                ?.let { lastLocation = it; transmitAiding(it) }
        }
        updateBackgroundLocationMode()
    }

    /**
     * Tie the foreground service to the ride state. Android will only keep
     * location running with the screen off inside a location-typed foreground
     * service, and only a RECORDING ride warrants that — this is the analogue of
     * iOS's `allowsBackgroundLocationUpdates`, including the reason it is gated:
     * tracking in the background while idle is what actually costs the battery.
     */
    private fun updateBackgroundLocationMode() {
        val shouldRun = wantsAiding && status.recording && locationAuthorized
        if (shouldRun == rideServiceRunning) return
        rideServiceRunning = shouldRun
        val intent = Intent(app, RideLocationService::class.java)
        if (shouldRun) {
            ContextCompat.startForegroundService(app, intent)
            beginLocationUpdates()   // re-request at the tighter recording interval
        } else {
            app.stopService(intent)
        }
    }

    private var rideServiceRunning = false

    fun stopLocationStream() {
        wantsAiding = false
        runCatching { locationManager?.removeUpdates(locationListener) }
        if (rideServiceRunning) {
            rideServiceRunning = false
            app.stopService(Intent(app, RideLocationService::class.java))
        }
    }

    private fun transmitAiding(loc: Location) {
        if (routeChar == null) return
        // Throttle so we don't flood the link with position updates.
        val minGap = if (status.recording) 1000L else 3000L
        val last = lastAidingSent
        val now = System.currentTimeMillis()
        if (last != null && now - last < minGap) return

        val p = Packet(21)
        p.u8(0x08)
        p.i32((loc.latitude * 1e7).roundToLong().toInt())
        p.i32((loc.longitude * 1e7).roundToLong().toInt())
        p.i32((now / 1000).toInt())                      // current UTC
        // Extended fields (the device parses them when present): altitude + accuracy.
        p.i16(loc.altitude.roundToInt().coerceIn(-2000, 9000))
        val acc = if (loc.hasAccuracy() && loc.accuracy > 0) loc.accuracy.toDouble() else 200.0
        p.i16(acc.roundToInt().coerceIn(1, 9999))
        writeChar(routeChar, p.bytes())
        lastAidingSent = now
    }

    /**
     * Only run the phone's GPS while the device NEEDS it. Once the head unit has
     * its own fix, stop streaming; resume if it loses it. Debounced so a brief
     * dropout doesn't flap the stream.
     */
    private fun syncLocationStreamToDeviceFix() {
        if (state != ConnState.CONNECTED || routeChar == null) return
        val firstStatus = !sawStatusSinceConnect
        sawStatusSinceConnect = true
        if (status.gpsFix) {
            // The device was ALREADY locked when we connected. We start the
            // stream the moment the route characteristic shows up, before the
            // device has said anything about itself, so this is the first chance
            // to know it was never needed — drop it now rather than running the
            // phone's GPS through the debounce window. That window exists to
            // ride out a DROPOUT mid-ride, not to delay this.
            if (firstStatus) {
                fixStableJob?.cancel(); fixStableJob = null
                if (wantsAiding) stopLocationStream()
                return
            }
            if (wantsAiding && fixStableJob == null) {
                fixStableJob = scope.launch {
                    delay(8_000)                          // fix held 8 s
                    if (status.gpsFix) stopLocationStream()
                    fixStableJob = null
                }
            }
        } else {
            fixStableJob?.cancel(); fixStableJob = null
            if (!wantsAiding) startLocationStream()       // device is cold again — help it
        }
    }

    companion object {
        private const val INSTALL_WATCHDOG_MSG =
            "Device didn't come back after installing. Check it's powered on and nearby, " +
                "or use the SD-card method."

        private fun le16(d: ByteArray, i: Int): Int =
            (d[i].toInt() and 0xFF) or ((d[i + 1].toInt() and 0xFF) shl 8)

        private fun le32(d: ByteArray, i: Int): Int =
            (d[i].toInt() and 0xFF) or ((d[i + 1].toInt() and 0xFF) shl 8) or
                ((d[i + 2].toInt() and 0xFF) shl 16) or ((d[i + 3].toInt() and 0xFF) shl 24)
    }
}

/** Little-endian packet builder — the wire order every device payload uses. */
private class Packet(capacity: Int = 32) {
    private val out = java.io.ByteArrayOutputStream(capacity)

    fun u8(v: Int) { out.write(v and 0xFF) }

    fun i16(v: Int) {
        out.write(v and 0xFF)
        out.write((v shr 8) and 0xFF)
    }

    fun i32(v: Int) {
        for (s in 0 until 32 step 8) out.write((v shr s) and 0xFF)
    }

    fun u32(v: Int) = i32(v)

    fun raw(b: ByteArray) = out.write(b, 0, b.size)

    fun bytes(): ByteArray = out.toByteArray()
}
