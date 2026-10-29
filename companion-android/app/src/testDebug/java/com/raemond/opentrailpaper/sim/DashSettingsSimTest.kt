package com.raemond.opentrailpaper.sim

import android.Manifest
import android.app.Application
import android.content.Context
import android.os.Looper
import com.google.gson.JsonObject
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.BleManager.ConnState
import com.raemond.opentrailpaper.ble.SimRadio
import com.raemond.opentrailpaper.data.DashConfig
import com.raemond.opentrailpaper.data.DashConfig.Page
import com.raemond.opentrailpaper.data.DashItem
import com.raemond.opentrailpaper.data.DashLayout
import com.raemond.opentrailpaper.data.DashSize
import com.raemond.opentrailpaper.data.Prefs
import com.raemond.opentrailpaper.link.proto.Feature
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Ignore
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import java.io.File
import java.time.Duration

/**
 * The Dashboard editor and the device Settings screen, as far as the app's
 * own code takes them: layouts built with the editor's model (DashConfig /
 * DashLayout, the same values DashboardEditorSheet builds and hands to
 * [BleManager.saveDashConfig]) and settings changed through BleManager's
 * `updateX` setters (what SettingsScreen calls), sent by the real
 * BleManager over SimRadio to the firmware in otp-sim.
 *
 * What the device applied is read from the device itself (control `status`:
 * `dash` — dash_config::currentText, `page_count`, `screen`, `settings`), and
 * what the app shows is read from the BleManager state the screens render
 * (dashConfig / dashTarget / pendingDashConfig, ftpWatts, wheelCircMm, …).
 *
 * Compose UI tests are not set up for the unit tests, so the editor's
 * gestures (add page, drag the map card, remove a field) are the model edits
 * the sheet's callbacks make.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = SimTestApp::class)
class DashSettingsSimTest {

    private lateinit var app: Application
    private lateinit var sim: FirmwareSim
    private var radio: SimRadio? = null
    private val log = StringBuilder()

    @Before
    fun setUp() {
        if (System.getenv("OTP_SIM_REQUIRED") != null) {
            check(FirmwareSim.binary != null) { "OTP_SIM_REQUIRED, but sim/build/otp-sim is not built" }
        }
        assumeTrue("sim/build/otp-sim is not built (run sim/build.sh)", FirmwareSim.binary != null)
        app = RuntimeEnvironment.getApplication()
        shadowOf(app).grantPermissions(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN)
        Prefs.onboarded = true
        sim = FirmwareSim.start()
    }

    @After
    fun tearDown() {
        if (::sim.isInitialized) println(dump())
        runCatching { radio?.setEnabled(false) }
        if (::sim.isInitialized) sim.close()
    }

    // ---- harness -----------------------------------------------------------------------------

    /** Pair a fresh phone and wait for Hello and the device's layout and settings. */
    private fun connect(): BleManager {
        val store = app.getSharedPreferences("simradio-dash", Context.MODE_PRIVATE)
        val r = SimRadio(
            "127.0.0.1", sim.blePort, store,
            SimRadio.Options(
                phoneName = "Sim dash",
                directConnectTimeoutMs = 1_500,
                log = { synchronized(log) { log.append(System.currentTimeMillis() % 100_000).append(' ').append(it).append('\n') } },
            ),
        )
        radio = r
        val ble = BleManager(app, r)
        waitFor("pairing and Hello", 30_000) { linkUp(ble) }
        assumeTrue("firmware without the link dashboard", ble.has(Feature.FEATURE_DASH))
        waitFor("the device's layout and settings", 10_000) {
            ble.dashConfig != null && ble.settingsCache.device != null
        }
        return ble
    }

    private fun linkUp(ble: BleManager) = ble.state == ConnState.CONNECTED && ble.linkReady && ble.deviceHello != null

    /** The main looper, moving with the wall clock (see BleManagerSimTest.LooperClock). */
    private fun waitFor(what: String, timeoutMs: Long, cond: () -> Boolean) {
        val end = System.currentTimeMillis() + timeoutMs
        var last = System.nanoTime()
        while (true) {
            val now = System.nanoTime()
            shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(maxOf(1L, (now - last) / 1_000_000)))
            last = now
            if (cond()) return
            if (System.currentTimeMillis() > end) fail("timed out waiting for $what\n${dump()}")
            Thread.sleep(20)
        }
    }

    /** Keep the looper running for [ms] (nothing should change meanwhile). */
    private fun runFor(ms: Long) {
        val end = System.currentTimeMillis() + ms
        waitFor("nothing", ms + 5_000) { System.currentTimeMillis() >= end }
    }

    private fun dump(): String =
        "--- SimRadio ---\n${synchronized(log) { log.toString().lines().takeLast(40).joinToString("\n") }}" +
            "\n--- otp-sim console (tail) ---\n${sim.consoleTail(40)}"

    private fun status(): JsonObject = sim.status()
    private fun deviceDash(): String = status().get("dash").asString
    private fun deviceSettings(): JsonObject = status().getAsJsonObject("settings")

    /**
     * The panel's page count (LiveView.pageCount) is refreshed by the UI
     * task's loop, a moment after the link has answered with the layout.
     */
    private fun waitForPageCount(n: Int, what: String) =
        waitFor("$what: $n pages on the device", 10_000) { status().get("page_count").asInt == n }

    /** A hand edit of /config/dashboard.cfg on the card (USB drive), read at the next boot. */
    private fun handEditCard(ble: BleManager, text: String) {
        File(sim.dir, "sd/config").mkdirs()
        File(sim.dir, "sd/config/dashboard.cfg").writeText(text)
        rebootAndReconnect(ble)
    }

    /** The device restarts; the app notices the link go and comes back by itself. */
    private fun rebootAndReconnect(ble: BleManager) {
        val upBefore = radio!!.linksUp.get()
        sim.reboot()
        waitFor("the app back on the rebooted device", 40_000) { linkUp(ble) && radio!!.linksUp.get() > upBefore }
        waitFor("the device's layout and settings on the new link", 10_000) {
            runCatching { status().get("phone_connected").asBoolean }.getOrDefault(false)
        }
    }

    /** The Send button and the wait for the device's answer. */
    private fun send(ble: BleManager, config: DashConfig) {
        ble.saveDashConfig(config)
        waitFor("the device's answer to the layout", 20_000) { ble.pendingDashConfig == null }
    }

    private fun item(field: String, size: DashSize, half: Boolean = false) = DashItem(field, size, half)

    /**
     * A layout using most of what the editor can do: every page kind, the
     * map dragged to the second slot, a radar rail at 75 % pinned to the
     * bottom, half rows (a pair, and a lone half that spans), all four sizes,
     * and a non-default map data strip. Six content pages.
     */
    private fun richConfig(): DashConfig = DashConfig(
        pages = listOf(
            Page.fields(DashLayout(listOf(
                item("power3s", DashSize.HERO),
                item("hr", DashSize.MEDIUM, half = true),
                item("cadence", DashSize.MEDIUM, half = true),
                item("speed", DashSize.LARGE),
                item("distance", DashSize.SMALL, half = true),   // lone half: spans
                DashItem("radar", DashSize.MEDIUM, false, vertical = true, heightPercent = 75, bottomAligned = true),
            ))),
            Page.map(),
            Page.workout(),
            Page.fields(DashLayout(listOf(
                item("clock", DashSize.SMALL, half = true),
                item("battery", DashSize.SMALL, half = true),
                item("altitude", DashSize.MEDIUM),
                item("grade", DashSize.MEDIUM, half = true),
                item("climb", DashSize.MEDIUM, half = true),
                item("routeleft", DashSize.LARGE),
            ))),
            Page.music(),
            Page.stopwatch(),
            Page(DashConfig.PageKind.SEGMENT, DashLayout(emptyList()), autoShow = false),
        ),
        mapFields = listOf("hr", "climb", "clock"),
    )

    // ---- dashboard ---------------------------------------------------------------------------

    /**
     * The editor's Send with a seven-page carousel: the device applies exactly
     * the bytes the editor serialized (DashConfig.configText is documented as
     * byte-compatible with dashSerializePages), has seven pages with the map
     * second, and the app's model of what the device holds is what the rider
     * built — nothing left "Syncing layout…".
     */
    @Test
    fun richLayoutFromTheEditorIsWhatTheDeviceShows() {
        val ble = connect()
        val config = richConfig()
        assertNotEquals("starts from something else", config.configText, deviceDash())

        send(ble, config)
        assertEquals("the device holds the editor's bytes", config.configText, deviceDash())
        waitForPageCount(7, "the carousel")
        assertEquals("the app's layout is the one sent", config, ble.dashConfig)
        assertEquals("the editor's target", config, ble.dashTarget)
        assertFalse("nothing left to sync", ble.hasPendingSync)
        // Each page kind and the map strip survive the app's parse of the echo.
        val got = ble.dashConfig!!
        assertEquals(config.pages.map { it.kind }, got.pages.map { it.kind })
        assertEquals(listOf("hr", "climb", "clock"), got.mapFields)
        assertFalse("segment page stays manual", got.pages.last().autoShow)
        val rail = got.pages[0].layout.verticalItem
        assertEquals("radar rail height", 75, rail?.heightPercent)
        assertEquals("radar rail pinned to the bottom", true, rail?.bottomAligned)

        // Ride-tab paging follows the carousel: slot 1 is the map now.
        assumeTrue("firmware without ShowPage", ble.has(Feature.FEATURE_SHOW_PAGE))
        ble.showDevicePage(1)
        waitFor("the device on its map page", 10_000) { status().get("screen").asString == "map" }
        ble.showDevicePage(3)
        waitFor("the device on the second data page", 10_000) {
            status().let { it.get("screen").asString == "dash" && it.get("page").asInt == 3 }
        }
    }

    /**
     * The layout lives on the device's card: after a reboot the device reads
     * it back from /config/dashboard.cfg, pushes it to the app on the new
     * link, and the app's parse of that text is the layout the rider sent.
     */
    @Test
    fun sentLayoutSurvivesADeviceRebootAndIsReadBack() {
        val ble = connect()
        val config = richConfig()
        send(ble, config)
        rebootAndReconnect(ble)
        assertEquals("the device read the layout back from its card", config.configText, deviceDash())
        waitForPageCount(7, "after the reboot")
        waitFor("the app's layout from the new link", 10_000) { ble.dashConfig == config }
        assertNull("no conflict raised", ble.dashConflict)
        assertFalse(ble.hasPendingSync)
    }

    /**
     * A rider who hand-edits dashboard.cfg on the card (USB drive) — compact,
     * with comments, a typo and a second radar — sees the same layout in the
     * app as on the panel: the app's forgiving parse of the device's
     * re-serialized text agrees with the device, and with the app's own parse
     * of the hand-written file.
     */
    @Test
    fun handEditedLayoutOnTheCardIsWhatTheAppShows() {
        val ble = connect()
        val hand = """
            # my layout
            map cadence hr distance
            speed hero
            powr large          # typo: skipped
            hr small half
            cadence small half
            radar medium vertical height=50
            radar medium vertical
            page music
            page
            distance large
            ridetime large
        """.trimIndent() + "\n"
        handEditCard(ble, hand)

        val dev = deviceDash()
        waitForPageCount(4, "speed, hr, cadence, one radar | music | distance, ridetime | map")
        waitFor("the app's layout from the device", 10_000) { ble.dashConfig?.configText == dev }
        val shown = ble.dashConfig!!
        assertEquals("the app's parse of the card's file agrees with the device", DashConfig.parse(hand), shown)
        assertEquals(listOf("cadence", "hr", "distance"), shown.mapFields)
        assertEquals(listOf("speed", "hr", "cadence", "radar"), shown.pages[0].layout.items.map { it.field })
        assertEquals("the one radar keeps its height", 50, shown.pages[0].layout.verticalItem?.heightPercent)
        assertTrue("map appended last", shown.pages.last().isMap)
    }

    /**
     * The device states its page limit (DeviceHello.dash_max_pages: seven
     * content pages, the map extra) and the editor's carousel is capped by it
     * (BleManager.dashMaxPages). Seven content pages with the map in the
     * middle — the most the editor builds — are all on the device.
     */
    @Test
    fun deviceTakesSevenContentPagesPlusTheMap() {
        val ble = connect()
        assertEquals("DeviceHello.dash_max_pages", 7, ble.deviceHello!!.dashMaxPages)
        assertEquals("what the editor offers", 7, ble.dashMaxPages)
        val pages = (1..7).map { n -> Page.fields(DashLayout(listOf(item(fieldN(n), DashSize.HERO)))) }.toMutableList<Page>()
        pages.add(3, Page.map())
        val config = DashConfig(pages)
        assertEquals(7, config.contentPages)
        send(ble, config)
        assertEquals(config.configText, deviceDash())
        waitForPageCount(8, "seven pages and the map")
        assertEquals(config, ble.dashConfig)
    }

    private fun fieldN(n: Int) = listOf("speed", "power", "hr", "cadence", "distance", "ridetime", "climb", "grade")[n - 1]

    /**
     * More data pages than the device holds — eight, from a hand edit of the
     * card (the editor's carousel never offers an eighth). The device says it
     * holds seven plus the map (DeviceHello.dash_max_pages), so it should keep
     * seven and its map, and the app should show the pages the panel has.
     */
    @Ignore(
        "BUG: a dashboard.cfg with 8 data pages loses the map screen. dashParsePages (src/dash_layout.cpp:256) " +
            "fills all DASH_MAX_PAGES (8) slots with data pages, so the `page map` line (or the implicit map, " +
            "dash_layout.cpp:337) finds no slot: the panel has 8 data pages and no map, although DeviceHello " +
            "promises 7 content pages + the map (src/link.cpp:1627). The app's DashConfig.parse caps at 7 and " +
            "re-adds a map, so the editor and the Ride tab show 7 pages + map while the panel has none, and " +
            "ShowPage to the map's slot lands on data page 8.",
    )
    @Test
    fun eightDataPagesOnTheCardKeepTheMapScreen() {
        val ble = connect()
        val text = (1..8).joinToString("page\n", postfix = "page map\n") { n -> "${fieldN(n)} hero\n" }
        handEditCard(ble, text)
        val dev = deviceDash()
        println("device after an 8-data-page card:\n$dev")
        assertTrue("the device still has its map page:\n$dev", dev.contains("page map"))
        waitForPageCount(8, "seven data pages and the map")
        waitFor("the app's layout from the device", 10_000) { ble.dashConfig != null }
        assertEquals("the app shows the pages the panel has", DashConfig.parse(dev).configText, ble.dashConfig!!.configText)
        assertEquals("the app's page count is the panel's", 8, ble.dashConfig!!.pages.size)
    }

    /**
     * A hand-written card file in compact form (no column padding) that fits
     * the device's 2 KB read, but whose padded re-serialization does not:
     * seven pages of twelve half fields. The panel uses it; the app must be
     * shown the same seven pages, not a cut-off copy.
     */
    @Ignore(
        "BUG: the app gets a cut-off copy of a layout whose serialization is over 2 KB. A compact hand-edited " +
            "dashboard.cfg (7 pages x 12 `<field> small half` lines, ~1.3 KB) parses and is drawn whole, but " +
            "sendDash (src/link.cpp:357) ignores dash_config::currentText's 0 return when dashSerializePages' padded " +
            "text (~2.2 KB) overflows DashConfig.text (2048, proto/link.options:36); snprintf leaves the first 2047 " +
            "bytes, so the app's editor shows page 7 with 8 of its 12 fields and no `page map` line, and a Send " +
            "from the editor would cut the rider's fields off the panel.",
    )
    @Test
    fun compactHandEditOfAFullCarouselReachesTheAppWhole() {
        val ble = connect()
        val page = twelveFields.joinToString("") { "$it small half\n" }
        val text = (1..7).joinToString("page\n", postfix = "page map\n") { page }
        assertTrue("fits the device's read (${text.length})", text.length < 2047)
        handEditCard(ble, text)
        waitForPageCount(8, "seven data pages and the map")
        waitFor("the app's layout from the device", 10_000) { ble.dashConfig != null }
        val shown = ble.dashConfig!!
        println("app's layout:\n${shown.configText}")
        assertEquals("pages in the app", 8, shown.pages.size)
        assertEquals("fields on the last data page", 12, shown.pages[6].layout.items.size)
    }

    /**
     * The rider removes every field from the only data page and taps Send:
     * the device has nothing to draw and refuses it (Result PARSE_ERROR), and
     * keeps showing its old layout. The app does not lose its copy of the
     * device's layout.
     */
    @Test
    fun layoutWithNothingToDrawIsRefusedAndTheDeviceKeepsItsOwn() {
        val ble = connect()
        val before = deviceDash()
        val beforeCount = status().get("page_count").asInt
        val empty = DashConfig(listOf(Page.fields(DashLayout(emptyList())), Page.map()))
        val seq = sim.logSeq()
        ble.saveDashConfig(empty)
        waitFor("the device refused it", 20_000) { sim.logSince(seq).any { "app layout rejected" in it } }
        runFor(2_000)
        assertEquals("the device kept its layout", before, deviceDash())
        assertEquals(beforeCount, status().get("page_count").asInt)
        assertEquals("the app's copy of the device's layout", before, ble.dashConfig?.configText)
    }

    /**
     * After the device refuses a layout (PARSE_ERROR), the app should show what
     * the device holds again — writeDash's and DashboardEditorSheet's own
     * comments say a rejected layout "corrects the editor instead of leaving
     * it out of step".
     */
    @Ignore(
        "BUG: a layout the device refuses (Result PARSE_ERROR) stays pending for good. " +
            "BleManager.writeDash (BleManager.kt:1954) calls dashWriteAcked(text, false), which only sets " +
            "dashWriteFailedThisLink (BleManager.kt:2040) and keeps settingsCache.pendingDash, so dashTarget " +
            "is the refused layout: the editor and the Ride tab's dashboard card show an empty page the panel " +
            "is not showing, the editor says \"Syncing layout…\" with no error, showDevicePage is disabled " +
            "while pendingDashConfig != null (BleManager.kt:4818) so Ride-tab paging stops, and every reconnect " +
            "sends the refused layout again.",
    )
    @Test
    fun refusedLayoutIsNotLeftPendingInTheEditor() {
        val ble = connect()
        val before = deviceDash()
        val seq = sim.logSeq()
        ble.saveDashConfig(DashConfig(listOf(Page.fields(DashLayout(emptyList())), Page.map())))
        waitFor("the device refused it", 20_000) { sim.logSince(seq).any { "app layout rejected" in it } }
        runFor(2_000)
        assertNull("the refused layout is not left pending", ble.pendingDashConfig)
        assertEquals("the editor shows what the device holds", before, ble.dashTarget?.configText)
    }

    /**
     * The editor allows seven pages of twelve fields, but the device takes at
     * most DeviceHello.dash_max_text bytes of layout. A full carousel of half
     * rows serializes past that, and the app must tell the rider rather than
     * sit on it.
     */
    @Ignore(
        "BUG: a layout over DeviceHello.dash_max_text (2047 bytes) is never sent and the rider is never told. " +
            "The editor allows 7 pages x 12 fields (DashboardEditorSheet PageCarousel maxPages, " +
            "DashboardPageEditor.kt:559 MAX_ITEMS) and a full carousel of half rows is ~2160 bytes; " +
            "BleManager.writeDash (BleManager.kt:1933) only logs \"not sent\" and sets dashWriteFailedThisLink, " +
            "so the layout stays pending with \"Syncing layout…\" forever and Ride-tab paging is disabled.",
    )
    @Test
    fun layoutOverTheDevicesTextLimitIsReportedNotLeftSyncing() {
        val ble = connect()
        val limit = ble.deviceHello!!.dashMaxText
        val full = DashConfig(
            (1..7).map {
                Page.fields(DashLayout(twelveFields.map { f -> item(f, DashSize.MEDIUM, half = true) }))
            } + Page.map(),
        )
        assertTrue("over the device's limit (${full.configText.length} > $limit)", full.configText.toByteArray().size > limit)
        val before = deviceDash()
        ble.saveDashConfig(full)
        runFor(6_000)
        assertEquals("the device kept its layout", before, deviceDash())
        assertNull("the over-size layout is not left pending", ble.pendingDashConfig)
    }

    private val twelveFields = listOf(
        "speed", "avgspeed", "power3s", "power", "hr", "cadence",
        "distance", "ridetime", "movingtime", "routeleft", "altitude", "battery",
    )

    // ---- settings ----------------------------------------------------------------------------

    private fun usbDriveOnDevice(): Boolean {
        val out = sim.cmd("console", "line" to "usbdrive").toString()
        return when {
            "[usbdrive] on" in out -> true
            "[usbdrive] off" in out -> false
            else -> error("usbdrive said: $out")
        }
    }

    /**
     * Every setting on the app's Settings screen reaches the device, is still
     * there after the device restarts, and is what the app shows on the new
     * link (nothing left pending, no "replaced" note).
     */
    @Test
    fun everySettingReachesTheDeviceAndSurvivesAReboot() {
        val ble = connect()
        assumeTrue("firmware without wheel size", ble.has(Feature.FEATURE_WHEEL_SIZE))
        val d0 = deviceSettings()
        // Values different from the device's defaults.
        val ftp = if (d0.get("ftp").asInt == 275) 280 else 275
        val tz = -300
        val miles = !d0.get("miles").asBoolean
        val bl = if (d0.get("backlight").asInt == 1) 2 else 1
        val clock24 = !d0.get("clock24h").asBoolean
        val usb = !usbDriveOnDevice()
        val wheel = 2150

        ble.updateFtp(ftp)
        ble.updateTz(tz)
        ble.updateUseMiles(miles)
        ble.updateBacklight(bl)
        ble.updateClock24h(clock24)
        ble.updateUsbDrive(usb)
        ble.updateWheelCircMm(wheel)

        fun deviceHasThem(): Boolean {
            val s = deviceSettings()
            return s.get("ftp").asInt == ftp && s.get("tz_min").asInt == tz && s.get("miles").asBoolean == miles &&
                s.get("backlight").asInt == bl && s.get("clock24h").asBoolean == clock24 && s.get("wheel_mm").asInt == wheel
        }
        waitFor("the device has every setting", 15_000) { deviceHasThem() && !ble.hasPendingSync }
        assertEquals("USB drive", usb, usbDriveOnDevice())

        rebootAndReconnect(ble)
        assertTrue("settings kept across the reboot: ${deviceSettings()}", deviceHasThem())
        assertEquals("USB drive kept across the reboot", usb, usbDriveOnDevice())
        waitFor("the app's settings from the new link", 10_000) { !ble.hasPendingSync }
        assertEquals(ftp, ble.ftpWatts)
        assertEquals(tz, ble.tzMinutes)
        assertEquals(miles, ble.useMiles)
        assertEquals(bl, ble.backlight)
        assertEquals(clock24, ble.clock24h)
        assertEquals(usb, ble.usbDrive)
        assertEquals(wheel, ble.wheelCircMm)
        assertNull("no sync note", ble.settingsSyncNote)
    }

    /**
     * Values the device won't take as typed: it clamps FTP to 50..500,
     * the time zone to -12..+14 h and the backlight to 0..3, and the app
     * clamps a wheel size to 800..3500 mm before sending. What the app shows
     * afterwards is what the device applied, not what was typed, and nothing
     * is left pending to be retried forever.
     */
    @Test
    fun outOfRangeSettingsShowWhatTheDeviceApplied() {
        val ble = connect()
        assumeTrue("firmware without wheel size", ble.has(Feature.FEATURE_WHEEL_SIZE))
        ble.updateFtp(9000)
        ble.updateTz(2000)
        ble.updateBacklight(9)
        ble.updateWheelCircMm(100)
        waitFor("the device's answer", 15_000) {
            val s = deviceSettings()
            s.get("ftp").asInt == 500 && s.get("tz_min").asInt == 840 && s.get("backlight").asInt == 3 &&
                s.get("wheel_mm").asInt == 800
        }
        waitFor("the app shows the device's values", 10_000) {
            ble.ftpWatts == 500 && ble.tzMinutes == 840 && ble.backlight == 3 && ble.wheelCircMm == 800
        }
        runFor(2_000)
        assertFalse("nothing left pending: ${ble.settingsCache.pending}", ble.hasPendingSync)
        assertEquals(500, ble.ftpWatts)
        assertEquals(840, ble.tzMinutes)
        assertEquals(3, ble.backlight)
        assertEquals(800, ble.wheelCircMm)

        // And the low side.
        ble.updateFtp(10)
        ble.updateTz(-2000)
        waitFor("the low clamps", 15_000) {
            deviceSettings().let { it.get("ftp").asInt == 50 && it.get("tz_min").asInt == -720 } &&
                ble.ftpWatts == 50 && ble.tzMinutes == -720 && !ble.hasPendingSync
        }
    }

    /**
     * Settings changed on the device itself (its serial console, which the
     * panel's own menus share) reach the app while connected: the wheel size
     * and the USB drive switch.
     */
    @Test
    fun deviceSideEditsReachTheApp() {
        val ble = connect()
        assumeTrue("firmware without wheel size", ble.has(Feature.FEATURE_WHEEL_SIZE))
        sim.cmd("console", "line" to "wheel 2150")
        waitFor("the app's wheel size", 10_000) { ble.wheelCircMm == 2150 }
        val usb = !ble.usbDrive
        sim.cmd("console", "line" to "usbdrive ${if (usb) "on" else "off"}")
        waitFor("the app's USB drive switch", 10_000) { ble.usbDrive == usb }
        // A refused value on the device changes nothing in the app.
        sim.cmd("console", "line" to "wheel 100")
        runFor(2_000)
        assertEquals(2150, deviceSettings().get("wheel_mm").asInt)
        assertEquals(2150, ble.wheelCircMm)
        assertFalse(ble.hasPendingSync)
        assertNull(ble.settingsSyncNote)
    }
}
