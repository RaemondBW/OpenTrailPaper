// The sim's BLE stack: NimBLE-Arduino's API (sim/shim/NimBLEDevice.h) over
// the simble protocol (sim/PROTOCOL.md). The firmware's GATT server, pairing
// policy and link layer run unchanged on top; this models what NimBLE and
// the radio do around them — connections, encryption and bonds, MTU, CCCDs,
// attribute permissions, notify back-pressure — and the central role the
// device uses for sensors and for the phone's AMS.
#include "sim_ble.h"
#include "sim.h"
#include "sim_clock.h"
#include "ble_pair_guard.h"
#include "nimble/nimble/host/services/gatt/include/services/gatt/ble_svc_gatt.h"

#include <arpa/inet.h>
#include <atomic>
#include <future>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

// ---- the host task -----------------------------------------------------------------

std::mutex g_hostM;
std::condition_variable g_hostCv;
std::deque<std::function<void()>> g_hostQ;

void hostLoop() {
    pthread_setname_np("nimble_host");
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lk(g_hostM);
            g_hostCv.wait(lk, [] { return !g_hostQ.empty(); });
            fn = std::move(g_hostQ.front());
            g_hostQ.pop_front();
        }
        fn();
    }
}

// ---- bonds ----------------------------------------------------------------------

std::mutex g_bondM;
std::vector<NimBLEAddress> g_bonds;

std::string bondsPath() { return sim::opts.dir + "/bonds.txt"; }

void loadBonds() {
    std::lock_guard<std::mutex> lk(g_bondM);
    g_bonds.clear();
    if (FILE* f = fopen(bondsPath().c_str(), "r")) {
        char line[64];
        while (fgets(line, sizeof line, f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.size() >= 17) g_bonds.emplace_back(s.substr(0, 17));
        }
        fclose(f);
    }
}

void saveBondsLocked() {
    if (FILE* f = fopen(bondsPath().c_str(), "w")) {
        for (auto& a : g_bonds) fprintf(f, "%s\n", a.toString().c_str());
        fclose(f);
    }
}

bool bonded(const NimBLEAddress& a) {
    std::lock_guard<std::mutex> lk(g_bondM);
    for (auto& b : g_bonds) if (b == a) return true;
    return false;
}

void addBond(const NimBLEAddress& a) {
    std::lock_guard<std::mutex> lk(g_bondM);
    for (auto& b : g_bonds) if (b == a) return;
    g_bonds.push_back(a);
    saveBondsLocked();
}

bool removeBond(const NimBLEAddress& a) {
    std::lock_guard<std::mutex> lk(g_bondM);
    for (size_t i = 0; i < g_bonds.size(); ++i)
        if (g_bonds[i] == a) {
            g_bonds.erase(g_bonds.begin() + i);
            saveBondsLocked();
            return true;
        }
    return false;
}

// ---- server-side objects -----------------------------------------------------------------

NimBLEServer* g_server = nullptr;
NimBLEAdvertising g_adv;
std::atomic<bool> g_advOn{false};
uint16_t g_mtuPref = 23;
ble_pair_guard_refuse_fn* g_refuse = nullptr;

enum : uint8_t {
    P_CONNECT = 0x01, P_DISCOVER = 0x02, P_READ = 0x03, P_WRITE = 0x04, P_SUBSCRIBE = 0x05,
    P_SECURE = 0x06, P_DISCONNECT = 0x07, P_PASSKEY = 0x08, P_ADV = 0x0A, P_PEER_READ_RSP = 0x10,
    P_PEER_NOTIFY = 0x11, P_PEER_SERVICES = 0x12,
    D_CONNECTED = 0x81, D_DISCOVERED = 0x82, D_READ_RSP = 0x83, D_WRITE_RSP = 0x84,
    D_SUBSCRIBE_RSP = 0x85, D_NOTIFY = 0x86, D_DISCONNECTED = 0x87, D_PASSKEY_REQ = 0x88,
    D_SERVICE_CHANGED = 0x89, D_SECURITY = 0x8A, D_ADV = 0x8B, D_PEER_READ = 0x90,
    D_PEER_SUBSCRIBE = 0x91, D_PEER_WRITE = 0x92,
};
enum : uint8_t { F_PHONE_BONDED = 1, F_PAIR = 2, F_AUTO_PASSKEY = 4, F_STALE_KEYS = 8 };
enum : uint8_t { ATT_OK = 0, ATT_NOT_PERMITTED = 0x03, ATT_INSUF_AUTHEN = 0x05,
                 ATT_NOT_FOUND = 0x0A, ATT_INSUF_ENC = 0x0F };

struct Conn {
    uint16_t handle = 0;
    int fd = -1;
    NimBLEConnInfo info;
    uint8_t flags = 0;
    std::string phoneName;
    bool connected = false;      // CONNECT handled
    bool gone = false;           // disconnect handled (callbacks done)
    std::atomic<bool> dropping{false};   // a disconnect was requested: nothing more completes
    bool securityDone = false;
    bool awaitingPasskey = false;
    // The bonded peer's CCCDs (Service Changed among them) are restored only
    // once encryption completes and the host's callback has returned.
    bool cccdsRestored = false;
    uint32_t passkey = 0;
    std::set<const NimBLECharacteristic*> subs;
    // writer
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> out;
    size_t queuedNotifies = 0;
    bool closing = false;
    // the phone's own GATT server
    std::shared_ptr<simble::SimPeripheral> peer = std::make_shared<simble::SimPeripheral>();
    std::unique_ptr<NimBLEClient> peerClient;
    std::mutex readsM;
    std::map<uint32_t, std::promise<std::pair<uint8_t, std::vector<uint8_t>>>> reads;
    uint32_t nextReq = 1;

    void send(uint8_t type, const std::string& body, bool notify = false) {
        std::string f(3, 0);
        uint16_t len = (uint16_t)(body.size() + 1);
        f[0] = (char)(len & 0xFF);
        f[1] = (char)(len >> 8);
        f[2] = (char)type;
        f += body;
        std::lock_guard<std::mutex> lk(m);
        if (closing) return;
        out.push_back(std::move(f));
        if (notify) ++queuedNotifies;
        cv.notify_all();
    }
    bool trySendNotify(const std::string& body) {
        {
            std::lock_guard<std::mutex> lk(m);
            if (closing) return false;
            if ((int)queuedNotifies >= sim::opts.notifyWindow) return false;
        }
        send(D_NOTIFY, body, true);
        return true;
    }
    void closeAfterFlush() {
        std::lock_guard<std::mutex> lk(m);
        closing = true;
        cv.notify_all();
    }
};

