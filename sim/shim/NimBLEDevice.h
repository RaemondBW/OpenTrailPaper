// The sim's BLE stack: NimBLE-Arduino 2.x's peripheral API, same signatures,
// over host/nimble_sim.cpp. A "central" is a TCP client speaking the simble
// protocol (sim/PROTOCOL.md); connection, pairing, bonding, MTU, CCCDs,
// attribute permissions and notify back-pressure are modelled; the radio is not.
#pragma once
#include <Arduino.h>
#include <string>
#include <vector>
#include <mutex>
#include <functional>

#define CONFIG_BT_ENABLED 1
#define CONFIG_BT_NIMBLE_ROLE_PERIPHERAL 1
#define BLE_HS_CONN_HANDLE_NONE 0xFFFF
#define BLE_HS_EBUSY 15
#define BLE_HS_ENOTCONN 7
#define BLE_HS_ERR_HCI_BASE 0x200
#define BLE_ERR_AUTH_FAIL 0x05
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_ERR_CONN_TERM_LOCAL 0x16
#define BLE_ERR_CONN_SPVN_TMO 0x08
#define BLE_GAP_INITIAL_CONN_MIN_CE_LEN 0
#define BLE_GAP_INITIAL_CONN_MAX_CE_LEN 0
#define BLE_ADDR_PUBLIC 0
#define BLE_ADDR_RANDOM 1
#define BLE_SM_IO_CAP_DISP_ONLY 0
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
#define ESP_PWR_LVL_P9 7
#define ESP_PWR_LVL_P3 5
#define ESP_PWR_LVL_N0 4

typedef struct { uint8_t type; uint8_t val[6]; } ble_addr_t;
typedef struct ble_gap_upd_params {
    uint16_t itvl_min, itvl_max, latency, supervision_timeout, min_ce_len, max_ce_len;
} ble_gap_upd_params;
extern "C" int ble_gap_update_params(uint16_t conn, const ble_gap_upd_params* p);
extern "C" int ble_gap_unpair(const ble_addr_t* addr);

namespace NIMBLE_PROPERTY {
enum : uint32_t {
    READ = 0x0002, READ_ENC = 0x0200, READ_AUTHEN = 0x0400, READ_AUTHOR = 0x0800,
    WRITE = 0x0008, WRITE_NR = 0x0004, WRITE_ENC = 0x1000, WRITE_AUTHEN = 0x2000,
    WRITE_AUTHOR = 0x4000, BROADCAST = 0x0001, NOTIFY = 0x0010, INDICATE = 0x0020,
};
}

class NimBLEAddress {
public:
    NimBLEAddress() { a_ = {}; }
    explicit NimBLEAddress(const ble_addr_t& a) : a_(a) {}
    NimBLEAddress(const std::string& s, uint8_t type = BLE_ADDR_PUBLIC);
    std::string toString() const;
    const ble_addr_t* getBase() const { return &a_; }
    uint8_t getType() const { return a_.type; }
    bool isNull() const { for (auto b : a_.val) if (b) return false; return true; }
    bool operator==(const NimBLEAddress& o) const { return memcmp(a_.val, o.a_.val, 6) == 0; }
    bool operator!=(const NimBLEAddress& o) const { return !(*this == o); }
    operator std::string() const { return toString(); }
private:
    ble_addr_t a_;
};

class NimBLEUUID {
public:
    NimBLEUUID() {}
    NimBLEUUID(const char* s) : s_(s ? s : "") { normalize(); }
    NimBLEUUID(const std::string& s) : s_(s) { normalize(); }
    NimBLEUUID(uint16_t u16);
    std::string toString() const { return s_; }
    bool operator==(const NimBLEUUID& o) const { return s_ == o.s_; }
private:
    void normalize();
    std::string s_;
};

class NimBLEAttValue {
public:
    NimBLEAttValue() {}
    NimBLEAttValue(const uint8_t* d, size_t n) : v_(d, d + n) {}
    const uint8_t* data() const { return v_.data(); }
    size_t size() const { return v_.size(); }
    size_t length() const { return v_.size(); }
    const char* c_str() const { s_.assign(v_.begin(), v_.end()); return s_.c_str(); }
    operator std::string() const { return std::string(v_.begin(), v_.end()); }
    uint8_t operator[](size_t i) const { return v_[i]; }
    const uint8_t* begin() const { return v_.data(); }
    const uint8_t* end() const { return v_.data() + v_.size(); }
private:
    std::vector<uint8_t> v_;
    mutable std::string s_;
};

