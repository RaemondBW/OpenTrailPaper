"""The mesh (Meshtastic-compatible group-ride messaging) against the sim's
LoRa radio (sim/host/lora_sim.cpp): what the device hears and shows the
phone, what it puts on the air, and what it must not.

Every frame here is built — and everything the device sends is read — by
otpsim.mesh, a Meshtastic codec written from Meshtastic's own definitions,
not from src/mesh_proto.cpp. So these tests are interoperability tests at
the level of the radio, where tools/mesh_test (the "Meshtastic wire format"
CI job) checks the codec's functions one by one: hashes, frequencies, key
expansion, AES/CCM vectors and protobuf layouts are not re-checked here."""
import os
import struct
import time

import pytest

from conftest import TIME_SCALE
from mesh_kit import BROADCAST, Mesh, is_port, keep_artifacts, lock_warnings, mesh, start_device  # noqa: F401
from otpsim import pb
from otpsim import mesh as M

ALEX = 0x1234ABCD
BEA = 0x0BEA0002


def alex(**kw):
    return M.VirtualNode(ALEX, "Alex Rivera", "ALX", **kw)


def bea(**kw):
    return M.VirtualNode(BEA, "Bea", "BEA", **kw)


# ---- the radio ---------------------------------------------------------------------------


def test_a_fresh_device_keeps_its_radio_silent_until_the_rider_turns_the_mesh_on(request):
    """Mesh is off out of the box: a rider who never asked for it must not be
    transmitting (and paying for idle receive). Turned on from the app, it
    listens exactly where an unconfigured stock node is — LongFast, US slot
    20, 906.875 MHz, SF11/250 kHz, sync word 0x2B — and introduces itself
    with a NodeInfo a stock node can read: its !id, names and public key,
    asking for replies so it relearns its neighbours' keys."""
    dev = start_device()
    try:
        m = Mesh(dev, radio_on=False)
        dev.set_speed(60)
        m.dev_sleep(40000)   # past the 20 s first announcement
        assert dev.lora()["up"] is False
        assert dev.lora_tx()[0] == [], "the radio transmitted while the mesh was off"
        assert not m.state().radio_ok

        dev.set_speed(1)
        m.radio(True, announce=False)
        tune = dev.lora()
        stock = M.tune_for("LongFast")
        assert abs(tune["freq_mhz"] - stock["freq_mhz"]) < 1e-3 and tune["freq_mhz"] == pytest.approx(906.875)
        assert (tune["sf"], tune["bw_khz"], tune["sync_word"]) == (11, 250, 0x2B)
        st = m.state()
        assert st.radio_ok and st.enabled and abs(st.freq_hz - 906875000) < 100

        hello = m.wait_sent(lambda h: is_port(h, M.PORT_NODEINFO), "a NodeInfo announcement", timeout=15)
        assert hello["dest"] == BROADCAST and hello["channel"] == M.Channel().hash
        assert hello["sender"] == m.num and hello["hop_start"] == hello["hop_limit"] > 0
        assert hello["data"]["want_response"], "the first announcement should ask neighbours to reply"
        user = M.decode_user(hello["data"]["payload"])
        assert user["id"] == "!%08x" % m.num
        assert user["long_name"] == st.long_name and user["short_name"] == st.short_name
        assert user["public_key"] == m.public_key and len(user["public_key"]) == 32
        assert user["hw_model"] == 0, "the board is not a Meshtastic hardware model and must not claim one"
        m.close()
    finally:
        keep_artifacts(request, dev)


def test_turning_the_mesh_off_silences_the_radio_and_refuses_sends(mesh):
    """Off means off: the module sleeps (hears nothing), anything queued is
    dropped rather than sent later, and a send from the app is refused with
    a reason instead of being shown as pending forever."""
    mesh.dev.lora(busy=True)                     # hold the outbox
    assert mesh.send("queued while busy").WhichOneof("body") == "mesh_queued"
    mesh.radio(False)
    mesh.dev.lora(busy=False)
    assert mesh.dev.lora()["mode"] == "sleep"
    r = mesh.send("hello?")
    assert r.WhichOneof("body") == "result" and r.result.code == pb.RESULT_REFUSED
    mesh.hear(alex().text("anyone?"))
    mesh.drain()
    mesh.dev_sleep(3000)
    assert all(not is_port(h, M.PORT_TEXT) for h in mesh.sent(since=0)), "a message went out after mesh off"
    assert not any(x.text == "anyone?" for x in mesh.history()), "a sleeping radio heard a message"
    assert mesh.dev.lora()["deaf"] >= 1


@pytest.mark.xfail(strict=True, reason=(
    "BUG: applyEnabled() (mesh_service.cpp) never sets `changed`, so turning the radio on or off is never "
    "followed by a mesh_changed push; the only state the app gets is the snapshot link.cpp onMesh sends "
    "straight after STAGING the change (radio_on -> MT_STATE), before the mesh task applies it. The app's "
    "Mesh screen keeps showing \"Mesh radio is off\" (or no radio) after the rider turned it on, until "
    "something else changes or the screen is reopened (found by the Android MeshSimTest too)"))
def test_the_app_hears_that_the_radio_came_on_without_asking_again(request):
    """The rider flips the radio switch in the app and looks at the screen:
    the app must learn that the radio is now up — from the state it is sent,
    or a "mesh changed" nudge to ask again — without polling."""
    dev = start_device()
    try:
        m = Mesh(dev, radio_on=False)
        m.link.drain_pushes()
        seen = []
        m.link.on_push(lambda msg: seen.append(msg))
        m.command(radio_on=True)
        m.dev_wait(lambda: dev.lora()["mode"] == "rx", "radio up")
        m.dev_sleep(1500)
        told = [x for x in seen if x.WhichOneof("body") == "mesh_changed" or
                (x.WhichOneof("body") == "mesh_state" and x.mesh_state.radio_ok)]
        assert told, "the app was left with: %s" % [str(x).replace("\n", " ") for x in seen]
        m.close()
    finally:
        keep_artifacts(request, dev)


