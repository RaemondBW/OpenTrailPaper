package com.raemond.opentrailpaper.sim

import android.Manifest
import android.app.Application
import android.content.Context
import android.os.Looper
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.BleManager.ConnState
import com.raemond.opentrailpaper.ble.BleManager.PairingIssue
import com.raemond.opentrailpaper.ble.SimRadio
import com.raemond.opentrailpaper.data.DeviceSettingsStore
import com.raemond.opentrailpaper.data.Prefs
import com.raemond.opentrailpaper.data.RideListingStore
import com.raemond.opentrailpaper.data.RideNames
import com.raemond.opentrailpaper.link.proto.Feature
import com.raemond.opentrailpaper.transfer.TransferCenter
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import java.time.Duration

/** Robolectric's application, with the app's stores but no BleManager of its own. */
class SimTestApp : Application() {
    override fun onCreate() {
        super.onCreate()
        Prefs.init(this)
        DeviceSettingsStore.init(this)
        RideListingStore.init(this)
        RideNames.init(this)
        TransferCenter.init(this)
    }
}

/**
 * The app's real BleManager — its connect / bond / refusal / barren-connect /
 * reconnect state machine, GattQueue and LinkClient — against the real
 * firmware running in the simulator (sim/build/otp-sim), through SimRadio,
 * which reports what Android's stack would (statuses, bond broadcasts).
 *
 * Robolectric supplies Android's main looper, SharedPreferences and the
 * android.bluetooth value classes; the looper is advanced in step with the
 * wall clock while the sim runs in real time (see [waitFor]).
 *
 * Skipped when the sim isn't built (sim/build.sh).
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = SimTestApp::class)
class BleManagerSimTest {

    private lateinit var app: Application
    private lateinit var sim: FirmwareSim
    private val radios = mutableListOf<SimRadio>()
    private val log = StringBuilder()

    @Before
    fun setUp() {
        assumeTrue(
            "sim/build/otp-sim is not built (run sim/build.sh) — skipping the firmware-sim tests",
            FirmwareSim.binary != null,
        )
        app = RuntimeEnvironment.getApplication()
        shadowOf(app).grantPermissions(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN)
        Prefs.onboarded = true
        sim = FirmwareSim.start()
    }

    @After
    fun tearDown() {
        // Into the test report's system-out: what the radio saw, for reading a run.
        if (::sim.isInitialized) println(dump())
        radios.forEach { runCatching { it.setEnabled(false) } }
        if (::sim.isInitialized) sim.close()
    }

    // ---- harness -----------------------------------------------------------------------------

    /** A phone: its own bond store (identity + bonds) and SimRadio options. */
    private fun radio(phone: String = "phoneA", autoConnectFailMs: Long = 0): SimRadio {
        val store = app.getSharedPreferences("simradio-$phone", Context.MODE_PRIVATE)
        return SimRadio(
            "127.0.0.1", sim.blePort, store,
            SimRadio.Options(
                phoneName = "Sim $phone",
                autoConnectFailMs = autoConnectFailMs,
                directConnectTimeoutMs = 1_500,
                log = { synchronized(log) { log.append(System.currentTimeMillis() % 100_000).append(' ').append(it).append('\n') } },
            ),
        ).also { radios += it }
    }

    private fun manager(radio: SimRadio) = BleManager(app, radio)

    /**
     * Run the main looper with the clock moving as the wall clock does, until
     * [cond] holds. [every] runs on each step (to watch for things that must
     * never happen on the way).
     */
    private fun waitFor(what: String, timeoutMs: Long, every: () -> Unit = {}, cond: () -> Boolean) {
        val end = System.currentTimeMillis() + timeoutMs
        while (true) {
            shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(20))
            every()
            if (cond()) return
            if (System.currentTimeMillis() > end) fail("timed out waiting for $what\n${dump()}")
            Thread.sleep(20)
        }
    }

    /** Keep the looper running for [ms], checking [every] throughout. */
    private fun runFor(ms: Long, every: () -> Unit = {}) {
        val end = System.currentTimeMillis() + ms
        while (System.currentTimeMillis() < end) {
            shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(20))
            every()
            Thread.sleep(20)
        }
    }

    private fun dump(): String {
        val tail = runCatching { sim.consoleLog.readLines().takeLast(30).joinToString("\n") }.getOrDefault("")
        return "--- SimRadio ---\n${synchronized(log) { log.toString().lines().takeLast(60).joinToString("\n") }}" +
            "\n--- otp-sim console (tail) ---\n$tail"
    }

    private fun phoneConnected() = runCatching { sim.status().get("phone_connected").asBoolean }.getOrDefault(false)

    private fun connectedOverLink(ble: BleManager) =
        ble.state == ConnState.CONNECTED && ble.linkReady && ble.deviceHello != null

    /** First connect: scan, connect, pair (the device shows the code; confirmed), Hello. */
    private fun pairFresh(ble: BleManager, radio: SimRadio) {
        waitFor("first connect, pairing and Hello", 30_000) { connectedOverLink(ble) }
        assertNull("no pairing issue", ble.pairingIssue)
        assertEquals("remembered as our device", radio.deviceAddress, ble.pairedDeviceAddress)
        assertTrue("Android holds the bond", radio.holdsBond(radio.deviceAddress))
        waitFor("device reports the phone connected", 5_000) { phoneConnected() }
    }

    private val neverNotRecognised: (BleManager) -> () -> Unit = { ble ->
        {
            if (ble.pairingIssue == PairingIssue.NOT_RECOGNISED) {
                fail("the app told the rider the device doesn't recognise this phone\n${dump()}")
            }
        }
    }

    // ---- scenarios ---------------------------------------------------------------------------

    /** (1) A factory-fresh device: the first connect pairs, Hello completes, the device has the phone. */
    @Test
    fun firstConnectPairsAndSaysHello() {
        val radio = radio()
        val ble = manager(radio)
        pairFresh(ble, radio)
        val st = sim.status()
        assertTrue("device paired", st.get("phone_paired").asBoolean)
        assertEquals("the phone's name reached the device", "Sim phoneA", st.get("phone_name").asString)
        assertTrue("DeviceHello lists features", ble.has(Feature.FEATURE_LIVE))
        val bonds = st.getAsJsonObject("ble").getAsJsonArray("bonds").map { it.asString.uppercase() }
        assertTrue("the device stored this phone's identity: $bonds", radio.identity in bonds)
    }

    /** (2) The radio link drops (supervision timeout): the app reconnects by itself, every time. */
    @Test
    fun linkDropReconnectsWithoutCallingItARefusal() {
        val radio = radio()
        val ble = manager(radio)
        pairFresh(ble, radio)
        repeat(3) { i ->
            val upBefore = radio.linksUp.get()
            sim.dropPhone(0x08)
            waitFor("drop #${i + 1} noticed", 5_000, neverNotRecognised(ble)) { ble.state != ConnState.CONNECTED }
            waitFor("reconnect #${i + 1}", 20_000, neverNotRecognised(ble)) {
                connectedOverLink(ble) && radio.linksUp.get() > upBefore
            }
            assertNull(ble.pairingIssue)
            waitFor("device has the phone again", 5_000) { phoneConnected() }
        }
    }

    /**
     * (3) The device goes out of reach and the reconnects fail without ever
     * reaching it (GATT 133, never STATE_CONNECTED) — more than three times.
     * That is radio, not the device refusing us: no NOT_RECOGNISED, and the
     * app is back as soon as the device is (regression for 9cb166d,
     * linkUpSinceConnect).
     */
    @Test
    fun connectsThatNeverComeUpAreNotARefusal() {
        val radio = radio(autoConnectFailMs = 700)
        val ble = manager(radio)
        pairFresh(ble, radio)

        sim.stop()   // powered off / out of range: nothing listens any more
        val attemptsBefore = radio.connectAttempts.get()
        waitFor("at least five failed reconnects", 30_000, neverNotRecognised(ble)) {
            radio.connectAttempts.get() >= attemptsBefore + 5
        }
        val upWhileAway = radio.linksUp.get()
        assertTrue("still trying to reconnect", ble.state == ConnState.CONNECTING)

        sim.launch()  // same device directory: it still holds our bond
        waitFor("reconnect once the device is back", 30_000, neverNotRecognised(ble)) {
            connectedOverLink(ble) && radio.linksUp.get() > upWhileAway
        }
        assertNull(ble.pairingIssue)
    }

    /**
     * (4) The device belongs to another phone: a second phone (its own
     * identity, no bond) is turned away — it says so, once, and does not keep
     * reconnecting.
     */
    @Test
    fun secondPhoneIsRefusedWithoutLooping() {
        val radioA = radio("phoneA")
        val bleA = manager(radioA)
        pairFresh(bleA, radioA)
        radioA.setEnabled(false)   // phone A switches Bluetooth off
        waitFor("phone A gone", 10_000) { !phoneConnected() && bleA.state == ConnState.POWERED_OFF }

        // Phone B: a different handset, the app freshly installed.
        app.getSharedPreferences("opentrailpaper", Context.MODE_PRIVATE).edit().clear().putBoolean("didOnboard", true).commit()
        val radioB = radio("phoneB")
        assertTrue(radioB.identity != radioA.identity)
        val bleB = manager(radioB)
        waitFor("phone B told the device belongs to another phone", 30_000) {
            bleB.pairingIssue == PairingIssue.PAIRED_ELSEWHERE || bleB.pairingIssue == PairingIssue.REFUSED
        }
        val attempts = radioB.connectAttempts.get()
        runFor(10_000) {
            if (bleB.state == ConnState.CONNECTED) fail("phone B was let in\n${dump()}")
        }
        assertEquals("phone B stopped connecting after the refusal", attempts, radioB.connectAttempts.get())
        assertTrue(bleB.pairingIssue == PairingIssue.PAIRED_ELSEWHERE || bleB.pairingIssue == PairingIssue.REFUSED)
        assertNull("not remembered as phone B's device", bleB.pairedDeviceAddress)
        assertTrue("the device still belongs to phone A", sim.status().get("phone_paired").asBoolean)
    }

    /** (5) Paging the Ride tab pages the device (link ShowPage). */
    @Test
    fun showPageReachesTheDevice() {
        val radio = radio()
        val ble = manager(radio)
        pairFresh(ble, radio)
        assumeTrue("firmware without ShowPage", ble.has(Feature.FEATURE_SHOW_PAGE))
        waitFor("the device's dashboard layout", 10_000) { ble.dashConfig != null }
        val pages = ble.dashConfig!!.pages.size
        assertTrue("layout has at least two pages ($pages)", pages >= 2)
        waitFor("device on page 0", 10_000) { sim.status().get("page").asInt == 0 }
        ble.showDevicePage(1)
        waitFor("device on page 1", 10_000) { sim.status().get("page").asInt == 1 }
        ble.showDevicePage(0)
        waitFor("device back on page 0", 10_000) { sim.status().get("page").asInt == 0 }
        assertNotNull(ble.deviceHello)
    }
}
