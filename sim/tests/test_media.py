"""Music (src/ams_client.cpp, src/media.cpp, the MUSIC page): an iPhone's
Apple Media Service feeding the device directly, the companion app's own
feed when there's no AMS (Android, or pairing declined), album art, the
page's transport and volume buttons, and what happens when the phone goes.

The iPhone side is SimPhone.offer_ams(): AMS on the phone's GATT server,
Entity Update notifications sent as iOS sends them, and the Remote Commands
the device writes recorded (sim/PROTOCOL.md PEER_* frames)."""
import threading
import time

import pytest

from conftest import TIME_SCALE, paired_phone
from otpsim import LinkClient, SimPhone, pb
from sensorkit import eventually, glass, set_dash, settle

MUSIC_DASH = "power3s hero\nhr medium half\ncadence medium half\npage music\n"
MUSIC_PAGE = 1
PLAY, PREV, NEXT = (176 + 94, 780 + 60), (24 + 70, 780 + 60), (376 + 70, 780 + 60)   # kMediaPlay/Prev/Next
VOL_UP, VOL_DOWN = (540 - 78 + 38, 140 + 38), (540 - 78 + 38, 232 + 38)            # kMediaVolUp/Down
ART_BOX = (108, 96, 432, 420)        # the 324 px frame
BAR = (16, 642, 524, 652)            # inside the progress bar (ui::CONTENT_X.., 640..654)

OWNER = "d0:00:00:00:00:01"
NO_HANDLER = [v.number for v in pb.Feature.values
              if v.number not in (0, pb.FEATURE_MEDIA_COMMAND_HANDLER)]


def iphone(device):
    """The owner's iPhone, publishing AMS, paired, with the device subscribed."""
    ph = SimPhone(device.ble_port, identity=OWNER, name="Sim iPhone").offer_ams().connect()
    sec = ph.wait_security(10)
    assert sec["encrypted"] and sec["bonded"], sec
    want = ph.wait_ams_ready(10 * TIME_SCALE)
    return ph, want


def media(device):
    return device.media_state()


def gray(w, h, fn):
    return bytes(fn(x, y) & 0xFF for y in range(h) for x in range(w))


def music_page(device, link):
    set_dash(link, MUSIC_DASH)
    link.tell(show_page=pb.ShowPage(page=MUSIC_PAGE))
    device.wait_status(lambda s: s["page"] == MUSIC_PAGE, timeout=5 * TIME_SCALE)
    time.sleep(1.2)


# ---- AMS -------------------------------------------------------------------------------------


def test_the_device_subscribes_to_the_track_and_player_on_ams(device):
    """Once the link is encrypted, the AMS client subscribes to Entity Update
    and names what it wants: artist, album, title and duration of the track,
    and the player's playback info."""
    ph, want = iphone(device)
    assert want[SimPhone.AMS_TRACK] == [0, 1, 2, 3], want
    assert want[SimPhone.AMS_PLAYER] == [1], want
    assert media(device)["ams"]


def test_an_ams_track_shows_and_is_forwarded_to_the_app_once(device):
    """AMS sends title, artist and album as separate notifications. The page
    shows the track; the app (which looks up the album art) gets it once,
    whole, after the burst — not three half-tracks."""
    ph, _ = iphone(device)
    link = LinkClient(ph).hello()
    link.drain_pushes()
    ph.ams_track(title="Hold On", artist="Alabama Shakes", album="Boys & Girls", duration=226.4)
    ph.ams_playback(1, 1.0, 3.0)
    m = eventually(lambda: media(device), lambda m: m["title"] == "Hold On" and m["playing"],
                   "the AMS track never reached the device", 5)
    assert m["artist"] == "Alabama Shakes" and m["album"] == "Boys & Girls" and m["duration_s"] == 226, m
    t = link.wait_push("media_track", timeout=5 * TIME_SCALE)
    assert (t.title, t.artist, t.album) == ("Hold On", "Alabama Shakes", "Boys & Girls"), t
    time.sleep(1.0)
    extra = [p for p in link.drain_pushes() if p.WhichOneof("body") == "media_track"]
    assert not extra, "the track was forwarded more than once: %s" % extra