@pytest.mark.parametrize("preset", M.PRESET_ORDER)
def test_each_modem_preset_tunes_where_stock_nodes_on_it_listen(mesh, preset):
    """With no channel name the preset's name IS the channel (a stock node
    leaves its primary unnamed), so each preset is its own frequency slot as
    well as its own modem. A device on any preset must hear a stock node on
    the same one."""
    idx = M.PRESET_ORDER.index(preset)
    if idx:
        mesh.command(preset=idx)
        mesh.dev_wait(lambda: mesh.dev.lora()["sf"] == M.PRESETS[preset][0] and mesh.dev.lora()["mode"] == "rx",
                      "retune to " + preset)
    stock = M.tune_for(preset)
    t = mesh.dev.lora()
    assert t["freq_mhz"] == pytest.approx(stock["freq_mhz"], abs=1e-3)
    assert (t["sf"], t["bw_khz"]) == (stock["sf"], stock["bw_khz"])
    ch = M.Channel(preset)
    node = alex(channel=ch)
    mesh.hear(node.text("on " + preset), **stock)
    mesh.drain()
    got = [x for x in mesh.history() if x.text == "on " + preset]
    assert got and got[0].channel == 0


def test_a_node_on_another_preset_or_channel_is_not_heard(mesh):
    """Same band, wrong slot or wrong modem: silence, as on the board — and
    the device must not count it as a damaged packet either."""
    node = alex(channel=M.Channel("MediumFast"))
    mesh.hear(node.text("wrong slot"), **M.tune_for("MediumFast"))
    lf = M.tune_for("LongFast")
    mesh.hear(alex().text("wrong modem"), **dict(lf, sf=9))
    mesh.drain()
    assert mesh.history() == []
    assert mesh.dev.lora()["mistuned"] == 2
    assert mesh.stats().rx_dropped == 0


# ---- hearing the mesh --------------------------------------------------------------------


def test_a_broadcast_reaches_the_phone_with_its_sender_signal_and_hop_count(mesh):
    """What the Mesh tab shows for a message: the text, who sent it (by name
    once their NodeInfo is known), how strong it was, and how many relays it
    took — hop_start minus hop_limit (here relayed twice). A sender too old
    to stamp hop_start (pre-2.3) gets no hop count rather than a wrong 0."""
    a = alex()
    mesh.hear(a.nodeinfo())
    mesh.hear(a.text("Regroup at the summit", hop_limit=1, hop_start=3), rssi=-112, snr=-7.25)
    mesh.hear(bea().text("old firmware", hop_limit=3, hop_start=0))
    mesh.drain()
    h = {x.text: x for x in mesh.history()}
    msg = h["Regroup at the summit"]
    assert (msg.__getattribute__("from"), msg.to, msg.outgoing) == (ALEX, BROADCAST, False)
    assert msg.HasField("hops") and msg.hops == 2
    assert (msg.rssi, msg.snr) == (-112, -7)
    assert msg.utc > 1_700_000_000, "the phone set the clock; the message should carry it"
    assert not h["old firmware"].HasField("hops")
    nodes = {n.num: n for n in mesh.nodes()}
    assert nodes[ALEX].long_name == "Alex Rivera" and nodes[ALEX].short_name == "ALX"
    assert nodes[ALEX].hops == 2 and nodes[ALEX].rssi == -112
    assert nodes[BEA].long_name == "", "no NodeInfo from Bea yet, so no name"
    st = mesh.state()
    assert st.unread == 2 and st.node_count == 2
    mesh.command(mark_read=pb.Empty())
    assert mesh.state().unread == 0


def test_a_message_too_long_for_the_screen_is_cut_at_200_bytes_not_dropped(mesh):
    """Meshtastic's own limit is ~228 bytes; the device keeps 200. A longer
    one must still arrive, cut, rather than vanish."""
    text = ("Long ride report: " + "x" * 300)[:220]
    mesh.hear(alex().text(text))
    mesh.drain()
    got = mesh.history()
    assert len(got) == 1 and got[0].text == text[:200]


def test_the_node_list_shows_names_positions_and_when_each_was_last_heard(mesh):
    """The Nodes sheet and the node map are built from this: names from
    NodeInfo, a position only for nodes that sent one (and not for one at
    0,0 — that is "no fix"), the precision a node chose to blur to, and an
    age that grows until the node is heard again."""
    a, b = alex(), bea()
    c = M.VirtualNode(0x0C0C0C0C, "Cam", "CAM")
    mesh.hear(a.nodeinfo())
    mesh.hear(a.position(45.70531, -121.52147, alt=312, sats=9))
    mesh.hear(b.nodeinfo())
    mesh.hear(b.position(45.71, -121.50, precision_bits=13))
    mesh.hear(c.position(0.0, 0.0))
    mesh.drain()
    nodes = {n.num: n for n in mesh.nodes()}
    na = nodes[ALEX]
    assert na.HasField("position")
    assert na.position.lat_e7 == 457053100 and na.position.lon_e7 == -1215214700
    assert (na.position.alt_m, na.position.sats) == (312, 9)
    assert nodes[BEA].position.precision_bits == 13
    assert not nodes[0x0C0C0C0C].HasField("position"), "a 0,0 position is no fix, not a place"

    age1 = na.age_ms
    mesh.dev_sleep(3000)
    na2 = {n.num: n for n in mesh.nodes()}[ALEX]
    assert na2.age_ms >= age1 + 2500, "last heard must age with device time"
    assert na2.position.age_ms >= 2500
    mesh.hear(a.text("still here"))
    mesh.drain()
    na3 = {n.num: n for n in mesh.nodes()}[ALEX]
    assert na3.age_ms < 2000, "hearing a node again resets its age"
    assert na3.position.age_ms >= 2500, "a text is not a new position"