std::mutex g_connsM;
std::map<uint16_t, std::shared_ptr<Conn>> g_conns;
std::atomic<uint16_t> g_nextHandle{1};

std::shared_ptr<Conn> conn(uint16_t h) {
    std::lock_guard<std::mutex> lk(g_connsM);
    auto it = g_conns.find(h);
    return it == g_conns.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<Conn>> allConns() {
    std::lock_guard<std::mutex> lk(g_connsM);
    std::vector<std::shared_ptr<Conn>> v;
    for (auto& kv : g_conns) if (kv.second->connected && !kv.second->gone) v.push_back(kv.second);
    return v;
}

std::string u16(uint16_t v) { return std::string{(char)(v & 0xFF), (char)(v >> 8)}; }
std::string u32(uint32_t v) {
    return std::string{(char)(v & 0xFF), (char)((v >> 8) & 0xFF), (char)((v >> 16) & 0xFF), (char)(v >> 24)};
}

std::vector<NimBLECharacteristic*> allChars() {
    std::vector<NimBLECharacteristic*> v;
    if (!g_server) return v;
    for (auto* s : g_server->services_)
        for (auto* c : s->chars_) v.push_back(c);
    return v;
}

NimBLECharacteristic* findChar(const NimBLEUUID& u) {
    for (auto* c : allChars()) if (c->getUUID() == u) return c;
    return nullptr;
}

void writerLoop(std::shared_ptr<Conn> c) {
    pthread_setname_np("simble_tx");
    int sentThisEvent = 0;
    auto eventStart = std::chrono::steady_clock::now();
    for (;;) {
        std::string f;
        bool isNotify;
        {
            std::unique_lock<std::mutex> lk(c->m);
            c->cv.wait(lk, [&] { return !c->out.empty() || c->closing; });
            if (c->out.empty()) break;   // closing, flushed
            isNotify = (uint8_t)c->out.front()[2] == D_NOTIFY;
        }
        // Connection events: at most K notifications per interval. A queued
        // notification holds its stack buffer until it goes out, so notify()
        // stays refused for as long as the link is slow.
        if (isNotify && sim::opts.connIntervalMs > 0 && sim::opts.packetsPerEvent > 0) {
            if (++sentThisEvent > sim::opts.packetsPerEvent) {
                std::this_thread::sleep_until(eventStart + sim_clock::realDuration(sim::opts.connIntervalMs));
                eventStart = std::chrono::steady_clock::now();
                sentThisEvent = 1;
            }
        }
        {
            std::lock_guard<std::mutex> lk(c->m);
            if (c->out.empty()) continue;
            f = std::move(c->out.front());
            c->out.pop_front();
            if ((uint8_t)f[2] == D_NOTIFY && c->queuedNotifies) --c->queuedNotifies;
        }
        const char* p = f.data();
        size_t left = f.size();
        while (left) {
            ssize_t n = ::send(c->fd, p, left, 0);
            if (n <= 0) { left = 0; break; }
            p += n;
            left -= (size_t)n;
        }
    }
    shutdown(c->fd, SHUT_RDWR);
    close(c->fd);
}

// The link ends: once, on the host task.
void endConn(const std::shared_ptr<Conn>& c, uint8_t hciReason, bool local) {
    if (c->gone) return;
    c->gone = true;
    {
        std::lock_guard<std::mutex> lk(g_connsM);
        g_conns.erase(c->handle);
    }
    if (c->peerClient && c->peerClient->peer_) {
        c->peerClient->peer_ = nullptr;
    }
    if (local) c->send(D_DISCONNECTED, u16(hciReason));
    c->closeAfterFlush();
    {
        std::lock_guard<std::mutex> lk(c->readsM);
        for (auto& kv : c->reads) kv.second.set_value({0xFF, {}});
        c->reads.clear();
    }
    if (c->connected && g_server && g_server->callbacks()) {
        NimBLEConnInfo ci = c->info;
        g_server->callbacks()->onDisconnect(g_server, ci, BLE_HS_ERR_HCI_BASE + hciReason);
    }
    // Only a real link ending resumes advertising: a scan probe (ADV on a
    // socket that never sent CONNECT) changes nothing.
    if (c->connected && g_server && g_server->advOnDisconnect_) g_advOn = true;
}

void sendSecurity(const std::shared_ptr<Conn>& c) {
    std::string b;
    b += (char)c->info.encrypted_;
    b += (char)c->info.bonded_;
    b += (char)c->info.authenticated_;
    b += (char)c->info.keySize_;
    c->send(D_SECURITY, b);
}

void finishPairing(const std::shared_ptr<Conn>& c, bool ok) {
    if (c->gone || c->dropping) return;
    c->awaitingPasskey = false;
    if (ok) {
        addBond(c->info.idAddr_);
        c->info.encrypted_ = c->info.bonded_ = c->info.authenticated_ = true;
        c->info.keySize_ = 16;
    } else {
        c->info.encrypted_ = false;
    }
    sendSecurity(c);
    NimBLEConnInfo ci = c->info;
    if (g_server->callbacks()) g_server->callbacks()->onAuthenticationComplete(ci);
    if (ok) c->cccdsRestored = true;
}

// Encryption or pairing, as iOS/Android and NimBLE would run it. Started by
// the firmware's security request (startSecurity) or by the phone (SECURE,
// which is what a phone does on an "insufficient authentication" error).
void runSecurity(const std::shared_ptr<Conn>& c) {
    if (c->gone || c->dropping || c->securityDone || c->awaitingPasskey || !g_server) return;
    const bool phoneBond = c->flags & F_PHONE_BONDED;
    const bool deviceBond = bonded(c->info.idAddr_);
    if (phoneBond) {
        c->securityDone = true;
        if (deviceBond && !(c->flags & F_STALE_KEYS)) {
            c->info.encrypted_ = c->info.bonded_ = c->info.authenticated_ = true;
            c->info.keySize_ = 16;
        } else {
            c->info.encrypted_ = false;   // stale keys: the LTK no longer matches
        }
        sendSecurity(c);
        NimBLEConnInfo ci = c->info;
        if (g_server->callbacks()) g_server->callbacks()->onAuthenticationComplete(ci);
        if (c->info.encrypted_) c->cccdsRestored = true;
        return;
    }
    if (!(c->flags & F_PAIR)) return;   // the phone won't pair: stays unencrypted
    c->securityDone = true;
    if (deviceBond) {
        // A pairing request from a peer we hold a bond for. The firmware's
        // guard (ble_pair_guard) may refuse it; otherwise NimBLE-Arduino's own
        // repeat-pairing handler deletes the old bond and lets pairing run.
        if (g_refuse && g_refuse(c->handle)) return;   // the guard dropped it
        removeBond(c->info.idAddr_);
    }
    // The phone puts its passkey dialog up as soon as pairing starts — before
    // the device's passkey callback has run, so a refusal there comes too late
    // to spare the rider the dialog.
    const bool askRider = !(c->flags & F_AUTO_PASSKEY);
    if (askRider) {
        c->awaitingPasskey = true;
        c->send(D_PASSKEY_REQ, "");
    }
    uint32_t code = g_server->callbacks() ? g_server->callbacks()->onPassKeyDisplay() : 123456;
    if (c->gone || c->dropping) return;
    if (!askRider) { finishPairing(c, true); return; }
    c->passkey = code;
    std::weak_ptr<Conn> w = c;
    simble::postAfter(30000, [w] {
        if (auto c2 = w.lock()) if (c2->awaitingPasskey) finishPairing(c2, false);
    });
}

bool permitted(const std::shared_ptr<Conn>& c, uint32_t props, bool write, uint8_t& status) {
    const uint32_t enc = write ? NIMBLE_PROPERTY::WRITE_ENC : NIMBLE_PROPERTY::READ_ENC;
    const uint32_t auth = write ? NIMBLE_PROPERTY::WRITE_AUTHEN : NIMBLE_PROPERTY::READ_AUTHEN;
    const uint32_t base = write ? (NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR) : NIMBLE_PROPERTY::READ;
    if (!(props & base)) { status = ATT_NOT_PERMITTED; return false; }
    if ((props & auth) && !c->info.authenticated_) { status = ATT_INSUF_AUTHEN; return false; }
    if ((props & enc) && !c->info.encrypted_) {
        status = c->info.bonded_ ? ATT_INSUF_ENC : ATT_INSUF_AUTHEN;
        return false;
    }
    status = ATT_OK;
    return true;
}

void handleConnect(const std::shared_ptr<Conn>& c, const std::string& b) {
    if (c->connected || b.size() < 11) return;
    // Not advertising yet (still booting): nothing to connect to.
    if (!g_server || !g_server->callbacks()) { endConn(c, 0x3E, true); return; }
    uint16_t mtu = (uint8_t)b[1] | ((uint8_t)b[2] << 8);
    c->flags = (uint8_t)b[3];
    char addr[18];
    snprintf(addr, sizeof addr, "%02x:%02x:%02x:%02x:%02x:%02x", (uint8_t)b[4], (uint8_t)b[5],
             (uint8_t)b[6], (uint8_t)b[7], (uint8_t)b[8], (uint8_t)b[9]);
    size_t nameLen = (uint8_t)b[10];
    c->phoneName = b.substr(11, nameLen);
    c->info.addr_ = NimBLEAddress(std::string(addr), BLE_ADDR_RANDOM);
    c->info.idAddr_ = NimBLEAddress(std::string(addr), BLE_ADDR_PUBLIC);
    c->info.handle_ = c->handle;
    c->info.mtu_ = std::max<uint16_t>(23, std::min<uint16_t>(mtu, g_mtuPref));
    c->info.interval_ = 24;   // 30 ms, iOS's opening interval
    c->connected = true;
    g_advOn = false;   // a connection stops advertising, as on the controller
    // The phone's GAP name, for the device's AMS read of 0x2A00.
    {
        auto ch = std::make_shared<simble::SimRemoteChar>();
        ch->svc = NimBLEUUID((uint16_t)0x1800);
        ch->uuid = NimBLEUUID((uint16_t)0x2A00);
        ch->props = NIMBLE_PROPERTY::READ;
        ch->value.assign(c->phoneName.begin(), c->phoneName.end());
        c->peer->chars.push_back(ch);
        c->peer->phoneHandle = c->handle;
        c->peer->addr = c->info.idAddr_;
        c->peer->name = c->phoneName;
    }
    c->send(D_CONNECTED, u16(c->handle) + u16(c->info.mtu_));
    NimBLEConnInfo ci = c->info;
    if (g_server && g_server->callbacks()) g_server->callbacks()->onConnect(g_server, ci);
    if (c->gone) return;
    if (c->info.mtu_ > 23 && g_server->callbacks()) {
        NimBLEConnInfo ci2 = c->info;
        g_server->callbacks()->onMTUChange(c->info.mtu_, ci2);
    }
}

void handleFrame(const std::shared_ptr<Conn>& c, uint8_t type, const std::string& b) {
    switch (type) {
        case P_CONNECT: handleConnect(c, b); break;
        case P_DISCOVER: {
            std::string out;
            if (g_server)
                for (auto* s : g_server->services_)
                    for (auto* ch : s->chars_)
                        out += simble::uuidBytes(s->getUUID()) + simble::uuidBytes(ch->getUUID()) +
                               u32(ch->getProperties());
            c->send(D_DISCOVERED, out);
            break;
        }
        case P_READ: {
            if (b.size() < 16) break;
            NimBLEUUID u = simble::uuidFrom((const uint8_t*)b.data());
            NimBLECharacteristic* ch = findChar(u);
            uint8_t st = ATT_NOT_FOUND;
            std::string val;
            if (ch && permitted(c, ch->getProperties(), false, st)) {
                NimBLEConnInfo ci = c->info;
                if (ch->getCallbacks()) ch->getCallbacks()->onRead(ch, ci);
                NimBLEAttValue v = ch->getValue();
                val.assign((const char*)v.data(), v.size());
            }
            c->send(D_READ_RSP, b.substr(0, 16) + (char)st + val);
            break;
        }
        case P_WRITE: {
            if (b.size() < 17) break;
            NimBLEUUID u = simble::uuidFrom((const uint8_t*)b.data());
            bool rsp = b[16] != 0;
            NimBLECharacteristic* ch = findChar(u);
            uint8_t st = ATT_NOT_FOUND;
            if (ch && permitted(c, ch->getProperties(), true, st)) {
                if (b.size() - 17 > ch->maxLen_) st = 0x0D;   // invalid attribute length
                else {
                    ch->setValue((const uint8_t*)b.data() + 17, b.size() - 17);
                    NimBLEConnInfo ci = c->info;
                    if (ch->getCallbacks()) ch->getCallbacks()->onWrite(ch, ci);
                }
            }
            if (rsp) c->send(D_WRITE_RSP, b.substr(0, 16) + (char)st);
            break;
        }
        case P_SUBSCRIBE: {
            if (b.size() < 18) break;
            NimBLEUUID u = simble::uuidFrom((const uint8_t*)b.data());
            uint16_t cccd = (uint8_t)b[16] | ((uint8_t)b[17] << 8);
            NimBLECharacteristic* ch = findChar(u);
            uint8_t st = ATT_NOT_FOUND;
            if (ch) {
                const uint32_t p = ch->getProperties();
                st = ATT_OK;
                if (!(p & (NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::INDICATE))) st = ATT_NOT_PERMITTED;
                else if ((p & (NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::WRITE_AUTHEN)) && !c->info.authenticated_)
                    st = ATT_INSUF_AUTHEN;
                else if ((p & (NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::WRITE_ENC)) && !c->info.encrypted_)
                    st = ATT_INSUF_AUTHEN;
                if (st == ATT_OK) {
                    if (cccd) c->subs.insert(ch); else c->subs.erase(ch);
                    NimBLEConnInfo ci = c->info;
                    if (ch->getCallbacks()) ch->getCallbacks()->onSubscribe(ch, ci, cccd);
                }
            }
            c->send(D_SUBSCRIBE_RSP, b.substr(0, 16) + (char)st);
            break;
        }
        case P_SECURE:
            // On a link already encrypted with a bond, the phone asking again
            // is a key refresh: the host reports another encryption change.
            if (c->securityDone && c->info.encrypted_ && c->info.bonded_ && g_server) {
                sendSecurity(c);
                NimBLEConnInfo ci = c->info;
                if (g_server->callbacks()) g_server->callbacks()->onAuthenticationComplete(ci);
            } else {
                runSecurity(c);
            }
            break;
        case P_DISCONNECT: endConn(c, b.empty() ? 0x13 : (uint8_t)b[0], false); break;
        case P_PASSKEY: {
            if (!c->awaitingPasskey || b.size() < 4) break;
            uint32_t pk = (uint8_t)b[0] | ((uint8_t)b[1] << 8) | ((uint8_t)b[2] << 16) | ((uint32_t)(uint8_t)b[3] << 24);
            finishPairing(c, pk == c->passkey);
            break;
        }
        case P_ADV: {
            const auto& d = g_adv.data_;
            std::string o;
            o += (char)(g_advOn ? 1 : 0);
            o += (char)d.name.size() + d.name;
            o += (char)d.mfg.size() + d.mfg;
            o += (char)d.uuids.size();
            for (auto& u : d.uuids) o += simble::uuidBytes(NimBLEUUID(u));
            c->send(D_ADV, o);
            break;
        }
        case P_PEER_NOTIFY: {
            if (b.size() < 32) break;
            NimBLEUUID s = simble::uuidFrom((const uint8_t*)b.data());
            NimBLEUUID ch = simble::uuidFrom((const uint8_t*)b.data() + 16);
            simble::peripheralNotify(c->peer, s, ch, (const uint8_t*)b.data() + 32, b.size() - 32);
            break;
        }
        default: break;
    }
}

void readerLoop(std::shared_ptr<Conn> c) {
    pthread_setname_np("simble_rx");
    std::string buf;
    char tmp[4096];
    for (;;) {
        ssize_t n = recv(c->fd, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
        while (buf.size() >= 3) {
            size_t len = (uint8_t)buf[0] | ((uint8_t)buf[1] << 8);
            if (len == 0) { buf.erase(0, 2); continue; }
            if (buf.size() < 2 + len) break;
            uint8_t type = (uint8_t)buf[2];
            std::string body = buf.substr(3, len - 1);
            buf.erase(0, 2 + len);
            if (type == P_PEER_READ_RSP && body.size() >= 5) {
                uint32_t id = (uint8_t)body[0] | ((uint8_t)body[1] << 8) | ((uint8_t)body[2] << 16) |
                              ((uint32_t)(uint8_t)body[3] << 24);
                std::lock_guard<std::mutex> lk(c->readsM);
                auto it = c->reads.find(id);
                if (it != c->reads.end()) {
                    it->second.set_value({(uint8_t)body[4], std::vector<uint8_t>(body.begin() + 5, body.end())});
                    c->reads.erase(it);
                }
                continue;
            }
            if (type == P_PEER_SERVICES) {
                for (size_t i = 0; i + 36 <= body.size(); i += 36) {
                    auto ch = std::make_shared<simble::SimRemoteChar>();
                    ch->svc = simble::uuidFrom((const uint8_t*)body.data() + i);
                    ch->uuid = simble::uuidFrom((const uint8_t*)body.data() + i + 16);
                    ch->props = (uint8_t)body[i + 32] | ((uint8_t)body[i + 33] << 8);
                    std::lock_guard<std::mutex> lk(simble::peripheralsMutex());
                    c->peer->chars.push_back(ch);
                }
                continue;
            }
            simble::post([c, type, body] { handleFrame(c, type, body); });
        }
    }
    simble::post([c] { endConn(c, 0x13, false); });
}

void acceptLoop(int lfd) {
    pthread_setname_np("simble_accept");
    for (;;) {
        int fd = accept(lfd, nullptr, nullptr);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        auto c = std::make_shared<Conn>();
        c->fd = fd;
        c->handle = g_nextHandle++;
        {
            std::lock_guard<std::mutex> lk(g_connsM);
            g_conns[c->handle] = c;
        }
        std::thread(writerLoop, c).detach();
        std::thread(readerLoop, c).detach();
    }
}

// ---- scanning (central) ----------------------------------------------------------------

std::mutex g_scanM;
bool g_scanning = false;
uint64_t g_scanEndMs = 0;

void scanLoop() {
    pthread_setname_np("ble_scan");
    for (;;) {
        sim_clock::sleepMs(250);
        NimBLEScanCallbacks* cb;
        {
            std::lock_guard<std::mutex> lk(g_scanM);
            if (!g_scanning) continue;
            if (g_scanEndMs && sim_clock::millis() >= g_scanEndMs) {
                g_scanning = false;
                cb = NimBLEDevice::getScan()->cb_;
                if (cb) simble::post([cb] { cb->onScanEnd(NimBLEScanResults(), 0); });
                continue;
            }
            cb = NimBLEDevice::getScan()->cb_;
        }
        if (!cb) continue;
        std::vector<NimBLEAdvertisedDevice> seen;
        {
            std::lock_guard<std::mutex> lk(simble::peripheralsMutex());
            for (auto& p : simble::peripherals()) {
                if (!p->advertising || p->client || p->phoneHandle != BLE_HS_CONN_HANDLE_NONE) continue;
                NimBLEAdvertisedDevice d;
                d.addr_ = p->addr;
                d.name_ = p->name;
                d.mfg_ = p->mfg;
                d.rssi_ = p->rssi;
                d.uuids_ = p->advUuids;
                seen.push_back(d);
            }
        }
        for (auto& d : seen)
            simble::post([cb, d] { cb->onResult(&d); });
    }
}

std::string hex(const std::string& s) {
    static const char* h = "0123456789abcdef";
    std::string o;
    for (unsigned char c : s) { o += h[c >> 4]; o += h[c & 15]; }
    return o;
}

}  // namespace

// ---- simble internals --------------------------------------------------------------

namespace simble {

void post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_hostM);
    g_hostQ.push_back(std::move(fn));
    g_hostCv.notify_one();
}