def test_elapsed_time_runs_locally_at_the_reported_rate(device):
    """AMS reports position only on a state change or seek. Between reports
    the device advances it itself — at the reported rate (2x fast-forward,
    backwards while rewinding) — freezes it on pause, and never runs past
    the track's end. The app's live MUSIC record carries the same clock."""
    ph, _ = iphone(device)
    link = LinkClient(ph).hello()
    link.expect_result(live_subscribe=pb.LiveSubscribe(topics=[pb.LIVE_TOPIC_RIDE, pb.LIVE_TOPIC_MUSIC]))
    ph.ams_track(title="Long Song", artist="A", album="B", duration=60)
    ph.ams_playback(1, 1.0, 10.0)
    eventually(lambda: media(device), lambda m: m["playing"] and m["title"] == "Long Song", "never playing", 5)
    time.sleep(2)
    p = media(device)["position_ms"]
    assert 11500 < p < 13500 * TIME_SCALE, "position didn't advance at 1x: %d" % p
    link.drain_pushes()                               # frames queued while we slept are old
    f = link.wait_push("live", lambda f: f.HasField("music") and f.music.playing, timeout=5)
    assert f.music.from_ams and f.music.duration_ms == 60000 and f.music.rate_pct == 100, f.music
    assert abs(f.music.elapsed_ms - media(device)["position_ms"]) < 1500
    ph.ams_playback(0, 0.0, 20.0)                     # paused at 0:20
    eventually(lambda: media(device), lambda m: not m["playing"], "pause never arrived", 5)
    time.sleep(1.5)
    assert media(device)["position_ms"] == 20000, "the paused position kept moving"
    ph.ams_playback(3, 2.0, 30.0)                     # fast-forwarding
    time.sleep(1.5)
    p = media(device)["position_ms"]
    assert 32000 < p < 30000 + 2 * 1500 * TIME_SCALE + 1000, "fast-forward didn't run at 2x: %d" % p
    ph.ams_playback(2, 1.0, 40.0)                     # rewinding: runs backwards
    time.sleep(1.5)
    assert media(device)["position_ms"] < 39500, "rewind didn't run the position backwards"
    ph.ams_playback(1, 1.0, 59.0)
    time.sleep(2.5)
    assert media(device)["position_ms"] == 60000, "the position ran past the end of the track"


def test_the_player_going_away_empties_the_page(device):
    """An empty PlaybackInfo is iOS saying there is no player at all."""
    ph, _ = iphone(device)
    ph.ams_track(title="Song", artist="X", album="Y", duration=100)
    ph.ams_playback(1, 1.0, 0.0)
    eventually(lambda: media(device), lambda m: m["present"], "never playing", 5)
    ph.ams_playback(None)
    eventually(lambda: media(device), lambda m: not m["present"] and m["title"] == "",
               "the page kept a track after the player closed", 5)


def test_music_page_buttons_reach_the_phone_as_ams_remote_commands(device, tmp_path):
    """Prev, play/pause, next and the volume stepper go to the iPhone as AMS
    Remote Commands — they reach whatever app is playing, with the companion
    app killed. Play/pause flips on the glass at once, before the phone's
    report comes back."""
    ph, _ = iphone(device)
    link = LinkClient(ph).hello()
    ph.ams_track(title="Song", artist="X", album="Y", duration=200)
    ph.ams_playback(1, 1.0, 5.0)
    eventually(lambda: media(device), lambda m: m["playing"], "never playing", 5)
    music_page(device, link)
    for xy in (PLAY, NEXT, PREV, VOL_UP, VOL_DOWN):
        device.tap(*xy)
        time.sleep(0.6)
    cmds = eventually(ph.ams_commands, lambda c: len(c) >= 5, "not every button reached the phone", 5)
    assert cmds == ["toggle", "next", "previous", "volume_up", "volume_down"], cmds
    assert not media(device)["playing"], "play/pause didn't answer on the glass"
    pushes = [p for p in link.drain_pushes() if p.WhichOneof("body") == "media_command"]
    assert not pushes, "with AMS up the app was asked to act too: %s" % pushes


