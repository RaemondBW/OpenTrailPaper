"""Two devices on one simulated air (otpsim.mesh.Air relays each one's
transmissions to the other's radio): two riders with OpenTrailPaper and
nothing else find each other, learn each other's names and keys, and
message — the group ride the mesh exists for."""
import pytest

from mesh_kit import Mesh, is_port, keep_artifacts, node_num_for, start_device
from otpsim import pb
from otpsim import mesh as M

MAC_A = "3c:84:a0:00:00:0a"
MAC_B = "3c:84:b0:00:00:0b"


@pytest.fixture
def pair(request):
    devs = [start_device(MAC_A), start_device(MAC_B)]
    meshes = []
    try:
        meshes = [Mesh(d, radio_on=False) for d in devs]
        with M.Air(devs) as air:
            yield meshes[0], meshes[1], air
    finally:
        for m in meshes:
            m.close()
        for d in devs:
            keep_artifacts(request, d)


def names(m):
    return {n.num: n.long_name for n in m.nodes()}


def test_two_riders_find_each_other_and_their_dms_are_end_to_end_encrypted(pair):
    """Both switch the mesh on, a moment apart, and each announces itself
    (NodeInfo, 5 s after switching on) while the other is already listening:
    within seconds both node lists name the other. A DM then goes
    PKI-encrypted (each has the other's key), arrives, and the sender's tick
    turns ACKED.

    Not at the same instant: two announcements on the air together are lost
    to each other (half-duplex), and neither then answers the other's request
    for replies, having just sent its own NodeInfo (the 10 min reply gap,
    NODEINFO_REPLY_GAP_MS, as stock Meshtastic does)."""
    a, b, air = pair
    assert (a.num, b.num) == (node_num_for(MAC_A), node_num_for(MAC_B))
    a.command(rename=pb.MeshRename(long_name="Ana on the Gravel", short_name="ANA"))
    b.command(rename=pb.MeshRename(long_name="Ben", short_name="BEN"))
    a.radio(True, announce=False)
    a.dev_sleep(2500)
    b.radio(True, announce=False)
    a.dev_wait(lambda: names(a).get(b.num) == "Ben" and names(b).get(a.num) == "Ana on the Gravel",
               "both to know each other", timeout=30)
    log_a = "\n".join(a.dev.log())
    assert "learned public key of !%08x" % b.num in log_a

    q = a.send("Wait for me at the bridge", dest=b.num).mesh_queued.packet_id
    a.dev_wait(lambda: any(x.text == "Wait for me at the bridge" for x in b.history()), "B to get the DM",
               timeout=20)
    got = [x for x in b.history() if x.text == "Wait for me at the bridge"][0]
    assert got.__getattribute__("from") == a.num and got.to == b.num
    a.dev_wait(lambda: {x.id: x.delivery for x in a.history()}[q] == pb.MESH_DELIVERY_ACKED,
               "A's message to be acknowledged", timeout=20)
    dm = [f for _, f in air.log if bytes.fromhex(f["hex"])[8:12] == q.to_bytes(4, "little")]
    assert dm and bytes.fromhex(dm[0]["hex"])[13] == 0, "the DM was not PKI (channel byte 0)"
    eve = M.VirtualNode(0xEEEEEEEE, "Eve", "EVE")
    assert eve.decode(bytes.fromhex(dm[0]["hex"]))["data"] is None


def test_riders_on_different_channels_do_not_hear_each_other_until_they_share_one(pair):
    """A private primary channel moves the frequency (and the key): a rider
    on "Hood River MTB" and one on the public default are on different
    meshes. Once B joins the same channel they hear each other — the group
    is defined by the channel, not by being close by."""
    a, b, air = pair
    a.command(primary=pb.MeshPrimary(name="Hood River MTB", key_index=4))
    a.dev_wait(lambda: a.state().channel_name == "Hood River MTB", "A on the club channel")
    a.radio(True)
    b.radio(True)
    b.send("anyone on LongFast?")
    b.dev_sleep(4000)
    assert not any(x.text == "anyone on LongFast?" for x in a.history())
    assert a.dev.lora()["mistuned"] >= 1
    assert names(a) == {} and names(b) == {}

    b.command(primary=pb.MeshPrimary(name="Hood River MTB", key_index=4))
    b.dev_wait(lambda: b.state().channel_name == "Hood River MTB" and b.dev.lora()["freq_mhz"] ==
               a.dev.lora()["freq_mhz"], "B on the club channel")
    b.dev_sleep(6000)           # B's own announcement after the retune
    b.send("found you")
    a.dev_wait(lambda: any(x.text == "found you" for x in a.history()), "A to hear B", timeout=20)
    # Not the instant B's transmission ends: B is deaf until its mesh task
    # re-arms receive (see test_mesh.py's back-to-back tests).
    b.dev_sleep(1000)
    assert a.send("welcome").WhichOneof("body") == "mesh_queued"
    try:
        b.dev_wait(lambda: any(x.text == "welcome" and x.__getattribute__("from") == a.num for x in b.history()),
                   "B to hear A", timeout=20)
    except AssertionError:
        raise AssertionError("B never heard A. air: %s; B radio: %s" % (
            [(i, f["start_ms"], f["end_ms"], f["freq_mhz"]) for i, f in air.log], b.dev.lora()))
    assert names(a).get(b.num), "A should know B's name from B's announcement"