void postAfter(uint32_t ms, std::function<void()> fn) {
    std::thread([ms, fn] {
        sim_clock::sleepMs(ms);
        post(fn);
    }).detach();
}

std::string uuidBytes(const NimBLEUUID& u) {
    std::string s = u.toString(), o;
    int hi = -1;
    for (char ch : s) {
        int v = isdigit((unsigned char)ch) ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : -1;
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { o += (char)(hi << 4 | v); hi = -1; }
    }
    o.resize(16, 0);
    return o;
}

NimBLEUUID uuidFrom(const uint8_t* b) {
    char s[37];
    snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return NimBLEUUID(std::string(s));
}

std::shared_ptr<SimRemoteChar> SimPeripheral::find(const NimBLEUUID& svc, const NimBLEUUID& chr) {
    for (auto& c : chars) if (c->svc == svc && c->uuid == chr) return c;
    return nullptr;
}

std::mutex& peripheralsMutex() { static std::mutex m; return m; }
std::vector<std::shared_ptr<SimPeripheral>>& peripherals() {
    static std::vector<std::shared_ptr<SimPeripheral>> v;
    return v;
}
std::shared_ptr<SimPeripheral> findPeripheral(const NimBLEAddress& a) {
    std::lock_guard<std::mutex> lk(peripheralsMutex());
    for (auto& p : peripherals()) if (p->addr == a) return p;
    return nullptr;
}