def test_more_neighbours_than_the_table_holds_evicts_the_longest_silent(mesh):
    """A big group ride or a busy city mesh has more nodes than the 32 the
    device remembers. The table must stay at 32, the phone must get all of
    them (over several pages), and the nodes dropped must be the ones not
    heard from for longest — never someone who just spoke."""
    mesh.dev.set_speed(4)
    nums = [0x10000000 + i for i in range(40)]
    riders = [M.VirtualNode(n, "Rider %02d" % i, "R%02d" % i) for i, n in enumerate(nums)]
    for r in riders[:20]:
        mesh.hear(r.nodeinfo(), rssi=-90)
    # The first rider speaks up again before the table fills: of the 40, it
    # is no longer among the longest silent.
    mesh.hear(riders[0].text("I'm still here"))
    for r in riders[20:]:
        mesh.hear(r.nodeinfo(), rssi=-90)
    mesh.drain(timeout=40)
    mesh.dev.set_speed(1)
    nodes = mesh.nodes()
    have = {n.num for n in nodes}
    assert len(nodes) == 32 and len(have) == 32
    assert mesh.state().node_count == 32
    assert nums[0] in have, "evicted a node that had just sent a message"
    assert {n.num for n in nodes if n.long_name} == have, "a node lost its name"
    gone = set(nums) - have
    assert gone == set(nums[1:9]), "evicted %s, expected the eight longest silent" % sorted(gone)


def test_the_message_history_keeps_the_newest_48_in_order(mesh):
    """The phone pulls the history in pages of four. Past the 48 the device
    keeps, the oldest go first, and what is left arrives oldest-first and
    complete."""
    mesh.dev.set_speed(4)
    for i in range(53):
        mesh.deliver(alex().text("msg %02d" % i))
    mesh.drain()
    mesh.dev.set_speed(1)
    texts = [x.text for x in mesh.history()]
    assert texts == ["msg %02d" % i for i in range(5, 53)]


# ---- what is not ours --------------------------------------------------------------------


def test_a_channel_with_the_wrong_key_is_ignored_not_shown_as_garbage(mesh):
    """Two ways to hold the wrong key: a different key (the channel byte
    differs, so the frame is recognisably someone else's), and — 1 in 256
    for random keys — a different key with the SAME channel byte, which
    decrypts to noise. Neither may put a message, a phantom node or a
    position in front of the rider."""
    other = alex(channel=M.Channel("LongFast", b"\x02"))           # well-known key #2
    assert other.channel.hash != M.Channel().hash
    same_hash_key = bytearray(M.DEFAULT_PSK)
    same_hash_key[0] ^= 0x5A
    same_hash_key[1] ^= 0x5A                                       # xor unchanged
    clash = M.Channel("LongFast", bytes(same_hash_key))
    assert clash.hash == M.Channel().hash
    for i in range(10):
        mesh.deliver(other.text("private %d" % i, packet_id=0x100 + i))
    mesh.drain()
    spy = M.VirtualNode(0x5B15B15B, "Spy", "SPY", channel=clash)
    mesh.dev.set_speed(4)
    for i in range(60):
        frame = [spy.text("secret %d" % i, packet_id=0x200 + i), spy.nodeinfo(packet_id=0x300 + i),
                 spy.position(45.7, -121.5, packet_id=0x400 + i)][i % 3]
        mesh.deliver(frame)
    mesh.drain()
    mesh.dev.set_speed(1)
    assert mesh.history() == []
    assert mesh.nodes() == [], "a frame we could not decrypt created a node"
    s = mesh.stats()
    assert s.rx == 0 and s.rx_other_channel == 10 and s.rx_dropped == 60


def test_a_private_channel_from_a_share_link_is_heard_and_kept_apart(mesh):
    """Joining a ride group: the app adds the channel from its share link
    (name + 16- or 32-byte key) as a private slot. Its traffic shares the
    primary's frequency, is decrypted with its own key, is labelled with its
    slot, and a reply goes out on it — encrypted so that only its members
    (not the public channel) can read it."""
    key = bytes(range(1, 33))
    mesh.set_private(1, "Sunday Ride", key)
    group = M.Channel("Sunday Ride", key)
    ch = {c.index: c for c in mesh.device_channels()}
    assert ch[1].psk == key and ch[1].hash == group.hash
    mesh.hear(alex(channel=group).text("Meet at 8"))
    mesh.hear(bea().text("public chatter"))
    mesh.drain()
    by = {x.text: x.channel for x in mesh.history()}
    assert by == {"Meet at 8": 1, "public chatter": 0}

    q = mesh.send("On my way", channel=1)
    assert q.WhichOneof("body") == "mesh_queued"
    h = mesh.wait_sent(lambda h: h["id"] == q.mesh_queued.packet_id, "the group reply")
    assert h["channel"] == group.hash and h.get("channel_name") == "Sunday Ride"
    assert h["data"]["payload"] == b"On my way"
    public_only = M.VirtualNode(0x0EA20002, "Ear2", "EA2")
    assert public_only.decode(h["frame"])["data"] is None, "the public channel could read the group's message"


def test_sending_on_a_channel_slot_that_is_not_set_up_is_refused(mesh):
    """A stale channel index from the app (say, just deleted on another
    phone) must be refused, not sent on some other channel's key."""
    r = mesh.send("to nowhere", channel=3)
    assert r.WhichOneof("body") == "result" and r.result.code == pb.RESULT_REFUSED
    mesh.dev_sleep(2500)
    assert not any(is_port(h, M.PORT_TEXT) for h in mesh.sent())


@pytest.mark.xfail(strict=True, reason=(
    "BUG: deleting a private channel blanks its messages in place (mesh_service.cpp applyPrivateChannel: "
    "msgs[i] = Message()) but leaves msgCount alone, so messageAt() still returns them and the phone's "
    "history gets empty entries (id 0, from !00000000, utc 0, no text) — blank bubbles from \"!00000000\" "
    "stamped at the device's boot time, in the primary channel's conversation (channel 0), where the deleted "
    "channel's messages were"))