def test_without_ams_the_buttons_ask_the_app(device):
    """No AMS (an Android phone, or pairing for AMS declined): the buttons go
    to an app that said it handles media commands — and nowhere else."""
    ph = paired_phone(device)                       # no AMS on this phone
    link = LinkClient(ph, platform=pb.PLATFORM_ANDROID).hello()
    device.wait_log(r"ams: phone doesn't expose AMS", since=0, timeout=10 * TIME_SCALE)
    link.request(media_meta=pb.MediaMeta(playing=True, position_s=4, duration_s=180, title="Track", artist="A"))
    music_page(device, link)
    link.drain_pushes()
    device.tap(*NEXT)
    m = link.wait_push("media_command", timeout=5 * TIME_SCALE)
    assert m.cmd == pb.MEDIA_CMD_NEXT, m
    device.tap(*VOL_DOWN)
    m = link.wait_push("media_command", timeout=5 * TIME_SCALE)
    assert m.cmd == pb.MEDIA_CMD_VOLUME_DOWN, m


def test_without_ams_an_app_that_cannot_handle_commands_gets_none(device):
    ph = paired_phone(device)
    link = LinkClient(ph, platform=pb.PLATFORM_ANDROID, features=NO_HANDLER).hello()
    device.wait_log(r"ams: phone doesn't expose AMS", since=0, timeout=10 * TIME_SCALE)
    music_page(device, link)
    link.drain_pushes()
    device.tap(*PLAY)
    time.sleep(1.5)
    assert not [p for p in link.drain_pushes() if p.WhichOneof("body") == "media_command"]


# ---- the app's feed --------------------------------------------------------------------------


def test_the_apps_now_playing_shows_when_there_is_no_ams(device):
    """Android: the app is the only feed. Its MediaMeta is the page; its
    MediaClear empties it."""
    ph = paired_phone(device)
    link = LinkClient(ph, platform=pb.PLATFORM_ANDROID).hello()
    link.request(media_meta=pb.MediaMeta(playing=True, position_s=30, duration_s=240,
                                         title="Midnight City", artist="M83", album="Hurry Up"))
    m = eventually(lambda: media(device), lambda m: m["title"] == "Midnight City",
                   "the app's track never showed", 5)
    assert m["playing"] and m["duration_s"] == 240 and not m["ams"], m
    assert 30000 <= m["position_ms"] < 33000 * TIME_SCALE
    link.request(media_clear=pb.MediaClear())
    eventually(lambda: media(device), lambda m: not m["present"], "MediaClear didn't empty the page", 5)


def test_the_apps_track_is_ignored_while_ams_reports(device):
    """AMS sees every player; the app (on iOS) only Apple Music. While AMS
    is reporting, the app's metadata and its clear must not fight it over
    the page."""
    ph, _ = iphone(device)
    link = LinkClient(ph).hello()
    ph.ams_track(title="Spotify Song", artist="S", album="T", duration=180)
    ph.ams_playback(1, 1.0, 0.0)
    eventually(lambda: media(device), lambda m: m["title"] == "Spotify Song", "AMS track never showed", 5)
    link.request(media_meta=pb.MediaMeta(playing=False, title="Apple Music Song", artist="AM"))
    link.request(media_clear=pb.MediaClear())
    time.sleep(1)
    m = media(device)
    assert m["present"] and m["title"] == "Spotify Song", m