void peripheralNotify(const std::shared_ptr<SimPeripheral>& p, const NimBLEUUID& svc,
                      const NimBLEUUID& chr, const uint8_t* data, size_t len) {
    NimBLERemoteCharacteristic* sub = nullptr;
    {
        std::lock_guard<std::mutex> lk(peripheralsMutex());
        auto c = p->find(svc, chr);
        if (!c) return;
        c->value.assign(data, data + len);
        sub = c->subscriber;
    }
    if (!sub || !sub->cb_) return;
    auto cb = sub->cb_;
    std::vector<uint8_t> copy(data, data + len);
    post([cb, sub, copy]() mutable { cb(sub, copy.data(), copy.size(), true); });
}

void peripheralGone(const std::shared_ptr<SimPeripheral>& p, int reason) {
    NimBLEClient* cl;
    {
        std::lock_guard<std::mutex> lk(peripheralsMutex());
        cl = p->client;
        p->client = nullptr;
        for (auto& c : p->chars) c->subscriber = nullptr;
    }
    if (!cl) return;
    cl->peer_ = nullptr;
    if (cl->cb_) {
        auto* cb = cl->cb_;
        post([cb, cl, reason] { cb->onDisconnect(cl, reason); });
    }
}

bool phoneRead(uint16_t handle, const NimBLEUUID& svc, const NimBLEUUID& chr, std::vector<uint8_t>& out) {
    auto c = conn(handle);
    if (!c || c->gone) return false;
    // Local values (the GAP name) answer at once.
    {
        std::lock_guard<std::mutex> lk(peripheralsMutex());
        auto ch = c->peer->find(svc, chr);
        if (ch && svc == NimBLEUUID((uint16_t)0x1800)) { out = ch->value; return true; }
    }
    std::future<std::pair<uint8_t, std::vector<uint8_t>>> f;
    uint32_t id;
    {
        std::lock_guard<std::mutex> lk(c->readsM);
        id = c->nextReq++;
        f = c->reads[id].get_future();
    }
    c->send(D_PEER_READ, u32(id) + uuidBytes(svc) + uuidBytes(chr));
    if (f.wait_for(sim_clock::realDuration(2000)) != std::future_status::ready) return false;
    auto r = f.get();
    if (r.first != 0) return false;
    out = r.second;
    return true;
}