struct SimConn;   // host/nimble_sim.cpp
class NimBLEClient;
class NimBLEScan;

class NimBLEConnInfo {
public:
    NimBLEConnInfo() {}
    NimBLEAddress getAddress() const { return addr_; }
    NimBLEAddress getIdAddress() const { return idAddr_; }
    uint16_t getConnHandle() const { return handle_; }
    uint16_t getConnInterval() const { return interval_; }
    uint16_t getConnTimeout() const { return 400; }
    uint16_t getConnLatency() const { return 0; }
    uint16_t getMTU() const { return mtu_; }
    bool isMaster() const { return false; }
    bool isSlave() const { return true; }
    bool isBonded() const { return bonded_; }
    bool isEncrypted() const { return encrypted_; }
    bool isAuthenticated() const { return authenticated_; }
    uint8_t getSecKeySize() const { return keySize_; }
    std::string toString() const { return "conn " + std::to_string(handle_); }
    // sim
    NimBLEAddress addr_, idAddr_;
    uint16_t handle_ = BLE_HS_CONN_HANDLE_NONE, interval_ = 24, mtu_ = 23;
    bool bonded_ = false, encrypted_ = false, authenticated_ = false;
    uint8_t keySize_ = 0;
};

class NimBLECharacteristic;
class NimBLEService;
class NimBLEServer;

class NimBLECharacteristicCallbacks {
public:
    virtual ~NimBLECharacteristicCallbacks() {}
    virtual void onRead(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {}
    virtual void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {}
    virtual void onStatus(NimBLECharacteristic* pCharacteristic, int code) {}
    virtual void onStatus(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo, int code) {}
    virtual void onSubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo, uint16_t subValue) {}
};

class NimBLECharacteristic {
public:
    NimBLECharacteristic(const NimBLEUUID& uuid, uint32_t props, uint16_t maxLen, NimBLEService* svc)
        : uuid_(uuid), props_(props), maxLen_(maxLen), svc_(svc) {}
    void setCallbacks(NimBLECharacteristicCallbacks* cb) { cb_ = cb; }
    NimBLECharacteristicCallbacks* getCallbacks() const { return cb_; }
    void setValue(const uint8_t* data, size_t size);
    void setValue(const char* s) { setValue((const uint8_t*)s, strlen(s)); }
    void setValue(const std::string& s) { setValue((const uint8_t*)s.data(), s.size()); }
    void setValue(const std::vector<uint8_t>& v) { setValue(v.data(), v.size()); }
    template <typename T> void setValue(const T& v) { setValue((const uint8_t*)&v, sizeof(T)); }
    NimBLEAttValue getValue() const;
    bool notify(uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE) const;
    bool notify(const uint8_t* value, size_t length, uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE) const;
    bool notify(const std::string& v, uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE) const {
        return notify((const uint8_t*)v.data(), v.size(), connHandle); }
    bool indicate(uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE) const { return notify(connHandle); }
    size_t getSubscribedCount() const;
    NimBLEUUID getUUID() const { return uuid_; }
    uint32_t getProperties() const { return props_; }
    NimBLEService* getService() const { return svc_; }
    uint16_t getHandle() const { return handle_; }
    // sim
    uint16_t handle_ = 0;
    uint16_t maxLen_;
private:
    NimBLEUUID uuid_;
    uint32_t props_;
    NimBLEService* svc_;
    NimBLECharacteristicCallbacks* cb_ = nullptr;
    mutable std::mutex m_;
    std::vector<uint8_t> value_;
};

class NimBLEService {
public:
    NimBLEService(const NimBLEUUID& uuid, NimBLEServer* srv) : uuid_(uuid), srv_(srv) {}
    NimBLECharacteristic* createCharacteristic(const char* uuid,
                                               uint32_t properties = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE,
                                               uint16_t max_len = 512);
    NimBLECharacteristic* createCharacteristic(const NimBLEUUID& uuid,
                                               uint32_t properties = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE,
                                               uint16_t max_len = 512);
    NimBLECharacteristic* getCharacteristic(const NimBLEUUID& uuid);
    bool start() { started_ = true; return true; }
    NimBLEUUID getUUID() const { return uuid_; }
    std::vector<NimBLECharacteristic*> chars_;
    bool started_ = false;
private:
    NimBLEUUID uuid_;
    NimBLEServer* srv_;
};

