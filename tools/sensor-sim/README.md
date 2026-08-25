# sensor-sim — BLE sensor simulator (ESP32-C6)

One C6 pretends to be two peripherals via two extended-advertising instances,
each with its own fixed random static address:

| instance | name           | address             | services                    |
|----------|----------------|---------------------|-----------------------------|
| 0        | `Magene H64`   | `C6:5E:11:00:00:01` | HR 0x180D, Battery 0x180F   |
| 1        | `ASSIOMA12345` | `C6:5E:11:00:00:02` | Power 0x1818, Battery 0x180F|

DIS (0x180A) answers per-connection: Magene / H64 vs Favero Electronics /
Assioma DUO. Power measurement uses flags 0x0023 (balance + left ref + crank
revs) like a real Assioma DUO, so the bike computer derives cadence from it.

## Build / flash

```sh
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.4_py3.13_env   # export.sh picks py3.14 otherwise
. ~/esp-idf/export.sh
idf.py set-target esp32c6      # first time only
idf.py -p /dev/cu.usbmodem2101 build flash monitor
```

## Console (115200, USB serial JTAG)

```
auto                 scripted 3-min-on/3-min-off interval ride (default at boot)
hr 150 / pwr 250 / cad 90 / bal 52     pin a value (turns auto off)
cad 0                stopped crank (revs/time freeze -> cadence 0)
contact off          strap reports no skin contact
batt hr 20           battery level (notifies if subscribed)
off pwr / on pwr     drop the sensor (disconnect + stop advertising) / bring it back
status
```

## One board = one connectable sensor

A BLE controller accepts only one link per peer address, so a single central
(the bike computer, a phone, a Mac) can connect to **one** of this board's two
instances at a time — the second connect silently times out. Use two boards
(any ESP32 with BLE 5: C3/C6/S3/H2) and set a persisted role on each:

```
role hr      # this board is only the Magene strap
role pwr     # this board is only the Assioma
role both    # default; fine for testing one sensor at a time
```

## Web control panel (Web Serial)

```sh
python3 webui.py            # http://127.0.0.1:8931 — open in Chrome/Edge
```

The **page** owns the boards: click **Connect board…** once per board and pick its
"USB JTAG/serial debug unit" port (approved ports reconnect automatically on
reload). Nothing else on the Mac can hold that port while the page is connected.

Per board: status pills (*asleep / advertising / connected · streaming*), sliders for
HR / watts / cadence / balance / battery, **Sleep** = `off <dev>` (link dropped, no
advertising), **Wake** = `on <dev>`, **Auto ride**, the **flutter** checkbox
(`jitter on|off`: manual-mode values wander a little — HR ±1–2 bpm, power ~±5 %,
cadence ±1–2 rpm), the role dropdown, and a raw console line.

**Flash** (two clicks to arm) writes `build/` to that board with esptool-js over the
same Web Serial port; it refuses anything that isn't an ESP32-C6, so picking the bike
computer's port by mistake is harmless. A blank board shows as *Unknown board* with
the same Flash button. **Rebuild firmware** runs `idf.py build` on the server
(needs `~/esp-idf` and the py3.13 venv above).

Command line / scripting goes through the page (it relays):

```sh
python3 webui.py state              # every connected board's state
python3 webui.py cmd hr "hr 172"    # by role, or by board number: cmd 1 "cad 0"
curl -X POST localhost:8931/api/cmd -d '{"board":"pwr","cmd":"pwr 300"}'
```

### Web Serial notes (learned the hard way)

* Open the port with DTR **and** RTS de-asserted. On the ESP32's USB-JTAG unit those
  lines drive EN and the boot strap (that is how esptool resets it); with DTR asserted
  the console never answers and the board shows as *Unknown board*.
* esptool-js 0.6 takes image `data` as a `Uint8Array`. The 0.5-era binary string is
  accepted without complaint and lands in flash as all zeros (`invalid header: 0x00000000`
  on boot). Recover with `idf.py -p <port> flash` if that ever happens again.
* esptool-js reports `Flash ID: 0` on the C6 even though writes work and esptool.py sees
  the real ID (GD 8 MB) — ignore the warning. Uncompressed writes at 115200 take ~3.5 s
  for the whole image; compressed at 460800 dropped blocks (`status 201`).