void phoneSubscribe(uint16_t handle, const NimBLEUUID& svc, const NimBLEUUID& chr, uint16_t cccd) {
    if (auto c = conn(handle)) c->send(D_PEER_SUBSCRIBE, uuidBytes(svc) + uuidBytes(chr) + u16(cccd));
}

void phoneWrite(uint16_t handle, const NimBLEUUID& svc, const NimBLEUUID& chr, const uint8_t* d, size_t n) {
    if (auto c = conn(handle))
        c->send(D_PEER_WRITE, uuidBytes(svc) + uuidBytes(chr) + std::string((const char*)d, n));
}

void dropPhone(int reason) {
    for (auto& c : allConns()) post([c, reason] { endConn(c, (uint8_t)reason, false); });
}

std::string statusJson() {
    std::ostringstream o;
    bool scanning;
    {
        std::lock_guard<std::mutex> lk(g_scanM);
        scanning = g_scanning;
    }
    o << "{\"advertising\":" << (g_advOn ? "true" : "false") << ",\"scanning\":" << (scanning ? "true" : "false")
      << ",\"adv_name\":\"" << g_adv.data_.name << "\",\"adv_mfg\":\"" << hex(g_adv.data_.mfg) << "\"";
    o << ",\"bonds\":[";
    {
        std::lock_guard<std::mutex> lk(g_bondM);
        for (size_t i = 0; i < g_bonds.size(); ++i) o << (i ? "," : "") << "\"" << g_bonds[i].toString() << "\"";
    }
    o << "],\"connections\":[";
    bool first = true;
    for (auto& c : allConns()) {
        o << (first ? "" : ",") << "{\"handle\":" << c->handle << ",\"identity\":\"" << c->info.idAddr_.toString()
          << "\",\"encrypted\":" << (c->info.encrypted_ ? "true" : "false")
          << ",\"bonded\":" << (c->info.bonded_ ? "true" : "false") << ",\"mtu\":" << c->info.mtu_
          << ",\"interval_ms\":" << c->info.interval_ * 1.25 << ",\"subscriptions\":[";
        bool f2 = true;
        for (auto* ch : c->subs) { o << (f2 ? "" : ",") << "\"" << ch->getUUID().toString() << "\""; f2 = false; }
        o << "]}";
        first = false;
    }
    o << "],\"peripherals\":[";
    {
        std::lock_guard<std::mutex> lk(peripheralsMutex());
        first = true;
        for (auto& p : peripherals()) {
            o << (first ? "" : ",") << "{\"addr\":\"" << p->addr.toString() << "\",\"name\":\"" << p->name
              << "\",\"advertising\":" << (p->advertising ? "true" : "false")
              << ",\"connected\":" << (p->client ? "true" : "false") << "}";
            first = false;
        }
    }
    o << "]}";
    return o.str();
}

}  // namespace simble