def test_deleting_a_private_channel_removes_its_messages_without_leaving_blanks(mesh):
    """Deleting a channel forgets what arrived on it ("leaving them readable
    after the key is gone would be a surprising kind of deleted") — and the
    rest of the conversation must be left as it was."""
    key = os.urandom(16)
    mesh.set_private(1, "Race Team", key)
    group = M.Channel("Race Team", key)
    mesh.hear(alex(channel=group).text("team 1"))
    mesh.hear(bea().text("public 1"))
    mesh.hear(alex(channel=group).text("team 2"))
    mesh.drain()
    assert len(mesh.history()) == 3
    mesh.command(delete_channel=1)
    mesh.dev_wait(lambda: all(c.index != 1 for c in mesh.device_channels()), "channel deleted")
    left = mesh.history()
    assert [(x.text, x.id != 0) for x in left] == [("public 1", True)]


@pytest.mark.xfail(strict=True, reason=(
    "BUG: chanForHash() (mesh_service.cpp) returns the FIRST channel whose 8-bit hash matches and "
    "handleFrame tries only that key; stock Meshtastic (Router::perhapsDecode) tries every channel with "
    "the hash. A private channel whose hash collides with the primary's (1 in 256) is then deaf: every "
    "message on it fails the primary's key and is dropped, silently"))
def test_a_private_channel_whose_hash_collides_with_the_primary_is_still_heard(mesh):
    """The channel byte is an 8-bit hint, not an identity: two of the
    rider's channels can share one. The primary winning a tie is fine;
    the other channel's traffic must still be tried with its own key."""
    # A name whose hash, with a fresh 16-byte key, equals the primary's 0x08.
    key = bytearray(16)
    key[0] = M.xor_hash(b"Gravel Crew") ^ M.Channel().hash
    key = bytes(key)
    crew = M.Channel("Gravel Crew", key)
    assert crew.hash == M.Channel().hash
    mesh.set_private(1, "Gravel Crew", key)
    mesh.hear(alex(channel=crew).text("crew only"))
    mesh.drain()
    assert [(x.text, x.channel) for x in mesh.history()] == [("crew only", 1)]


# ---- flooding, duplicates and acks ----------------------------------------------------------


def test_a_flooded_message_is_shown_once_and_never_relayed_or_acked(mesh):
    """The mesh floods: a broadcast arrives once directly and again from each
    relay (hop_limit decremented, relay byte rewritten). The rider sees it
    once; the device, a leaf, never puts it back on the air — not as a
    rebroadcast and not as an ack even though it asked for one (acking a
    broadcast would make every listener transmit at once)."""
    a = alex()
    pid = 0xA11CE001
    original = a.text("Flat tire at mile 12", packet_id=pid, hop_limit=3, want_ack=True)
    mesh.hear(original)
    for hl, relay in ((2, 0x11), (1, 0x22), (2, 0x33)):
        f = bytearray(original)
        f[12] = (f[12] & ~7) | hl
        f[15] = relay
        mesh.hear(bytes(f))
    mesh.drain()
    mesh.dev_sleep(4000)
    texts = [x.text for x in mesh.history()]
    assert texts == ["Flat tire at mile 12"]
    assert mesh.stats().rx_duplicate == 3
    for h in mesh.sent(since=0):
        assert h["id"] != pid, "the device rebroadcast someone else's packet"
        assert not is_port(h, M.PORT_ROUTING), "the device acked a broadcast"


def test_a_direct_message_is_acked_once_with_a_reply_that_can_get_back(mesh):
    """A DM that wants an ack gets exactly one ROUTING ack, addressed to the
    sender, carrying its packet id, on its channel, with enough hops to get
    back (hops it used + 2, Meshtastic's getHopLimitForResponse). Copies of
    it from relays must not trigger more acks."""
    a = alex()
    mesh.hear(a.nodeinfo(with_key=False))
    mesh.drain()
    pid = 0xD1EC7001
    dm = a.text("you there?", dest=mesh.num, packet_id=pid, hop_limit=5, hop_start=6, want_ack=True)
    mesh.hear(dm)
    f = bytearray(dm)
    f[12] = (f[12] & ~7) | 4
    mesh.hear(bytes(f))
    mesh.drain()
    ack = mesh.wait_sent(lambda h: is_port(h, M.PORT_ROUTING), "an ack")
    assert ack["dest"] == ALEX and ack["data"]["request_id"] == pid
    assert M.decode_routing(ack["data"]["payload"]) == M.ROUTING_NONE
    assert ack["hop_limit"] == 3 and not ack["want_ack"]
    mesh.dev_sleep(4000)
    more = [h for h in mesh.sent() if is_port(h, M.PORT_ROUTING)]
    assert more == [], "a relayed copy of the DM was acked again"
    got = [x for x in mesh.history() if x.text == "you there?"]
    assert len(got) == 1 and got[0].to == mesh.num


def test_a_direct_message_nobody_acknowledges_is_sent_three_times_then_fails(mesh):
    """A DM lost on the air (a collision with the addressee's own
    announcement is the usual way) is sent again — the identical bytes, a
    few seconds apart, three sends in all: ReliableRouter's retransmission.
    The tick stays "sent" meanwhile and turns FAILED only when the last send
    too gets no ack."""
    a = alex()
    mesh.hear(a.nodeinfo(with_key=False))
    mesh.drain()
    pid = mesh.send("you there?", dest=ALEX).mesh_queued.packet_id
    copies = [mesh.wait_sent(lambda h: h["id"] == pid, "the DM")]
    for n in (2, 3):
        again = mesh.wait_sent(lambda h: h["id"] == pid, "send %d of the DM" % n, timeout=12)
        assert again["frame"] == copies[0]["frame"], "the resend was not the same bytes"
        copies.append(again)
    gaps = [y["tx"]["start_ms"] - x["tx"]["start_ms"] for x, y in zip(copies, copies[1:])]
    assert all(4000 <= g <= 9000 for g in gaps), gaps

    def status():
        return {x.id: x.delivery for x in mesh.history()}[pid]
    assert status() == pb.MESH_DELIVERY_SENT
    mesh.dev_wait(lambda: status() == pb.MESH_DELIVERY_FAILED, "the message to be given up", timeout=12)
    mesh.dev_sleep(3000)
    assert [h for h in mesh.sent() if h["id"] == pid] == [], "sent a fourth time"