class NimBLEServerCallbacks {
public:
    virtual ~NimBLEServerCallbacks() {}
    virtual void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {}
    virtual void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {}
    virtual void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) {}
    virtual uint32_t onPassKeyDisplay() { return 123456; }
    virtual void onPassKeyEntry(NimBLEConnInfo& connInfo) {}
    virtual void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pin) {}
    virtual void onAuthenticationComplete(NimBLEConnInfo& connInfo) {}
    virtual void onIdentity(NimBLEConnInfo& connInfo) {}
    virtual void onConnParamsUpdate(NimBLEConnInfo& connInfo) {}
    virtual void onPhyUpdate(NimBLEConnInfo& connInfo, uint8_t txPhy, uint8_t rxPhy) {}
};

class NimBLEServer {
public:
    void setCallbacks(NimBLEServerCallbacks* cb, bool deleteCallbacks = true) { cb_ = cb; (void)deleteCallbacks; }
    NimBLEServerCallbacks* callbacks() const { return cb_; }
    NimBLEService* createService(const char* uuid);
    NimBLEService* createService(const NimBLEUUID& uuid);
    NimBLEService* getServiceByUUID(const NimBLEUUID& uuid);
    bool start() { return true; }
    void advertiseOnDisconnect(bool on) { advOnDisconnect_ = on; }
    size_t getConnectedCount() const;
    NimBLEConnInfo getPeerInfo(uint8_t index) const;
    NimBLEConnInfo getPeerInfoByHandle(uint16_t handle) const;
    std::vector<uint16_t> getPeerDevices() const;
    bool disconnect(uint16_t connHandle, uint8_t reason = BLE_ERR_REM_USER_CONN_TERM) const;
    bool disconnect(const NimBLEConnInfo& info, uint8_t reason = BLE_ERR_REM_USER_CONN_TERM) const {
        return disconnect(info.getConnHandle(), reason); }
    bool updateConnParams(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) const { return true; }
    uint16_t getPeerMTU(uint16_t handle) const { return getPeerInfoByHandle(handle).getMTU(); }
    NimBLEClient* getClient(uint16_t connHandle);
    NimBLEClient* getClient(const NimBLEConnInfo& info) { return getClient(info.getConnHandle()); }
    std::vector<NimBLEService*> services_;
    bool advOnDisconnect_ = true;
private:
    NimBLEServerCallbacks* cb_ = nullptr;
};

class NimBLEAdvertisementData {
public:
    void setName(const std::string& n) { name = n; }
    void setManufacturerData(const std::string& d) { mfg = d; }
    void setFlags(uint8_t) {}
    void addServiceUUID(const NimBLEUUID& u) { uuids.push_back(u.toString()); }
    void setCompleteServices(const NimBLEUUID& u) { addServiceUUID(u); }
    std::string name, mfg;
    std::vector<std::string> uuids;
};

class NimBLEAdvertising {
public:
    bool start(uint32_t duration = 0, const NimBLEAddress* dir = nullptr);
    bool stop();
    bool isAdvertising() const;
    bool reset() { data_ = NimBLEAdvertisementData(); return true; }
    bool addServiceUUID(const NimBLEUUID& u) { data_.addServiceUUID(u); return true; }
    bool addServiceUUID(const char* u) { return addServiceUUID(NimBLEUUID(u)); }
    bool removeServices() { data_.uuids.clear(); return true; }
    bool setName(const std::string& n) { data_.name = n; return true; }
    bool setManufacturerData(const uint8_t* d, size_t n) { data_.mfg.assign((const char*)d, n); return true; }
    bool setManufacturerData(const std::string& d) { data_.mfg = d; return true; }
    bool setAdvertisementData(const NimBLEAdvertisementData& d) { data_ = d; return true; }
    bool setScanResponseData(const NimBLEAdvertisementData& d) { scan_ = d; return true; }
    bool enableScanResponse(bool) { return true; }
    void setMinInterval(uint16_t) {}
    void setMaxInterval(uint16_t) {}
    bool setPreferredParams(uint16_t, uint16_t) { return true; }
    bool setAppearance(uint16_t) { return true; }
    bool addTxPower() { return true; }
    void setAdvertisementType(uint8_t) {}
    NimBLEAdvertisementData data_, scan_;
};