void sim::bleBegin() {
    loadBonds();
    std::thread(hostLoop).detach();
    std::thread(scanLoop).detach();
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)sim::opts.blePort);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (sockaddr*)&a, sizeof a) != 0 || listen(lfd, 4) != 0) {
        fprintf(stderr, "[sim] simble: can't listen on %d\n", sim::opts.blePort);
        exit(2);
    }
    std::thread(acceptLoop, lfd).detach();
}

std::string sim::bleStatusJson() { return simble::statusJson(); }

// ---- NimBLE API: addresses, UUIDs, attributes ------------------------------------------

NimBLEAddress::NimBLEAddress(const std::string& s, uint8_t type) {
    a_ = {};
    a_.type = type;
    unsigned v[6] = {};
    if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
        for (int i = 0; i < 6; ++i) a_.val[5 - i] = (uint8_t)v[i];   // NimBLE stores LSB first
}

std::string NimBLEAddress::toString() const {
    char s[18];
    snprintf(s, sizeof s, "%02x:%02x:%02x:%02x:%02x:%02x", a_.val[5], a_.val[4], a_.val[3], a_.val[2],
             a_.val[1], a_.val[0]);
    return s;
}

NimBLEUUID::NimBLEUUID(uint16_t u) {
    char s[40];
    snprintf(s, sizeof s, "%04x", u);
    s_ = s;
    normalize();
}

void NimBLEUUID::normalize() {
    for (auto& ch : s_) ch = (char)tolower((unsigned char)ch);
    if (s_.size() == 4) s_ = "0000" + s_ + "-0000-1000-8000-00805f9b34fb";
    else if (s_.size() == 8) s_ = s_ + "-0000-1000-8000-00805f9b34fb";
}

void NimBLECharacteristic::setValue(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lk(m_);
    value_.assign(data, data + std::min<size_t>(size, maxLen_));
}

NimBLEAttValue NimBLECharacteristic::getValue() const {
    std::lock_guard<std::mutex> lk(m_);
    return NimBLEAttValue(value_.data(), value_.size());
}

bool NimBLECharacteristic::notify(uint16_t connHandle) const {
    std::vector<uint8_t> v;
    {
        std::lock_guard<std::mutex> lk(m_);
        v = value_;
    }
    return notify(v.data(), v.size(), connHandle);
}

// NimBLE: a notification to one connection goes regardless of its CCCD; to
// all, only to subscribers. False when the stack is out of buffers.
bool NimBLECharacteristic::notify(const uint8_t* value, size_t length, uint16_t connHandle) const {
    std::string body = simble::uuidBytes(uuid_) + std::string((const char*)value, length);
    bool ok = true;
    for (auto& c : allConns()) {
        if (connHandle != BLE_HS_CONN_HANDLE_NONE && c->handle != connHandle) continue;
        if (connHandle == BLE_HS_CONN_HANDLE_NONE && !c->subs.count(this)) continue;
        if (length > (size_t)(c->info.mtu_ - 3)) length = c->info.mtu_ - 3, body.resize(16 + length);
        if (!c->trySendNotify(body)) ok = false;
    }
    return ok;
}

size_t NimBLECharacteristic::getSubscribedCount() const {
    size_t n = 0;
    for (auto& c : allConns()) n += c->subs.count(this);
    return n;
}

NimBLECharacteristic* NimBLEService::createCharacteristic(const char* uuid, uint32_t props, uint16_t maxLen) {
    return createCharacteristic(NimBLEUUID(uuid), props, maxLen);
}
NimBLECharacteristic* NimBLEService::createCharacteristic(const NimBLEUUID& uuid, uint32_t props, uint16_t maxLen) {
    auto* c = new NimBLECharacteristic(uuid, props, maxLen, this);
    chars_.push_back(c);
    return c;
}
NimBLECharacteristic* NimBLEService::getCharacteristic(const NimBLEUUID& uuid) {
    for (auto* c : chars_) if (c->getUUID() == uuid) return c;
    return nullptr;
}

NimBLEService* NimBLEServer::createService(const char* uuid) { return createService(NimBLEUUID(uuid)); }
NimBLEService* NimBLEServer::createService(const NimBLEUUID& uuid) {
    auto* s = new NimBLEService(uuid, this);
    services_.push_back(s);
    return s;
}
NimBLEService* NimBLEServer::getServiceByUUID(const NimBLEUUID& uuid) {
    for (auto* s : services_) if (s->getUUID() == uuid) return s;
    return nullptr;
}
size_t NimBLEServer::getConnectedCount() const { return allConns().size(); }
NimBLEConnInfo NimBLEServer::getPeerInfo(uint8_t index) const {
    auto v = allConns();
    return index < v.size() ? v[index]->info : NimBLEConnInfo();
}
NimBLEConnInfo NimBLEServer::getPeerInfoByHandle(uint16_t handle) const {
    auto c = conn(handle);
    return c ? c->info : NimBLEConnInfo();
}
std::vector<uint16_t> NimBLEServer::getPeerDevices() const {
    std::vector<uint16_t> v;
    for (auto& c : allConns()) v.push_back(c->handle);
    return v;
}
bool NimBLEServer::disconnect(uint16_t h, uint8_t reason) const {
    auto c = conn(h);
    if (!c) return false;
    c->dropping = true;
    simble::post([c, reason] { endConn(c, reason, true); });
    return true;
}
NimBLEClient* NimBLEServer::getClient(uint16_t h) {
    auto c = conn(h);
    if (!c) return nullptr;
    // NimBLE's getClient() hands back a client with no discovered attributes.
    c->peerClient.reset(new NimBLEClient());
    NimBLEClient* cl = c->peerClient.get();
    cl->handle_ = h;
    cl->addr_ = c->info.idAddr_;
    cl->peer_ = c->peer.get();
    return cl;
}