def test_the_senders_own_resend_of_a_direct_message_is_acked_again(mesh):
    """A second copy of a DM straight from its sender — hop counts untouched —
    means our ack did not reach it, so it gets another. (A relay's copy does
    not: the acked-once test above.) The message is still shown once."""
    a = alex()
    mesh.hear(a.nodeinfo(with_key=False))
    mesh.drain()
    pid = 0xD1EC7002
    dm = a.text("still there?", dest=mesh.num, packet_id=pid, hop_limit=3, hop_start=3, want_ack=True)
    for what in ("the ack", "the ack again"):
        mesh.hear(dm)
        mesh.drain()
        ack = mesh.wait_sent(lambda h: is_port(h, M.PORT_ROUTING) and h["data"]["request_id"] == pid, what)
        assert ack["dest"] == ALEX
    assert len([x for x in mesh.history() if x.text == "still there?"]) == 1


def test_an_ack_from_the_addressee_marks_the_riders_message_delivered(mesh):
    """The app's tick: a DM the phone sent is PENDING, then SENT once on the
    air, then ACKED when the addressee's routing ack (request_id = its
    packet id) comes back. A NAK makes it FAILED. A broadcast stays SENT —
    nobody in particular acknowledges it."""
    a = alex()
    mesh.hear(a.nodeinfo(with_key=False))   # no key: a legacy channel-encrypted DM
    mesh.drain()
    q1 = mesh.send("Are you at the cafe?", dest=ALEX).mesh_queued.packet_id
    q2 = mesh.send("Group: leaving in 5").mesh_queued.packet_id
    dm = mesh.wait_sent(lambda h: h["id"] == q1, "the DM")
    assert dm["dest"] == ALEX and dm["want_ack"] and dm["data"]["payload"] == b"Are you at the cafe?"
    mesh.wait_sent(lambda h: h["id"] == q2, "the broadcast")

    def status(pid):
        return {x.id: x.delivery for x in mesh.history()}[pid]
    mesh.dev_wait(lambda: status(q1) == status(q2) == pb.MESH_DELIVERY_SENT, "both to show sent")
    mesh.hear(a.ack(mesh.num, q1))
    mesh.drain()
    assert status(q1) == pb.MESH_DELIVERY_ACKED and status(q2) == pb.MESH_DELIVERY_SENT
    assert mesh.stats().acks_rx == 1

    q3 = mesh.send("second try", dest=ALEX).mesh_queued.packet_id
    mesh.wait_sent(lambda h: h["id"] == q3, "the second DM")
    mesh.hear(a.ack(mesh.num, q3, error=M.ROUTING_NO_RESPONSE))
    mesh.drain()
    assert status(q3) == pb.MESH_DELIVERY_FAILED


def test_a_dm_to_a_node_without_its_key_goes_out_the_legacy_way_and_asks_for_the_key(mesh):
    """Before the device knows a node's public key it can only send the old
    channel-encrypted DM; it also asks that node for its NodeInfo (directly,
    want_response) so the next DM can be PKI-encrypted as current
    Meshtastic requires."""
    mesh.hear(alex().nodeinfo(with_key=False))
    mesh.drain()
    # At most one NodeInfo a minute, and the device has just announced itself.
    mesh.dev.set_speed(10)
    mesh.dev_sleep(61 * 1000)
    mesh.dev.set_speed(1)
    pid = mesh.send("hello Alex", dest=ALEX).mesh_queued.packet_id
    dm = mesh.wait_sent(lambda h: h["id"] == pid, "the DM")
    assert dm["channel"] == M.Channel().hash and not dm["pki"]
    ask = mesh.wait_sent(lambda h: is_port(h, M.PORT_NODEINFO) and h["dest"] == ALEX, "a NodeInfo request")
    assert ask["data"]["want_response"]


def test_dms_with_a_node_whose_key_is_known_are_pki_encrypted_both_ways(mesh):
    """Meshtastic 2.5+ end-to-end DMs: once the device has a node's public
    key (from its NodeInfo) a DM to it is AES-CCM sealed with the X25519
    shared secret (channel byte 0) — readable by that node and nobody else
    on the channel — and a PKI DM from it is decrypted, shown and acked."""
    a = alex()
    mesh.hear(a.nodeinfo())
    mesh.drain()
    pid = mesh.send("PKI hello", dest=ALEX).mesh_queued.packet_id
    dm = mesh.wait_sent(lambda h: h["id"] == pid, "the DM", ear=a, pki_from=mesh.public_key)
    assert dm["pki"] and dm["channel"] == 0 and dm["data"]["payload"] == b"PKI hello"
    eve = M.VirtualNode(0xEEEEEEEE, "Eve", "EVE")
    assert eve.decode(dm["frame"], pki_from=mesh.public_key)["data"] is None

    mesh.deliver(a.ack(mesh.num, pid, pki_to=mesh.public_key))
    mesh.hear(a.text("PKI reply", dest=mesh.num, pki_to=mesh.public_key, want_ack=True, packet_id=0x0F0F0F0F))
    mesh.drain()
    h = {x.text: x for x in mesh.history()}
    assert h["PKI hello"].delivery == pb.MESH_DELIVERY_ACKED
    assert h["PKI reply"].__getattribute__("from") == ALEX
    ack = mesh.wait_sent(lambda h: is_port(h, M.PORT_ROUTING) and h["data"]["request_id"] == 0x0F0F0F0F,
                         "an ack for the PKI DM", ear=a, pki_from=mesh.public_key)
    assert ack["dest"] == ALEX