def test_a_phone_that_disconnects_takes_its_music_with_it(device):
    """The page shows the phone's player; with the phone gone there is no
    player — title, art and all — rather than a frozen 'now playing'."""
    ph, _ = iphone(device)
    link = LinkClient(ph).hello()
    ph.ams_track(title="Song", artist="X", album="Y", duration=100)
    ph.ams_playback(1, 1.0, 0.0)
    eventually(lambda: media(device), lambda m: m["present"], "never playing", 5)
    assert link.upload(pb.TARGET_ALBUM_ART, "art", gray(64, 64, lambda x, y: x * 4),
                       art_format=pb.ART_GRAY8, art_width=64, art_height=64).code == pb.RESULT_OK
    eventually(lambda: media(device), lambda m: m["art_w"] == 64, "the art never landed", 5)
    ph.disconnect()
    eventually(lambda: media(device), lambda m: not m["present"] and m["art_w"] == 0 and m["art_hash"] == -1,
               "the phone left but its music stayed on the page", 5)
    # Back in range: AMS comes up again and the page follows the phone.
    ph.peer_subscriptions.clear()
    ph._pw_log = []
    ph.connect()                                     # the same, bonded iPhone
    assert ph.wait_security(10)["encrypted"]
    ph.wait_ams_ready(10 * TIME_SCALE)
    ph.ams_track(title="Next Song", artist="X", album="Y", duration=100)
    eventually(lambda: media(device), lambda m: m["title"] == "Next Song", "AMS never resumed", 10)


# ---- album art ---------------------------------------------------------------------------------


