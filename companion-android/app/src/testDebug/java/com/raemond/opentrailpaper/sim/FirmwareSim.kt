package com.raemond.opentrailpaper.sim

import com.google.gson.JsonObject
import com.google.gson.JsonParser
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.ServerSocket
import java.net.Socket
import java.nio.file.Files

/**
 * The firmware simulator (sim/build/otp-sim) as a child process, and its JSON
 * control channel (sim/PROTOCOL.md) — the Kotlin twin of sim/client/otpsim/device.py.
 */
class FirmwareSim private constructor(
    val dir: File,
    val blePort: Int,
    val ctlPort: Int,
    /** Extra otp-sim options (e.g. `--lora`, `--conn-interval-ms 300 --packets-per-event 1`), kept across [launch]es. */
    val args: List<String> = emptyList(),
) : AutoCloseable {
    private var proc: Process? = null
    private var sock: Socket? = null
    private var reader: BufferedReader? = null
    private var writer: OutputStreamWriter? = null
    private var nextId = 1
    val consoleLog = File(dir, "console.log")

    companion object {
        /** sim/build/otp-sim above the test's working directory, or null. */
        val binary: File? by lazy {
            System.getenv("OTP_SIM")?.let { File(it) }?.takeIf { it.canExecute() }
                ?: generateSequence(File(System.getProperty("user.dir")).absoluteFile) { it.parentFile }
                    .map { File(it, "sim/build/otp-sim") }
                    .firstOrNull { it.canExecute() }
        }

        fun freePort(): Int = ServerSocket(0).use { it.localPort }

        /**
         * A free port can be taken by something else before otp-sim binds it;
         * then it exits with "can't listen", and we try again on new ports.
         */
        fun start(
            dir: File = Files.createTempDirectory("otp-sim-android").toFile(),
            args: List<String> = emptyList(),
        ): FirmwareSim {
            repeat(3) {
                val sim = FirmwareSim(dir, freePort(), freePort(), args)
                try {
                    sim.launch()
                    return sim
                } catch (e: IllegalStateException) {
                    sim.stop()
                    if (!sim.consoleTail().contains("can't listen")) throw e
                }
            }
            return FirmwareSim(dir, freePort(), freePort(), args).also { it.launch() }
        }
    }

    fun launch(bootTimeoutMs: Long = 60_000) {
        val bin = binary ?: error("sim/build/otp-sim not built")
        proc = ProcessBuilder(
            listOf(bin.path, "--dir", dir.path, "--ble-port", "$blePort", "--ctl-port", "$ctlPort", "--no-stdin") + args,
        ).redirectErrorStream(true).redirectOutput(ProcessBuilder.Redirect.appendTo(consoleLog)).start()
        // Never outlive the test JVM, however it ends.
        proc?.let { p -> Runtime.getRuntime().addShutdownHook(Thread { p.destroyForcibly() }) }
        attachControl(bootTimeoutMs)
    }

    /** Connect the control channel and wait for the firmware's boot to finish. */
    private fun attachControl(bootTimeoutMs: Long) {
        val deadline = System.currentTimeMillis() + bootTimeoutMs
        while (true) {
            try {
                val s = Socket("127.0.0.1", ctlPort)
                s.soTimeout = 90_000
                sock = s
                reader = BufferedReader(InputStreamReader(s.getInputStream(), Charsets.UTF_8))
                writer = OutputStreamWriter(s.getOutputStream(), Charsets.UTF_8)
                break
            } catch (e: java.io.IOException) {
                if (proc?.isAlive != true) error("otp-sim exited during boot (see $consoleLog)")
                if (System.currentTimeMillis() > deadline) throw e
                Thread.sleep(50)
            }
        }
        cmd("wait_log", "pattern" to "boot: all tasks started", "timeout_ms" to bootTimeoutMs, "since" to 0)
    }

    /**
     * The firmware restarts (control `reboot`: NVS and card kept). The sim
     * re-execs itself in the same process, closing every socket, so the
     * phone's link drops and the control channel is reattached to the new
     * image once it has booted (the Kotlin twin of device.py reboot()).
     */
    @Synchronized
    fun reboot(bootTimeoutMs: Long = 60_000) {
        cmd("reboot")
        // The old image answers, then closes everything 50 ms later.
        Thread.sleep(300)
        runCatching { sock?.close() }
        sock = null; reader = null; writer = null
        attachControl(bootTimeoutMs)
    }

    /**
     * After the firmware restarts itself (an OTA's reboot re-execs otp-sim on
     * the same ports), its control channel is a new one: connect again and
     * wait for the boot to finish.
     */
    @Synchronized
    fun reconnectControl(timeoutMs: Long = 30_000) {
        runCatching { sock?.close() }
        sock = null; reader = null; writer = null
        val deadline = System.currentTimeMillis() + timeoutMs
        while (true) {
            try {
                val s = Socket("127.0.0.1", ctlPort)
                s.soTimeout = 90_000
                sock = s
                reader = BufferedReader(InputStreamReader(s.getInputStream(), Charsets.UTF_8))
                writer = OutputStreamWriter(s.getOutputStream(), Charsets.UTF_8)
                break
            } catch (e: java.io.IOException) {
                if (proc?.isAlive != true) error("otp-sim exited (see $consoleLog)")
                if (System.currentTimeMillis() > deadline) throw e
                Thread.sleep(50)
            }
        }
        cmd("wait_log", "pattern" to "boot: all tasks started", "timeout_ms" to timeoutMs, "since" to 0)
    }

    @Synchronized
    fun cmd(name: String, vararg args: Pair<String, Any?>): JsonObject {
        val req = JsonObject()
        val id = nextId++
        req.addProperty("cmd", name)
        req.addProperty("id", id)
        for ((k, v) in args) when (v) {
            is Number -> req.addProperty(k, v)
            is Boolean -> req.addProperty(k, v)
            null -> Unit
            is com.google.gson.JsonElement -> req.add(k, v)
            else -> req.addProperty(k, v.toString())
        }
        val w = writer ?: error("control channel not open")
        w.write(req.toString() + "\n")
        w.flush()
        while (true) {
            val line = reader?.readLine() ?: error("control channel closed during $name\n${consoleTail()}")
            val msg = JsonParser.parseString(line).asJsonObject
            if (msg.get("id")?.asInt != id) continue
            if (msg.get("ok")?.asBoolean != true) error("$name: ${msg.get("error")}")
            return msg
        }
    }

    fun consoleTail(lines: Int = 30): String =
        runCatching { consoleLog.readLines().takeLast(lines).joinToString("\n") }.getOrDefault("")

    fun status(): JsonObject = cmd("status").getAsJsonObject("status")

    /**
     * The map tiles (or .poi files) on the card, id -> version record ("" for
     * none), as the device lists them to a phone (control `tiles`).
     */
    fun tiles(poi: Boolean = false): Map<String, String> =
        cmd("tiles", "poi" to poi).getAsJsonArray("tiles").associate {
            val o = it.asJsonObject
            o.get("id").asString to o.get("version").asString
        }

    /** The console's sequence number now, for [logSince]. */
    fun logSeq(): Long = cmd("seq").get("seq").asLong

    /** Console lines after [since]. */
    fun logSince(since: Long): List<String> =
        cmd("log", "since" to since).getAsJsonArray("lines").map { it.asJsonArray[1].asString }

    /** The radio link to the phone is lost (default: supervision timeout). */
    fun dropPhone(reason: Int = 0x08) { cmd("ble_drop", "reason" to reason) }

    // ---- the LoRa radio (start with args = listOf("--lora"); see MeshAir) ----------------------

    /** One frame the device's radio put on the air (control `lora_tx`). */
    class LoraFrame(val seq: Long, val startMs: Long, val endMs: Long, val freqMhz: Double, val bytes: ByteArray)

    /**
     * Puts [frame] on the air for the device, on the channel it is tuned to;
     * it arrives after its time on air. Returns the sim's answer (`lost`,
     * `start_ms`, `end_ms` in device time).
     */
    fun loraRx(frame: ByteArray, rssi: Int? = null, snr: Int? = null): JsonObject =
        cmd("lora_rx", "hex" to frame.joinToString("") { "%02x".format(it) }, "rssi" to rssi, "snr" to snr)

    /**
     * What the device transmitted after [since] (a seq), and the seq of the
     * last one returned. The sim logs a transmission as it starts; only those
     * finished (and re-armed for: the firmware's mesh task polls every
     * 250 ms) are returned, so a test that answers one is not answering into
     * a radio still transmitting. The rest come on a later call.
     */
    fun loraTx(since: Long = 0): Pair<List<LoraFrame>, Long> {
        val r = cmd("lora_tx", "since" to since)
        val now = r.get("now_ms").asLong
        val frames = r.getAsJsonArray("frames").map { it.asJsonObject }
            .takeWhile { it.get("end_ms").asLong + 300 <= now }
            .map { o ->
                val hex = o.get("hex").asString
                LoraFrame(
                    o.get("seq").asLong, o.get("start_ms").asLong, o.get("end_ms").asLong, o.get("freq_mhz").asDouble,
                    ByteArray(hex.length / 2) { i -> hex.substring(2 * i, 2 * i + 2).toInt(16).toByte() },
                )
            }
        return frames to (frames.lastOrNull()?.seq ?: since)
    }

    /** The radio's state and counters (control `lora`). */
    fun lora(): JsonObject = cmd("lora").getAsJsonObject("lora")

    /** The device goes away entirely (powered off): nothing listens on its ports. */
    fun stop() {
        val p = proc ?: return
        if (p.isAlive) {
            runCatching { cmd("quit") }
            if (!p.waitFor(3, java.util.concurrent.TimeUnit.SECONDS)) p.destroyForcibly().waitFor()
        }
        runCatching { sock?.close() }
        sock = null; reader = null; writer = null
        proc = null
    }

    override fun close() {
        stop()
        dir.deleteRecursively()
    }
}