def test_a_pki_dm_from_a_node_whose_key_the_device_lost_asks_for_it_and_then_gets_through(mesh):
    """Neighbours' keys live in RAM, so after a reboot a peer's PKI DM can't
    be read. The device must say so (a PKI_UNKNOWN_PUBKEY NAK, only because
    the DM wanted an ack), show nothing garbled, and read the retry once
    the peer has answered with its NodeInfo."""
    a = alex()
    mesh.hear(a.text("can you read this?", dest=mesh.num, pki_to=mesh.public_key, want_ack=True,
                     packet_id=0x0BAD0001))
    mesh.hear(a.text("no ack wanted", dest=mesh.num, pki_to=mesh.public_key, packet_id=0x0BAD0002))
    mesh.drain()
    nak = mesh.wait_sent(lambda h: is_port(h, M.PORT_ROUTING), "a NAK")
    assert nak["dest"] == ALEX and nak["data"]["request_id"] == 0x0BAD0001
    assert M.decode_routing(nak["data"]["payload"]) == M.ROUTING_PKI_UNKNOWN_PUBKEY
    mesh.dev_sleep(3000)
    assert not [h for h in mesh.sent() if is_port(h, M.PORT_ROUTING)], "NAKed a DM that wanted no ack"
    assert mesh.history() == []
    assert mesh.stats().rx_dropped == 2

    mesh.deliver(a.nodeinfo(dest=mesh.num))
    mesh.hear(a.text("can you read this?", dest=mesh.num, pki_to=mesh.public_key, want_ack=True,
                     packet_id=0x0BAD0003))
    mesh.drain()
    assert [x.text for x in mesh.history()] == ["can you read this?"]


def test_a_node_cannot_take_over_another_nodes_identity_or_ours(mesh):
    """Keys are pinned on first sight, as NodeDB::updateUser does: a later
    NodeInfo for the same node with a different key is ignored whole (it
    can't rename the node either), and a NodeInfo claiming the DEVICE's own
    public key is never adopted. DMs keep going to the real key."""
    a = alex()
    mesh.hear(a.nodeinfo())
    mesh.drain()
    impostor = M.VirtualNode(ALEX, "Not Alex", "BAD", private_key=os.urandom(32))
    mesh.hear(impostor.nodeinfo())
    copycat = M.VirtualNode(0xC0C0C0C0, "Copycat", "CC")
    mesh.hear(copycat.nodeinfo(public_key=mesh.public_key))
    mesh.drain()
    nodes = {n.num: n for n in mesh.nodes()}
    assert nodes[ALEX].long_name == "Alex Rivera"
    log = "\n".join(mesh.dev.log())
    assert "changed its public key" in log and "advertises our own public key" in log
    pid = mesh.send("only for Alex", dest=ALEX).mesh_queued.packet_id
    dm = mesh.wait_sent(lambda h: h["id"] == pid, "the DM", ear=a, pki_from=mesh.public_key)
    assert dm["pki"] and dm["data"]["payload"] == b"only for Alex"
    assert impostor.decode(dm["frame"], pki_from=mesh.public_key)["data"] is None
    # A DM to the copycat cannot be PKI (we never took its "key").
    pid2 = mesh.send("hi copycat", dest=0xC0C0C0C0).mesh_queued.packet_id
    dm2 = mesh.wait_sent(lambda h: h["id"] == pid2, "the DM to the copycat")
    assert not dm2["pki"] and dm2["channel"] == M.Channel().hash


def test_nodeinfo_requests_are_answered_but_cannot_make_the_device_chatter(mesh):
    """How a node that just booted learns our name and key: it asks, we
    answer (to it, request_id = its packet id). But a busy mesh must not be
    able to make us transmit on demand: one answer per asker per 12 h to a
    broadcast ask, at most one reply every 10 min to broadcasts, and every
    60 s to a request addressed to us."""
    mesh.dev_sleep(500)
    mesh.wait_sent(lambda h: is_port(h, M.PORT_NODEINFO), "the boot announcement", timeout=15)
    mesh.dev.set_speed(200)
    # The announcement counts as our last NodeInfo: wait out the 10 min gap.
    mesh.dev_sleep(11 * 60 * 1000)
    mesh.dev.set_speed(1)
    a, b = alex(), bea()
    mesh.hear(a.nodeinfo(want_response=True, packet_id=0xAAAA0001))
    mesh.drain()
    r = mesh.wait_sent(lambda h: is_port(h, M.PORT_NODEINFO), "an answer to Alex")
    assert r["dest"] == ALEX and r["data"]["request_id"] == 0xAAAA0001
    mesh.hear(a.nodeinfo(want_response=True, packet_id=0xAAAA0002))
    mesh.hear(b.nodeinfo(want_response=True, packet_id=0xBBBB0001))
    mesh.drain()
    mesh.dev_sleep(5000)
    assert not [h for h in mesh.sent() if is_port(h, M.PORT_NODEINFO)], "answered inside the rate limit"
    # Bea asks us directly after a minute: answered.
    mesh.dev.set_speed(30)
    mesh.dev_sleep(61 * 1000)
    mesh.dev.set_speed(1)
    mesh.hear(b.nodeinfo(dest=mesh.num, want_response=True, packet_id=0xBBBB0002, want_ack=True))
    mesh.drain()
    r = mesh.wait_sent(lambda h: is_port(h, M.PORT_NODEINFO), "a direct answer to Bea")
    assert r["dest"] == BEA and r["data"]["request_id"] == 0xBBBB0002
    mesh.dev_sleep(3000)
    assert not [h for h in mesh.sent() if is_port(h, M.PORT_ROUTING)], \
        "the reply is the ack; a separate ack doubles the airtime"
    assert mesh.stats().rx_duplicate == 0


# ---- damaged and hostile frames -----------------------------------------------------------