bool NimBLEAdvertising::start(uint32_t, const NimBLEAddress*) { g_advOn = true; return true; }
bool NimBLEAdvertising::stop() { g_advOn = false; return true; }
bool NimBLEAdvertising::isAdvertising() const { return g_advOn; }

bool NimBLEDevice::setMTU(uint16_t mtu) { g_mtuPref = mtu; return true; }
uint16_t NimBLEDevice::getMTU() { return g_mtuPref; }
NimBLEServer* NimBLEDevice::createServer() {
    if (!g_server) g_server = new NimBLEServer();
    return g_server;
}
NimBLEServer* NimBLEDevice::getServer() { return g_server; }
NimBLEAdvertising* NimBLEDevice::getAdvertising() { return &g_adv; }
int NimBLEDevice::getNumBonds() {
    std::lock_guard<std::mutex> lk(g_bondM);
    return (int)g_bonds.size();
}
NimBLEAddress NimBLEDevice::getBondedAddress(int i) {
    std::lock_guard<std::mutex> lk(g_bondM);
    return i >= 0 && i < (int)g_bonds.size() ? g_bonds[i] : NimBLEAddress();
}
bool NimBLEDevice::isBonded(const NimBLEAddress& a) { return bonded(a); }
// NimBLE-Arduino's deleteBond is ble_gap_unpair.
bool NimBLEDevice::deleteBond(const NimBLEAddress& a) { return ble_gap_unpair(a.getBase()) == 0; }
bool NimBLEDevice::deleteAllBonds() {
    std::lock_guard<std::mutex> lk(g_bondM);
    g_bonds.clear();
    saveBondsLocked();
    return true;
}
NimBLEAddress NimBLEDevice::getAddress() { return NimBLEAddress(std::string("3c:84:27:51:4d:00")); }
bool NimBLEDevice::startSecurity(uint16_t h, int* rc) {
    auto c = conn(h);
    if (rc) *rc = c ? 0 : BLE_HS_ENOTCONN;
    if (!c) return false;
    simble::post([c] { runSecurity(c); });
    return true;
}

extern "C" int ble_gap_update_params(uint16_t h, const ble_gap_upd_params* p) {
    auto c = conn(h);
    if (!c) return BLE_HS_ENOTCONN;
    uint16_t itvl = p->itvl_max;
    simble::postAfter(100, [c, itvl] {
        if (c->gone) return;
        c->info.interval_ = itvl;
        NimBLEConnInfo ci = c->info;
        if (g_server && g_server->callbacks()) g_server->callbacks()->onConnParamsUpdate(ci);
    });
    return 0;
}

// A phone's bond carries an IRK, and the controller's resolving list can't
// change while advertising or scanning: NimBLE answers BLE_HS_EBUSY and
// deletes nothing.
extern "C" int ble_gap_unpair(const ble_addr_t* addr) {
    NimBLEAddress a(*addr);
    if (!bonded(a)) return 5;   // BLE_HS_ENOENT
    if (g_advOn || NimBLEDevice::getScan()->isScanning()) return BLE_HS_EBUSY;
    removeBond(a);
    for (auto& c : allConns())
        if (c->info.idAddr_ == a) simble::post([c] { endConn(c, 0x16, true); });
    return 0;
}

extern "C" void ble_pair_guard_harden(void) {}
extern "C" void ble_pair_guard_set_refuse(ble_pair_guard_refuse_fn* fn) { g_refuse = fn; }
extern "C" int ble_pair_guard_install(uint16_t) { return 0; }

// An indication: only links whose Service Changed CCCD is in place get it.
extern "C" void ble_svc_gatt_changed(uint16_t, uint16_t) {
    for (auto& c : allConns()) if (c->cccdsRestored) c->send(D_SERVICE_CHANGED, "");
}

// ---- NimBLE API: central --------------------------------------------------------------

static NimBLEScan g_scanObj;
NimBLEScan* NimBLEDevice::getScan() { return &g_scanObj; }
NimBLEClient* NimBLEDevice::createClient() { return new NimBLEClient(); }
NimBLEClient* NimBLEDevice::createClient(const NimBLEAddress& a) {
    auto* c = new NimBLEClient();
    c->addr_ = a;
    return c;
}
bool NimBLEDevice::deleteClient(NimBLEClient* c) { delete c; return true; }

bool NimBLEScan::start(uint32_t duration, bool, bool) {
    std::lock_guard<std::mutex> lk(g_scanM);
    g_scanning = true;
    g_scanEndMs = duration ? sim_clock::millis() + duration : 0;
    return true;
}
bool NimBLEScan::stop() {
    std::lock_guard<std::mutex> lk(g_scanM);
    g_scanning = false;
    return true;
}
bool NimBLEScan::isScanning() {
    std::lock_guard<std::mutex> lk(g_scanM);
    return g_scanning;
}

static std::atomic<uint16_t> g_clientHandles{0x100};

bool NimBLEClient::connect(const NimBLEAddress& address, bool, bool, bool) {
    auto p = simble::findPeripheral(address);
    if (!p) {
        sim_clock::sleepMs(5000);   // the connect timeout runs out
        if (cb_) { auto* cb = cb_; auto* self = this; simble::post([cb, self] { cb->onConnectFail(self, 0x0D); }); }
        return false;
    }
    sim_clock::sleepMs(150);
    {
        std::lock_guard<std::mutex> lk(simble::peripheralsMutex());
        p->client = this;
    }
    addr_ = address;
    peer_ = p.get();
    handle_ = g_clientHandles++;
    for (auto* s : services_) { for (auto* c : s->chars_) delete c; delete s; }
    services_.clear();
    if (cb_) { auto* cb = cb_; auto* self = this; simble::post([cb, self] { cb->onConnect(self); }); }
    return true;
}