def test_grey_album_art_is_dithered_to_the_panels_five_tones(device):
    """The app sends 8-bit grey; the device dithers it to the five tones this
    glass renders. A left-to-right ramp has to use all five, black on the
    left and white on the right."""
    ph, _ = iphone(device)
    link = LinkClient(ph).hello()
    ph.ams_track(title="Ramp", artist="X", album="Y", duration=100)
    eventually(lambda: media(device), lambda m: m["title"] == "Ramp", "never playing", 5)
    w = h = 200
    done = link.upload(pb.TARGET_ALBUM_ART, "art", gray(w, h, lambda x, y: x * 255 // (w - 1)),
                       art_format=pb.ART_GRAY8, art_width=w, art_height=h)
    assert done.code == pb.RESULT_OK, done
    m = eventually(lambda: media(device), lambda m: m["art_w"] == w, "the art never landed", 5)
    assert m["art_h"] == h and sum(m["art_tones"]) == w * h, m
    assert all(n > 0 for n in m["art_tones"]), "the dither didn't use every tone: %s" % m["art_tones"]


def test_tone_art_from_the_phone_lands_exactly(device):
    """Art the phone already dithered (base-5 tone indices, three pixels per
    byte) is unpacked, not dithered again: every pixel lands as sent."""
    ph, _ = iphone(device)
    link = LinkClient(ph).hello()
    ph.ams_track(title="Tones", artist="X", album="Y", duration=100)
    eventually(lambda: media(device), lambda m: m["title"] == "Tones", "never playing", 5)
    w, h = 90, 60
    idx = [(x // 18) % 5 for y in range(h) for x in range(w)]   # five vertical bands
    packed = bytes(idx[i] + 5 * (idx[i + 1] if i + 1 < len(idx) else 0) + 25 * (idx[i + 2] if i + 2 < len(idx) else 0)
                   for i in range(0, len(idx), 3))
    assert link.upload(pb.TARGET_ALBUM_ART, "art", packed, art_format=pb.ART_TONE5,
                       art_width=w, art_height=h).code == pb.RESULT_OK
    m = eventually(lambda: media(device), lambda m: m["art_w"] == w, "the art never landed", 5)
    assert m["art_tones"] == [w * h // 5] * 5, m["art_tones"]


def test_album_art_of_a_silly_size_is_refused(device):
    ph, _ = iphone(device)
    link = LinkClient(ph).hello()
    from otpsim import LinkError
    with pytest.raises(LinkError) as e:
        link.upload(pb.TARGET_ALBUM_ART, "art", bytes(400 * 10), art_format=pb.ART_GRAY8, art_width=400, art_height=10)
    assert e.value.code == "RESULT_BAD_REQUEST"
    with pytest.raises(LinkError):
        link.upload(pb.TARGET_ALBUM_ART, "art", bytes(99), art_format=pb.ART_GRAY8, art_width=10, art_height=10)
    assert media(device)["art_w"] == 0


def test_a_new_track_drops_the_old_tracks_art(device, link):
    """Art belongs to a title: when the track changes, the old cover goes at
    once — the next one may never come (a track with no art)."""
    link.request(media_meta=pb.MediaMeta(playing=True, duration_s=100, title="One", artist="X"))
    assert link.upload(pb.TARGET_ALBUM_ART, "art", gray(40, 40, lambda x, y: 0), art_format=pb.ART_GRAY8,
                       art_width=40, art_height=40).code == pb.RESULT_OK
    eventually(lambda: media(device), lambda m: m["art_w"] == 40, "the art never landed", 5)
    link.request(media_meta=pb.MediaMeta(playing=True, duration_s=100, title="One", artist="X", position_s=9))
    assert media(device)["art_w"] == 40, "the same track's update dropped its art"
    link.request(media_meta=pb.MediaMeta(playing=True, duration_s=100, title="Two", artist="X"))
    eventually(lambda: media(device), lambda m: m["art_w"] == 0, "track Two shows track One's cover", 5)


@pytest.mark.xfail(strict=True, reason=(
    "BUG: the link path (link.cpp, TARGET_ALBUM_ART) buffers the whole upload and only calls "
    "media::beginArt at the end, so the track generation the art is checked against at commit is "
    "the one current when it FINISHED, not when it started: art still in flight when the track "
    "changes lands under the new title (media.h promises it is dropped)"))
def test_art_still_in_flight_when_the_track_changes_is_dropped(device):
    """media.h: 'stale art is dropped on any title change — including art
    still in flight when the title changed, which never gets published.' On
    a slow link a 320 px cover takes seconds; the rider skips meanwhile."""
    ph, _ = iphone(device)
    link = LinkClient(ph).hello()
    ph.ams_track(title="First", artist="X", album="Y", duration=100)
    eventually(lambda: media(device), lambda m: m["title"] == "First", "never playing", 5)
    ph.pace_writes(30, 2)                            # a slow connection interval
    art = gray(320, 320, lambda x, y: (x + y) // 3)
    result = {}

    def send():
        result["done"] = link.upload(pb.TARGET_ALBUM_ART, "art", art, art_format=pb.ART_GRAY8,
                                     art_width=320, art_height=320, timeout=60)
    t = threading.Thread(target=send)
    t0 = time.time()
    t.start()
    time.sleep(0.5)
    ph.ams_track(title="Second")                     # skipped while the cover was on its way
    t.join(90)
    assert result.get("done") and result["done"].code == pb.RESULT_OK, result
    assert time.time() - t0 > 1.0, "the upload was too quick to have been in flight"
    m = media(device)
    assert m["title"] == "Second"
    assert m["art_w"] == 0, "the first track's cover landed under the second track's title"


# ---- the page on the glass -------------------------------------------------------------------


def test_the_music_page_draws_the_art_and_the_progress(device, tmp_path):
    """On the glass: the cover where the record placeholder was, and the
    progress bar filled to the track's position (half way at 1:00 of 2:00)."""
    ph, _ = iphone(device)
    link = LinkClient(ph).hello()
    ph.ams_track(title="Half", artist="X", album="Y", duration=120)
    ph.ams_playback(0, 0.0, 60.0)                    # paused at the half
    eventually(lambda: media(device), lambda m: m["title"] == "Half", "never arrived", 5)
    music_page(device, link)
    settle(device)
    g = glass(device, tmp_path, "noart")
    bar = g.rows[(BAR[1] + BAR[3]) // 2][BAR[0]:BAR[2]]
    filled = sum(1 for b in bar if b <= 0x40) / len(bar)
    assert 0.4 < filled < 0.6, "the progress bar is %.0f%% full at 1:00 of 2:00" % (filled * 100)
    before = g.ink(*ART_BOX)
    assert link.upload(pb.TARGET_ALBUM_ART, "art", gray(300, 300, lambda x, y: 0), art_format=pb.ART_GRAY8,
                       art_width=300, art_height=300).code == pb.RESULT_OK
    eventually(lambda: media(device), lambda m: m["art_w"] == 300, "the art never landed", 5)
    time.sleep(1.5)
    settle(device)
    after = glass(device, tmp_path, "art").ink(*ART_BOX)
    assert after > 0.8 and after > before + 0.4, "the black cover isn't on the glass (%.2f → %.2f)" % (before, after)