def test_malformed_frames_neither_crash_the_device_nor_invent_nodes(mesh):
    """At the edge of range, in a noisy band, or from a buggy or hostile
    node, frames arrive that pass the radio's CRC but are nonsense. None may
    take the device down, create a phantom neighbour or a blank message —
    and the next good frame must still get through."""
    ch = M.Channel()
    a = alex()

    def enc(data, pid, sender=ALEX, **kw):
        return M.header(BROADCAST, sender, pid, channel=ch.hash, **kw) + ch.encrypt(sender, pid, data)

    long_name = "N" * 120
    frames = [
        b"\x01",                                                        # runt
        bytes(15),                                                      # one short of a header
        M.header(BROADCAST, ALEX, 1, channel=ch.hash),                  # header, no payload
        enc(M.data_msg(M.PORT_TEXT, b"from nobody"), 2, sender=0),      # sender 0
        enc(b"\x0a\xff\xff\xff\xff\x0f", 3),                            # length past the end
        enc(b"\x08", 4),                                                # truncated varint
        enc(b"\x08\x01\x12\x05ab", 5),                                  # payload cut short
        enc(b"\x0f\x01\x02\x03", 6),                                    # wire type 7
        enc(b"\x08" + b"\xff" * 12, 7),                                 # varint > 64 bits
        enc(M.data_msg(0, b"no port"), 8),                              # portnum 0
        enc(M.data_msg(M.PORT_NODEINFO, M.user_msg(ALEX, long_name, "S" * 30, public_key=b"\x01" * 31)), 10),
        enc(M.data_msg(M.PORT_NODEINFO, b"\x12\xff\x01"), 11),          # User truncated
        enc(M.data_msg(M.PORT_POSITION, M.position_msg(123.0, 500.0)), 12),   # out of range
        enc(M.data_msg(M.PORT_POSITION, b"\x0d\x01\x02"), 13),          # sfixed32 cut short
        enc(M.data_msg(M.PORT_ROUTING, b"\x18"), 14),                   # routing truncated
        enc(M.data_msg(M.PORT_TELEMETRY, os.urandom(40)), 15),          # an app we don't speak
        enc(os.urandom(200), 16),                                       # noise on our channel
        M.header(mesh.num, ALEX, 17, channel=0) + os.urandom(64),       # "PKI" noise for us
        M.header(mesh.num, ALEX, 18, channel=0) + os.urandom(5),        # PKI shorter than its overhead
        os.urandom(255),                                                # the longest frame, all noise
        M.header(BROADCAST, ALEX, 21, channel=ch.hash) + bytes(239),     # the longest, zeros
    ]
    for f in frames:
        mesh.hear(f)
    mesh.hear(a.text("damaged", packet_id=19), crc_error=True)
    mesh.drain(timeout=60)
    assert mesh.dev.proc.poll() is None, "the device died"
    assert mesh.dev.lora()["crc"] == 1
    hist = mesh.history()
    assert [x.text for x in hist] == [], "a malformed frame became a message: %s" % hist
    nodes = mesh.nodes()
    assert all(n.num in (ALEX,) for n in nodes), "phantom nodes: %s" % [hex(n.num) for n in nodes]
    for n in nodes:
        assert len(n.long_name) <= 39 and len(n.short_name) <= 7
        assert not n.HasField("position")
    mesh.hear(a.text("good frame after all that", packet_id=20))
    mesh.drain()
    assert [x.text for x in mesh.history()] == ["good frame after all that"]


# ---- the transmitter ------------------------------------------------------------------------


def test_a_busy_channel_delays_a_message_but_never_loses_it(mesh):
    """Listen-before-talk: while channel-activity detection says busy, the
    device backs off; if it never clears (a CAD threshold that's wrong at
    SF11 looks exactly like this) the message goes anyway after 8 tries
    rather than silently vanishing."""
    mesh.dev.lora(busy=True)
    pid = mesh.send("flat tire, going back").mesh_queued.packet_id
    mesh.dev_sleep(1500)
    assert not [h for h in mesh.sent(since=0) if h["id"] == pid]
    assert {x.id: x.delivery for x in mesh.history()}[pid] == pb.MESH_DELIVERY_PENDING
    h = mesh.wait_sent(lambda h: h["id"] == pid, "the message, sent anyway", timeout=20)
    assert mesh.dev.lora()["cad_busy"] >= 8
    assert "sending anyway" in "\n".join(mesh.dev.log())
    assert h["data"]["payload"] == b"flat tire, going back"
    mesh.dev.lora(busy=False)


def test_a_full_outbox_refuses_the_next_message_instead_of_dropping_one(mesh):
    """Six frames wait at most. A seventh send is refused (the app says
    "radio off or outbox full") — and the six already queued all go out
    once the channel clears, in order."""
    mesh.dev.lora(busy=True)
    ids = []
    for i in range(6):
        r = mesh.send("queued %d" % i)
        assert r.WhichOneof("body") == "mesh_queued", r
        ids.append(r.mesh_queued.packet_id)
    r = mesh.send("one too many")
    assert r.WhichOneof("body") == "result" and r.result.code == pb.RESULT_REFUSED
    mesh.dev.lora(busy=False)
    out = []
    deadline = time.time() + 30 * TIME_SCALE
    while len(out) < 6 and time.time() < deadline:
        out += [h["id"] for h in mesh.sent() if is_port(h, M.PORT_TEXT)]
        time.sleep(0.2)
    assert out == ids
    gaps = [b["tx"]["start_ms"] - a["tx"]["end_ms"] for a, b in zip(
        mesh.sent(since=0)[:-1], mesh.sent(since=0)[1:])]
    assert min(gaps) >= 1500, "frames closer than the 1.5 s politeness gap: %s" % gaps


def slow_clock(mesh, speed=0.1):
    """For a test of the radio's turnaround (tens of ms on the board, where
    the DIO1 interrupt wakes the mesh task in microseconds). The sim's clock
    is real time, so a host thread that wakes late — a loaded CI runner — is
    a late wake on the device too. Run slower, the host's latency is a small
    part of the device's window again."""
    mesh.dev.set_speed(speed)


def turnaround(mesh):
    """For a failure message: interrupts fired, and how late (device ms) the
    receiver came back after our own frames — the number these tests are about."""
    r = mesh.dev.lora()
    return {k: r[k] for k in ("irqs", "turn_max_ms", "turn_last_ms", "tx")}