// ---------------------------------------------------------------------------
// Central role: the device scanning for and connecting to sensors, and (AMS)
// reading the phone's own GATT server. Remote peripherals are sim objects
// (host/nimble_sim.cpp: SimPeripheral) — virtual sensors driven by the
// control channel, or the connected phone over simble.
#define BLE_HS_IO_DISPLAY_ONLY 0
#define BLE_HS_IO_DISPLAY_YESNO 1
#define BLE_HS_IO_KEYBOARD_ONLY 2
#define BLE_HS_IO_NO_INPUT_OUTPUT 3

namespace simble { struct SimPeripheral; struct SimRemoteChar; }
class NimBLEClient;
class NimBLERemoteService;

class NimBLEAdvertisedDevice {
public:
    const NimBLEAddress& getAddress() const { return addr_; }
    std::string getName() const { return name_; }
    int getRSSI() const { return rssi_; }
    bool isAdvertisingService(const NimBLEUUID& uuid) const {
        for (auto& u : uuids_) if (u == uuid) return true; return false; }
    bool haveName() const { return !name_.empty(); }
    std::string getManufacturerData(uint8_t index = 0) const { (void)index; return mfg_; }
    bool haveServiceUUID() const { return !uuids_.empty(); }
    NimBLEAddress addr_;
    std::string name_, mfg_;
    int rssi_ = -60;
    std::vector<NimBLEUUID> uuids_;
};

class NimBLEScanResults {};
class NimBLEScanCallbacks {
public:
    virtual ~NimBLEScanCallbacks() {}
    virtual void onDiscovered(const NimBLEAdvertisedDevice* advertisedDevice) {}
    virtual void onResult(const NimBLEAdvertisedDevice* advertisedDevice) {}
    virtual void onScanEnd(const NimBLEScanResults& scanResults, int reason) {}
};

class NimBLEScan {
public:
    bool start(uint32_t duration, bool isContinue = false, bool restart = true);
    bool stop();
    bool isScanning();
    void setScanCallbacks(NimBLEScanCallbacks* cb, bool wantDuplicates = false) { cb_ = cb; dup_ = wantDuplicates; }
    void setActiveScan(bool) {}
    void setInterval(uint16_t) {}
    void setWindow(uint16_t) {}
    void setDuplicateFilter(uint8_t) {}
    void setMaxResults(uint8_t) {}
    void clearResults() {}
    NimBLEScanCallbacks* cb_ = nullptr;
    bool dup_ = false;
};

class NimBLERemoteCharacteristic {
public:
    typedef std::function<void(NimBLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData,
                               size_t length, bool isNotify)> notify_callback;
    bool subscribe(bool notifications = true, const notify_callback notifyCallback = nullptr,
                   bool response = true) const;
    bool unsubscribe(bool response = true) const;
    NimBLEAttValue readValue(time_t* timestamp = nullptr);
    bool writeValue(const uint8_t* data, size_t length, bool response = false);
    bool writeValue(const std::string& v, bool response = false) {
        return writeValue((const uint8_t*)v.data(), v.size(), response); }
    bool canRead() const;
    bool canWrite() const;
    bool canWriteNoResponse() const;
    bool canNotify() const;
    bool canIndicate() const;
    NimBLEUUID getUUID() const;
    NimBLEClient* getClient() const { return client_; }
    // sim
    NimBLEClient* client_ = nullptr;
    simble::SimRemoteChar* chr_ = nullptr;
    mutable notify_callback cb_;
};

class NimBLERemoteService {
public:
    NimBLERemoteCharacteristic* getCharacteristic(const char* uuid) const { return getCharacteristic(NimBLEUUID(uuid)); }
    NimBLERemoteCharacteristic* getCharacteristic(const NimBLEUUID& uuid) const;
    NimBLEAttValue getValue(const NimBLEUUID& characteristicUuid) const;
    NimBLEClient* getClient(void) const { return client_; }
    // sim
    NimBLEClient* client_ = nullptr;
    NimBLEUUID uuid_;
    mutable std::vector<NimBLERemoteCharacteristic*> chars_;
};

