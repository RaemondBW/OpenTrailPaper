# Meshtastic mesh messaging

The T5 e-paper S3 **PRO** carries an SX1262 LoRa radio (the *Lite* board does
not). This turns it into a [Meshtastic](https://meshtastic.org) node, so the head
unit can send and receive text messages over the mesh with no phone signal
anywhere — the phone app is the keyboard and the screen, the device is the radio.

Messages you send reach ordinary Meshtastic nodes, and theirs reach you: the
implementation follows the real wire format rather than inventing a private one.

## What it does and does not do

**Does**

- Joins a Meshtastic channel (currently `MediumFast`, key 1 — see
  [Modem preset](#modem-preset)) and sends/receives `TEXT_MESSAGE_APP` messages.
- Broadcasts to the channel, or direct-messages one node.
- Acknowledges direct messages addressed to it, so the sender's app shows a
  delivery tick, and marks its own direct messages acknowledged when the reply
  arrives.
- Learns neighbour names from `NODEINFO_APP` and announces its own, so messages
  read as "Alex" rather than `!a4c1380c`.
- Keeps the last 48 messages and 32 neighbours on the device, so a phone that
  was away still sees what arrived while it was gone.

**Does not**

- **Rebroadcast** other people's traffic. The device is a leaf on the mesh, not a
  router. Adding flood routing would put every packet it hears back on the air —
  a real cost in battery and airtime on something whose day job is a bike
  computer.
- **Send position or telemetry.** The device knows exactly where you are; it does
  not tell the mesh.
- **PKI-encrypted direct messages** (Meshtastic 2.5+). The device publishes no
  public key, which is what makes peers fall back to the shared channel key when
  they message it — so direct messages still work, they are just channel-
  encrypted rather than end-to-end. A PKC packet meant for somebody else is
  counted as dropped and ignored.
- **Custom 32-byte channel keys.** Only Meshtastic's ten well-known keys (1–10)
  are selectable. A named channel with key 1 is the usual "separate channel on
  the same mesh" setup and that does work.

## Region

`MESH_REGION_NAME` and the frequency bounds in `src/config.h` are **compile-time
constants**, deliberately: which band the radio may use is a legal question, not
a preference, and a phone-selectable region would let a US-certified unit be
switched onto EU frequencies. The default is US (902–928 MHz).

To build for another region, change these in `config.h` and rebuild:

```c
#define MESH_REGION_NAME    "US"
#define MESH_FREQ_START_MHZ 902.0f
#define MESH_FREQ_END_MHZ   928.0f
#define MESH_SPACING_MHZ    0.0f
#define MESH_TX_DBM         22
```

Meshtastic's own region table is the reference for the numbers. Note the radio
must also be the right hardware variant — a 915 MHz module cannot be talked onto
868 MHz by software.

## How a channel becomes a frequency

This is the part worth understanding, because it explains why two devices that
"have Meshtastic" can still be completely deaf to each other. The channel **name**
feeds two different hashes:

1. **The frequency slot.** The band is divided into bandwidth-wide slots
   (US at 250 kHz → 104 of them). `djb2(name) % 104` picks one.
   `djb2("LongFast") = 130429955`, slot 19, centre **906.875 MHz**.
2. **The channel hash**, one byte stamped in the clear on every packet so a
   receiver knows which key to try: `xor(name) ^ xor(psk)`. For the default
   channel that is `0x0a ^ 0x02 = 0x08`.

So renaming the channel *retunes the radio*. A device on `LongFast` and one on
`MyTrail` are not on the same frequency at all, let alone the same key.

## Modem preset

The catch: a node with no explicit channel name uses the **preset name** as its
channel name. So the preset is not just a speed setting — it picks the frequency
and the channel byte too.

| Preset | SF | BW | Slot | Frequency (US) | Channel hash |
|---|---|---|---|---|---|
| `LongFast` | 11 | 250 | 19 | 906.875 MHz | `0x08` |
| `MediumFast` | 9 | 250 | 44 | **913.125 MHz** | `0x1f` |

`MESH_PRESET_NAME`, `MESH_SF`, `MESH_BW_KHZ` and `MESH_CR` in `src/config.h` move
together and must match the preset name exactly as Meshtastic spells it.

This firmware currently ships **MediumFast**. It is roughly 4x faster on the air
than LongFast, so a message occupies the channel for a fraction as long and costs
less battery to send, at the price of sensitivity and therefore range.

**Every node you want to talk to must be on the same preset.** A LongFast node is
not merely slower to reach — it is on a different frequency and cannot hear this
one at all.

Because the preset name is the default channel name, the device deliberately does
**not** persist it: `settings::meshChannel()` returns the compiled preset unless
the phone set an explicit channel. Storing a copy would pin a unit to whatever
preset it first booted with, and a firmware built for MediumFast would keep
talking LongFast on the wrong frequency with nothing in the log to explain it.

Both hashes and the modem parameters (sync word `0x2B`, 16-symbol preamble) are
checked against known-good values by
`tools/mesh_test/run_mesh_test.sh`. Run it after touching `src/mesh_proto.cpp` —
every value it covers fails *silently* on real hardware, as a radio that hears
nothing and is heard by nobody.

## Wire format

A packet is a 16-byte plaintext header followed by an AES-CTR encrypted payload:

```
 0  dest        u32 LE     0xFFFFFFFF = broadcast
 4  sender      u32 LE     the node number
 8  packet id   u32 LE     random, non-zero
12  flags       u8         hop_limit:0-2  want_ack:3  via_mqtt:4  hop_start:5-7
13  channel     u8         the channel hash above
14  next_hop    u8         0 = no hint
15  relay_node  u8         0 = unknown
16  payload…               AES-CTR(meshtastic.Data)
```

The counter block is `[packet id as u64 LE][sender as u32 LE][extra nonce u32]`,
which binds a payload to its sender and packet id — the reason a packet id must
never repeat under one key.

`meshtastic.Data`, `User` and `Routing` are hand-encoded in
`src/mesh_proto.cpp`. Generated nanopb bindings were not worth their weight for
four messages of scalar fields, and hand-rolling them keeps the whole codec
host-compilable and therefore testable.

## On the device

The SX1262 shares its SPI bus with the SD card and shares its 3V3 rail with the
GPS (one XL9555 expander pin gates both — never cut it to save radio power).
Two consequences shape `src/lora_radio.cpp`:

- Every radio SPI transfer takes the `sd_bus.h` lock, same as an SD access.
- A transmission is **split** — stage and start under the lock, then release it
  and watch the DIO1 interrupt for completion. A LongFast packet is up to ~2.5 s
  in the air, and the 1 Hz ride recorder cannot wait that long for the bus.

Before transmitting, the radio does a channel-activity scan and backs off with a
randomised delay. After eight busy scans it transmits anyway: a message the rider
typed is better off colliding than silently vanishing, and a scan that reports
busy forever is as likely to be a bad CAD threshold at SF11 as a saturated band.

Source map:

| File | Role |
|---|---|
| `src/mesh_proto.{h,cpp}` | wire format, protobufs, AES-CTR, both channel hashes |
| `src/lora_radio.{h,cpp}` | SX1262 via RadioLib, non-blocking, bus-shared |
| `src/mesh_service.{h,cpp}` | node identity, dedup, message ring, outbox, NodeDB |
| `src/ble_server.cpp` | the `b1c5000a-…` characteristic bridging it to the phone |
| `tools/mesh_test/` | host tests for everything interop depends on |

## Phone protocol

One characteristic, `b1c5000a-9e0f-4b7a-9c6d-1f2e3a4b5c6d`. Small and
text-shaped, so unlike the ride/map transfers it needs no windowing — one
notification per message, each inside the 247-byte MTU.

Phone → device:

| Op | Payload | Meaning |
|---|---|---|
| `0x01` | — | send me the state |
| `0x02` | `u32 dest` + UTF-8 | send a message (`ffffffff` = broadcast) |
| `0x03` | — | send me the message history |
| `0x04` | — | send me the node list |
| `0x05` | `u8 longLen` + long + short | rename this node |
| `0x06` | `u8 on` | radio on / off |
| `0x07` | `u8 keyIndex` + name | switch channel (retunes the radio) |
| `0x08` | — | mark everything read |
| `0x09` | — | send me the packet counters |

Device → phone (notify): `0x90` state, `0x91` one message, `0x92` end of
history, `0x93` one node, `0x94` end of node list, `0x95` counters, `0x96`
something changed (ask again), `0x97` queued with the packet id, `0x98` refused.

`0x96` exists so a new message costs one byte rather than a re-stream of the
whole history the app usually already has. Messages are keyed on packet id
because the history is rebuilt from scratch on each pull.

Timestamps come as **both** a UTC stamp and an age in milliseconds: until GPS
sets the clock the device does not know what time it is, and then the phone's own
clock minus the age is the only date the two ends can agree on.

## Battery

An idle receive is a few milliamps — small next to the panel and the GPS, but not
free. Turning the radio off from the app's Mesh tab sleeps the module (config
retained, so coming back is instant). Transmitting at 22 dBm is ~110 mA for the
duration of the packet, and a LongFast packet is seconds long, so chatting is
noticeably more expensive than listening.