bool NimBLEClient::disconnect(uint8_t reason) {
    if (!peer_) return false;
    {
        std::lock_guard<std::mutex> lk(simble::peripheralsMutex());
        peer_->client = nullptr;
        for (auto& c : peer_->chars) if (c->subscriber && c->subscriber->client_ == this) c->subscriber = nullptr;
    }
    peer_ = nullptr;
    if (cb_) {
        auto* cb = cb_;
        auto* self = this;
        int r = BLE_HS_ERR_HCI_BASE + (reason == BLE_ERR_REM_USER_CONN_TERM ? BLE_ERR_CONN_TERM_LOCAL : reason);
        simble::post([cb, self, r] { cb->onDisconnect(self, r); });
    }
    return true;
}

bool NimBLEClient::isConnected() const { return peer_ != nullptr; }

NimBLERemoteService* NimBLEClient::getService(const NimBLEUUID& uuid) {
    if (!peer_) return nullptr;
    for (auto* s : services_) if (s->uuid_ == uuid) return s;
    bool any = false;
    {
        std::lock_guard<std::mutex> lk(simble::peripheralsMutex());
        for (auto& c : peer_->chars) if (c->svc == uuid) any = true;
    }
    if (!any) return nullptr;
    auto* s = new NimBLERemoteService();
    s->client_ = this;
    s->uuid_ = uuid;
    services_.push_back(s);
    return s;
}

NimBLEAttValue NimBLEClient::getValue(const NimBLEUUID& svc, const NimBLEUUID& chr) {
    NimBLERemoteService* s = getService(svc);
    NimBLERemoteCharacteristic* c = s ? s->getCharacteristic(chr) : nullptr;
    return c ? c->readValue() : NimBLEAttValue();
}

NimBLEConnInfo NimBLEClient::getConnInfo() const {
    NimBLEConnInfo i;
    i.handle_ = handle_;
    i.addr_ = i.idAddr_ = addr_;
    i.interval_ = 40;   // 50 ms, a typical sensor link
    i.mtu_ = 23;
    return i;
}

NimBLERemoteCharacteristic* NimBLERemoteService::getCharacteristic(const NimBLEUUID& uuid) const {
    for (auto* c : chars_) if (c->getUUID() == uuid) return c;
    if (!client_ || !client_->peer_) return nullptr;
    std::shared_ptr<simble::SimRemoteChar> found;
    {
        std::lock_guard<std::mutex> lk(simble::peripheralsMutex());
        found = client_->peer_->find(uuid_, uuid);
    }
    if (!found) return nullptr;
    auto* rc = new NimBLERemoteCharacteristic();
    rc->client_ = client_;
    rc->chr_ = found.get();
    chars_.push_back(rc);
    return rc;
}

NimBLEAttValue NimBLERemoteService::getValue(const NimBLEUUID& u) const {
    auto* c = getCharacteristic(u);
    return c ? c->readValue() : NimBLEAttValue();
}

NimBLEUUID NimBLERemoteCharacteristic::getUUID() const { return chr_ ? chr_->uuid : NimBLEUUID(); }
bool NimBLERemoteCharacteristic::canRead() const { return chr_ && (chr_->props & NIMBLE_PROPERTY::READ); }
bool NimBLERemoteCharacteristic::canWrite() const { return chr_ && (chr_->props & NIMBLE_PROPERTY::WRITE); }
bool NimBLERemoteCharacteristic::canWriteNoResponse() const { return chr_ && (chr_->props & NIMBLE_PROPERTY::WRITE_NR); }
bool NimBLERemoteCharacteristic::canNotify() const { return chr_ && (chr_->props & NIMBLE_PROPERTY::NOTIFY); }
bool NimBLERemoteCharacteristic::canIndicate() const { return chr_ && (chr_->props & NIMBLE_PROPERTY::INDICATE); }

NimBLEAttValue NimBLERemoteCharacteristic::readValue(time_t*) {
    if (!chr_ || !client_ || !client_->peer_) return NimBLEAttValue();
    if (client_->peer_->phoneHandle != BLE_HS_CONN_HANDLE_NONE) {
        std::vector<uint8_t> v;
        if (simble::phoneRead(client_->peer_->phoneHandle, chr_->svc, chr_->uuid, v))
            return NimBLEAttValue(v.data(), v.size());
        return NimBLEAttValue();
    }
    std::lock_guard<std::mutex> lk(simble::peripheralsMutex());
    return NimBLEAttValue(chr_->value.data(), chr_->value.size());
}

bool NimBLERemoteCharacteristic::writeValue(const uint8_t* d, size_t n, bool) {
    if (!chr_ || !client_ || !client_->peer_) return false;
    if (client_->peer_->phoneHandle != BLE_HS_CONN_HANDLE_NONE) {
        simble::phoneWrite(client_->peer_->phoneHandle, chr_->svc, chr_->uuid, d, n);
        return true;
    }
    std::lock_guard<std::mutex> lk(simble::peripheralsMutex());
    chr_->value.assign(d, d + n);
    return true;
}

bool NimBLERemoteCharacteristic::subscribe(bool notifications, const notify_callback cb, bool) const {
    if (!chr_ || !client_ || !client_->peer_) return false;
    cb_ = cb;
    {
        std::lock_guard<std::mutex> lk(simble::peripheralsMutex());
        chr_->subscriber = const_cast<NimBLERemoteCharacteristic*>(this);
    }
    if (client_->peer_->phoneHandle != BLE_HS_CONN_HANDLE_NONE)
        simble::phoneSubscribe(client_->peer_->phoneHandle, chr_->svc, chr_->uuid, notifications ? 1 : 2);
    return true;
}

bool NimBLERemoteCharacteristic::unsubscribe(bool) const {
    if (!chr_) return false;
    {
        std::lock_guard<std::mutex> lk(simble::peripheralsMutex());
        if (chr_->subscriber == this) chr_->subscriber = nullptr;
    }
    if (client_ && client_->peer_ && client_->peer_->phoneHandle != BLE_HS_CONN_HANDLE_NONE)
        simble::phoneSubscribe(client_->peer_->phoneHandle, chr_->svc, chr_->uuid, 0);
    return true;
}