class NimBLEClientCallbacks {
public:
    virtual ~NimBLEClientCallbacks() {}
    virtual void onConnect(NimBLEClient* pClient) {}
    virtual void onConnectFail(NimBLEClient* pClient, int reason) {}
    virtual void onDisconnect(NimBLEClient* pClient, int reason) {}
    virtual void onPassKeyEntry(NimBLEConnInfo& connInfo) {}
    virtual void onAuthenticationComplete(NimBLEConnInfo& connInfo) {}
    virtual void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) {}
    virtual void onIdentity(NimBLEConnInfo& connInfo) {}
    virtual void onMTUChange(NimBLEClient* pClient, uint16_t MTU) {}
    virtual void onPhyUpdate(NimBLEClient* pClient, uint8_t txPhy, uint8_t rxPhy) {}
};

class NimBLEClient {
public:
    bool connect(const NimBLEAddress& address, bool deleteAttributes = true, bool asyncConnect = false,
                 bool exchangeMTU = true);
    bool connect(const NimBLEAdvertisedDevice* device, bool deleteAttributes = true,
                 bool asyncConnect = false, bool exchangeMTU = true) {
        return connect(device->getAddress(), deleteAttributes, asyncConnect, exchangeMTU); }
    bool disconnect(uint8_t reason = BLE_ERR_REM_USER_CONN_TERM);
    bool isConnected() const;
    void setClientCallbacks(NimBLEClientCallbacks* cb, bool deleteCallbacks = true) { cb_ = cb; (void)deleteCallbacks; }
    void setConnectTimeout(uint32_t ms) { (void)ms; }
    void setConnectionParams(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t = 0, uint16_t = 0) {}
    bool updateConnParams(uint16_t, uint16_t, uint16_t, uint16_t) { return true; }
    NimBLERemoteService* getService(const char* uuid) { return getService(NimBLEUUID(uuid)); }
    NimBLERemoteService* getService(const NimBLEUUID& uuid);
    NimBLEAttValue getValue(const NimBLEUUID& serviceUUID, const NimBLEUUID& characteristicUUID);
    NimBLEAddress getPeerAddress() const { return addr_; }
    NimBLEConnInfo getConnInfo() const;
    uint16_t getConnHandle() const { return handle_; }
    int getRssi() const { return -60; }
    bool secureConnection(bool async = false) const { (void)async; return true; }
    // sim
    NimBLEClientCallbacks* cb_ = nullptr;
    NimBLEAddress addr_;
    simble::SimPeripheral* peer_ = nullptr;
    uint16_t handle_ = BLE_HS_CONN_HANDLE_NONE;
    std::vector<NimBLERemoteService*> services_;
};

class NimBLEUtils {
public:
    static const char* returnCodeToString(int rc) { (void)rc; return rc == 0 ? "OK" : "error"; }
};

class NimBLEDevice {
public:
    static bool init(const std::string& name) { (void)name; return true; }
    static bool deinit(bool clearAll = false) { (void)clearAll; return true; }
    static bool isInitialized() { return true; }
    static bool setMTU(uint16_t mtu);
    static uint16_t getMTU();
    static NimBLEServer* createServer();
    static NimBLEServer* getServer();
    static NimBLEAdvertising* getAdvertising();
    static bool startAdvertising(uint32_t duration = 0) { return getAdvertising()->start(duration); }
    static bool stopAdvertising() { return getAdvertising()->stop(); }
    static int getNumBonds();
    static NimBLEAddress getBondedAddress(int index);
    static bool isBonded(const NimBLEAddress& address);
    static bool deleteBond(const NimBLEAddress& address);
    static bool deleteAllBonds();
    static bool setPower(int8_t dbm) { (void)dbm; return true; }
    static void setSecurityAuth(bool bonding, bool mitm, bool sc) { (void)bonding; (void)mitm; (void)sc; }
    static void setSecurityAuth(uint8_t auth) { (void)auth; }
    static void setSecurityIOCap(uint8_t cap) { (void)cap; }
    static void setSecurityInitKey(uint8_t k) { (void)k; }
    static void setSecurityRespKey(uint8_t k) { (void)k; }
    static void setSecurityPasskey(uint32_t pk) { (void)pk; }
    static bool setDeviceName(const std::string& n) { (void)n; return true; }
    static NimBLEAddress getAddress();
    static NimBLEScan* getScan();
    static NimBLEClient* createClient();
    static NimBLEClient* createClient(const NimBLEAddress& peer);
    static bool deleteClient(NimBLEClient* c);
    static bool startSecurity(uint16_t connHandle, int* rcPtr = nullptr);
};