def test_back_to_back_packets_on_a_fast_preset_are_both_received(mesh):
    """Two riders answering at once on ShortFast: their packets follow each
    other on the channel within tens of milliseconds. Both messages must
    reach the rider."""
    mesh.command(preset=M.PRESET_ORDER.index("ShortFast"))
    mesh.dev_wait(lambda: mesh.dev.lora()["sf"] == 7 and mesh.dev.lora()["mode"] == "rx", "retune")
    slow_clock(mesh)
    sf = M.Channel("ShortFast")
    for i in range(4):
        mesh.hear(alex(channel=sf).text("alex %d" % i))
        mesh.hear(bea(channel=sf).text("bea %d" % i))
        mesh.drain()
    got = sorted(x.text for x in mesh.history())
    assert got == sorted(["alex %d" % i for i in range(4)] + ["bea %d" % i for i in range(4)]), \
        "lost to overrun: %d; radio: %s" % (mesh.dev.lora()["overrun"], turnaround(mesh))


def test_an_ack_sent_straight_back_after_the_dm_is_heard(mesh):
    """The addressee answers as soon as it has the DM: its ack can start
    moments after the DM ends. The device must be listening by then."""
    a = alex()
    mesh.hear(a.nodeinfo(with_key=False))
    mesh.drain()
    slow_clock(mesh)
    acked = []
    for i in range(3):
        pid = mesh.send("quick %d" % i, dest=ALEX).mesh_queued.packet_id
        deadline = time.time() + 15 * TIME_SCALE
        while True:
            r = mesh.dev.cmd("lora_tx", since=mesh.tx_seq)
            f = [x for x in r["frames"] if bytes.fromhex(x["hex"])[8:12] == pid.to_bytes(4, "little")]
            if f:
                break
            assert time.time() < deadline, "the DM never went out"
            time.sleep(0.02)
        mesh.tx_seq = r["seq"]
        # The ack starts 10 ms after the DM's last symbol.
        mesh.hear(a.ack(mesh.num, pid), delay_ms=max(0, f[0]["end_ms"] - r["now_ms"]) + 10)
        mesh.drain()
        mesh.dev_sleep(2000)
        acked.append({x.id: x.delivery for x in mesh.history()}[pid] == pb.MESH_DELIVERY_ACKED)
    assert all(acked), "acks heard: %s; lost while deaf: %d; radio: %s" % (acked, mesh.dev.lora()["deaf"], turnaround(mesh))


def test_changing_channel_forgets_the_old_mesh_but_power_changes_do_not(mesh):
    """A new channel (or preset, or region) is a different mesh: its
    neighbours and messages belong to the old one and must go, and anything
    still queued was encrypted for frames nobody will read. Turning the
    power down is the same mesh and must keep everything."""
    mesh.hear(alex().nodeinfo())
    mesh.hear(alex().text("before"))
    mesh.drain()
    mesh.command(tx_power_dbm=10)
    mesh.dev_wait(lambda: mesh.state().tx_power_dbm == 10, "power applied")
    assert len(mesh.nodes()) == 1 and len(mesh.history()) == 1
    assert mesh.dev.lora()["power_dbm"] == 10
    mesh.command(primary=pb.MeshPrimary(name="Velo Club", key_index=1))
    # The answer comes before the mesh task applies it; the device then says
    # "changed" and the app asks again.
    mesh.dev_wait(lambda: mesh.state().channel_name == "Velo Club" and not mesh.nodes() and not mesh.history(),
                  "the old channel's nodes and messages to go")
    stock = M.tune_for("LongFast", channel="Velo Club")
    assert mesh.dev.lora()["freq_mhz"] == pytest.approx(stock["freq_mhz"], abs=1e-3)
    mesh.hear(alex(channel=M.Channel("Velo Club")).text("on the club channel"), **stock)
    mesh.drain()
    assert [x.text for x in mesh.history()] == ["on the club channel"]


def test_mesh_traffic_during_a_ride_leaves_the_recording_alone(mesh):
    """The radio shares the card's SPI bus. A ride recorded while messages,
    node announcements and the rider's own messages fly must finish as a
    normal ride, and no card access may happen outside the bus lock (on the
    board that wedges the card) nor any lock be taken in both orders."""
    from test_rides import SAVE_BUTTON, check_fit, line_track
    dev = mesh.dev
    dev.gps_track(line_track(45.70, -121.52, 3), speed_kmh=25)
    dev.wait_status(lambda s: s["ride"]["gps_fix"], timeout=15)
    dev.button("boot")
    dev.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    mesh.command(share=pb.MeshShare(index=0, on=True))
    mesh.wait_sent(lambda h: is_port(h, M.PORT_POSITION), "the rider's position")
    mesh.dev_sleep(1500)
    for i in range(12):
        mesh.hear(alex().text("ride msg %d" % i))
        if i % 3 == 0:
            mesh.hear(bea().position(45.70 + i * 0.001, -121.5))
    mesh.drain(timeout=40)
    for i in range(3):
        assert mesh.send("rider says %d" % i).WhichOneof("body") == "mesh_queued"
    mesh.dev_wait(lambda: sum(1 for x in mesh.history() if x.outgoing and x.delivery == pb.MESH_DELIVERY_SENT) == 3,
                  "the rider's messages on the air", timeout=20)
    dev.wait_status(lambda s: s["ride"]["distance_m"] > 50, timeout=30)
    dev.button("boot")                     # stop: the save/discard summary
    dev.wait_status(lambda s: s["screen"] == "other", timeout=10)
    dev.tap(*SAVE_BUTTON)
    dev.wait_status(lambda s: not s["ride"]["recording"], timeout=20)
    assert len([x for x in mesh.history() if not x.outgoing]) == 12
    assert {n.num for n in mesh.nodes() if n.HasField("position")} == {BEA}
    rides = [f for f in os.listdir(os.path.join(dev.sd, "rides")) if f.endswith(".fit")]
    assert len(rides) == 1, rides
    with open(os.path.join(dev.sd, "rides", rides[0]), "rb") as f:
        check_fit(f.read())
    assert lock_warnings(dev) == []
