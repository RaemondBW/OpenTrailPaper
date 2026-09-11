#!/usr/bin/env python3
"""Host fault injection for production PM/logger sources (only includes replaced).

No hardware claims: this checks failure policy, lock order, and retained bytes.
The firmware build separately verifies the real ESP/Arduino API and linker hooks.
"""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / '.pio' / 'power-tests'
OUT.mkdir(parents=True, exist_ok=True)

def production(name):
    return re.sub(r'^#include[^\n]*\n', '', (ROOT / 'src' / name).read_text(), flags=re.M)

def build(name, source):
    path = OUT / (name + '.cpp')
    path.write_text(source)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unused-const-variable',
                    '-fsanitize=address,undefined', '-g', '-I', str(ROOT / 'src'),
                    str(path), '-o', str(OUT / name)], check=True)
    return OUT / name

common = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <atomic>
#include <string>
#include <vector>
#include <algorithm>
#define RTC_NOINIT_ATTR
constexpr int ESP_RST_POWERON=1;
int esp_reset_reason(){return ESP_RST_POWERON;}
uint32_t nowMs = 40000;
uint32_t millis() { return nowMs; }
struct Console {
 bool connected = false;
 operator bool() const { return connected; }
 void print(const char*) {}
 void println(const char*) {}
 size_t write(const uint8_t*, size_t n) { return n; }
} Serial;
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_NOT_SUPPORTED=1, ESP_ERR_INVALID_STATE=2, ESP_ERR_NO_MEM=3;
const char* esp_err_to_name(int err) { return err ? "ERROR" : "ESP_OK"; }
const char* esp_get_idf_version() { return "test"; }
#define ESP_ERROR_CHECK(x) assert((x)==ESP_OK)
struct Critical { bool held = false; };
using portMUX_TYPE = Critical;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) do { assert(!(m)->held); (m)->held=true; } while(0)
#define portEXIT_CRITICAL(m) do { assert((m)->held); (m)->held=false; } while(0)
'''

pm = common + r'''
#include "power_mgmt.h"
struct Lock { int count=0; };
using esp_pm_lock_handle_t = Lock*;
constexpr int ESP_PM_NO_LIGHT_SLEEP=0;
struct esp_pm_config_esp32s3_t { int max_freq_mhz=0, min_freq_mhz=0; bool light_sleep_enable=false; };
std::vector<Lock*> locks;
int failCreate=0, creates=0;
bool unsupported=false, configured=false, vbus=false, vbusKnown=true;
#ifndef PM_MIN_CPU_MHZ
#define PM_MIN_CPU_MHZ 240
#endif
#ifndef PM_LIGHT_SLEEP
#define PM_LIGHT_SLEEP 1
#endif
int configureCalls=0;
int esp_pm_configure(const esp_pm_config_esp32s3_t* cfg) {
 if (unsupported) return ESP_ERR_NOT_SUPPORTED;
 // Initialization must stay at 240 MHz; only begin/runtime requests may
 // enable DFS. Every runtime toggle must preserve the selected minimum.
 assert(cfg->max_freq_mhz==240);
 assert(cfg->min_freq_mhz==(configureCalls++ == 0 ? 240 : PM_MIN_CPU_MHZ));
 if (cfg->light_sleep_enable) { assert(locks.size()==2); assert(locks[0]->count>0); }
 configured=cfg->light_sleep_enable; return ESP_OK;
}
int esp_pm_lock_create(int,int,const char*,Lock** out) {
 if (++creates==failCreate) return ESP_ERR_NO_MEM;
 *out=new Lock; locks.push_back(*out); return ESP_OK;
}
int esp_pm_lock_acquire(Lock* p) { ++p->count; return ESP_OK; }
int esp_pm_lock_release(Lock* p) { assert(p->count>0); --p->count; return ESP_OK; }
int esp_pm_dump_locks(FILE*) { return ESP_OK; }
int esp_bt_sleep_enable(){return ESP_OK;}
int esp_bt_sleep_disable(){return ESP_OK;}
namespace diag { void log(const char*, ...) {} }
bool phoneUp=false, phoneLong=false, sleepAllowed=false, xtalReady=false, hunt=false;
extern "C" bool board_ble_xtal_clock_ready(){return xtalReady;}
namespace ble_sensors { bool anyConnected(){return false;} bool radioBusy(){return hunt;} }
namespace ble_server { bool isPhoneConnected(){return phoneUp;} bool linkRelaxed(){return phoneLong;}
 bool relaxedSleepAllowed(){return sleepAllowed;} }
namespace ride_recorder { bool longAutoPaused(){return false;} bool sdMounted(){return true;} }
namespace usb_storage { bool hostActive(){return false;} }
bool board_usb_power_present(bool& value) { if(vbusKnown) value=vbus; return vbusKnown; }
void power_mgmt::sleepStats(uint32_t& ok,uint32_t& rejected,uint64_t& us){ok=rejected=0;us=0;}
''' + production('power_mgmt.cpp') + r'''
int main(int argc,char** argv) {
 std::string scenario=argc>1?argv[1]:"normal";
 if(scenario=="first-lock-fail") failCreate=1;
 if(scenario=="second-lock-fail") failCreate=2;
 if(scenario=="stock") unsupported=true;
 bool ready=power_mgmt::prepare();
 assert(!configured);
 if(failCreate || unsupported) {
  assert(!ready); assert(!power_mgmt::begin()); assert(!configured);
  power_mgmt::tick(); assert(!configured);
 } else {
  assert(ready); assert(locks[0]->count==1);
  // A peripheral already busy when PM is enabled must retain its protection.
  power_mgmt::busyAcquire(); power_mgmt::busyAcquire();
  assert(locks[1]->count==2);
  assert(power_mgmt::begin()); assert(configured == (PM_LIGHT_SLEEP != 0));
  power_mgmt::busyRelease(); assert(locks[1]->count==1);
  power_mgmt::busyRelease(); assert(locks[1]->count==0);
  power_mgmt::busyRelease(); assert(locks[1]->count==0); // no negative counter
  vbus=true; power_mgmt::tick(); assert(locks[0]->count==1);
  vbus=false; vbusKnown=false; power_mgmt::tick(); assert(locks[0]->count==1);
  vbusKnown=true; power_mgmt::tick(); assert(locks[0]->count==0);
  Serial.connected=true; // stale DTR after a physical unplug must not hold PM
  power_mgmt::tick(); assert(locks[0]->count==0);
  vbusKnown=false; power_mgmt::tick(); assert(locks[0]->count==1);
  vbusKnown=true; Serial.connected=false;
  power_mgmt::requestSleep(false); power_mgmt::tick(); assert(!configured);
  char state[64]; power_mgmt::stateStr(state,sizeof(state));
  assert(strstr(state,"disabled")); power_mgmt::stateStr(nullptr,0);
  // Re-arm while USB guard is held, as happens through the console.
  vbus=true; power_mgmt::tick(); power_mgmt::requestSleep(true);
  power_mgmt::tick(); assert(configured);
#ifdef PM_BLE_XTAL
  vbus=false; phoneUp=true; sleepAllowed=true; phoneLong=false; xtalReady=true;
  power_mgmt::tick(); assert(locks[0]->count==0); // fast phone may sleep
  sleepAllowed=false; power_mgmt::tick(); assert(locks[0]->count==1); // fallback
  sleepAllowed=true; xtalReady=false; phoneLong=true;
  power_mgmt::tick(); assert(locks[0]->count==1); // no controller, even long link
  xtalReady=true; hunt=true;
  power_mgmt::tick(); assert(locks[0]->count==1); // sensor hunt still protected
#endif
 }
 for(auto lock:locks) delete lock;
 puts("PM fault/guard checks passed");
}
'''
exe = build('pm', pm)
for mode in ['normal', 'first-lock-fail', 'second-lock-fail', 'stock']:
    subprocess.run([str(exe), mode], check=True)
subprocess.run([str(build('pm-xtal', '#define PM_BLE_XTAL 1\n' + pm))], check=True)
exe = build('pm-dfs', '#define PM_BLE_XTAL 1\n#define PM_MIN_CPU_MHZ 80\n#define PM_LIGHT_SLEEP 0\n' + pm)
for mode in ['normal', 'first-lock-fail', 'second-lock-fail', 'stock']:
    subprocess.run([str(exe), mode], check=True)

logger = common + r'''
#include <ctime>
#include "diag.h"
#include "diag_buffer.h"
struct Mutex { bool held=false; };
using SemaphoreHandle_t=Mutex*;
constexpr int portMAX_DELAY=0, MALLOC_CAP_SPIRAM=0, MALLOC_CAP_8BIT=1;
bool failExternal=false;
void* heap_caps_malloc(size_t n,int caps) { return failExternal && caps==MALLOC_CAP_SPIRAM ? nullptr : malloc(n); }
bool esp_ptr_external_ram(void*) { return true; }
SemaphoreHandle_t xSemaphoreCreateMutex(){return new Mutex;}
void xSemaphoreTake(Mutex* m,int){assert(!m->held);m->held=true;}
void xSemaphoreGive(Mutex* m){assert(m->held);m->held=false;}
bool cardMounted=false, recording=false;
namespace ride_recorder { bool sdMounted(){return cardMounted;} bool isRecording(){return recording;} }
namespace settings { int tzMinutes(){return 0;} }
namespace power_mgmt { void busyAcquire(){} void busyRelease(){} }
int sdDepth=0, opens=0, flashWrites=0;
void sdLock(){++sdDepth;} void sdUnlock(){assert(sdDepth>0);--sdDepth;}
size_t acceptBytes=SIZE_MAX;
bool openOk=true;
std::string disk, flash;
constexpr int FILE_APPEND=1;
struct File {
 bool valid=true;
 operator bool() const{return valid;}
 size_t write(const uint8_t* p,size_t n) { assert(sdDepth);n=std::min(n,acceptBytes);disk.append((const char*)p,n);return n; }
 void close(){} void flush(){}
};
struct FakeSD {
 bool exists(const char*){return true;}
 bool mkdir(const char*){return true;}
 File open(const char*,int){++opens;return File{openOk};}
} SD;
struct Preferences {
 bool begin(const char*,bool){return true;}
 size_t getBytesLength(const char*){return flash.size();}
 size_t getBytes(const char*,void* p,size_t n){memcpy(p,flash.data(),n);return n;}
 size_t putBytes(const char*,const void* p,size_t n){++flashWrites;flash.assign((const char*)p,n);return n;}
 void end(){}
};
void diag::drainDriverLogs(){}
''' + production('diag.cpp') + r'''
int main(int argc,char** argv){
 if(argc>1 && std::string(argv[1])=="low-memory") {
  failExternal=true; diag::begin(); assert(pending.capacity==8192); assert(!boot);
  for(int i=0;i<1000;++i) diag::log("fallback line %d",i);
  assert(pending.size<=8192 && pending.dropped>0);
  diag::dumpToSerial(); diag::checkpoint("fallback"); assert(flashWrites==1);
  free(buf); delete mtx; puts("Logger low-memory fallback passed"); return 0;
 }
 diag::begin(); diag::log("boot sentinel"); diag::finishBoot();
 std::string original(pending.data,pending.size);
 diag::flushToSD(); assert(opens==0); assert(pending.size==original.size());
 diag::checkpoint("card absent"); assert(flashWrites==1); assert(flash.find("boot sentinel")!=std::string::npos);
 diag::checkpoint("again"); assert(flashWrites==1);
 cardMounted=true; acceptBytes=7;
 diag::flushToSD(); assert(disk==original.substr(0,7)); assert(pending.size>0);
 std::string remaining(pending.data,pending.size);
 int previousOpens=opens; diag::flushToSD(); assert(opens==previousOpens); // backoff
 nowMs+=30000; acceptBytes=SIZE_MAX; diag::flushToSD();
 assert(disk.substr(7)==remaining); assert(pending.size==0);
 diag::log("open failure"); original.assign(pending.data,pending.size);
 openOk=false; diag::flushToSD(); assert(std::string(pending.data,pending.size).find(original)==0);
 openOk=true; nowMs+=30000; diag::flushToSD(); assert(pending.size==0);
 // A long card outage must leave the protected boot record recoverable.
 cardMounted=false;
 for(int i=0;i<5000;++i) diag::log("outage line %d abcdefghijklmnopqrstuvwxyz",i);
 assert(pending.dropped>0); assert(std::string(boot,bootLen).find("boot sentinel")!=std::string::npos);
 cardMounted=true; diag::flushToSD(); assert(disk.find("retained boot after log overflow")!=std::string::npos);
 assert(pending.size==0); assert(flashWrites==1);
 free(buf); free(boot); delete mtx;
 puts("Logger missing-card/short-write/open-failure/overflow/checkpoint checks passed");
}
'''
logger_exe = build('logger', logger)
subprocess.run([str(logger_exe)], check=True)
subprocess.run([str(logger_exe), 'low-memory'], check=True)

buffer_test = r'''
#include <cassert>
#include <string>
#include "diag_buffer.h"
int main(){
 char bytes[12]; DiagBuffer b(bytes,sizeof(bytes));
 b.append("first\n",6); b.append("two\n",4); b.append("three\n",6);
 assert(std::string(b.data,b.size)=="two\nthree\n"); assert(b.dropped==6);
 b.consume(2); assert(std::string(b.data,b.size)=="o\nthree\n");
 b.consume(100); assert(b.size==0);
 b.append("0123456789012345",16); assert(b.size==0 && b.dropped==22);
}
'''
subprocess.run([str(build('buffer', buffer_test))], check=True)
print('All power/SD diagnostic host checks passed.')

# Exercise the actual driver logging wrapper and its deferred queue: calling
# diag::log while the queue spinlock is held would assert here.
driver = common + r'''
#include "diag.h"
#include "diag_buffer.h"
int forwarded=0;
std::vector<std::string> lines;
extern "C" int log_printfv(const char*,va_list){++forwarded;return 1;}
''' + production('sd_driver_log.cpp') + r'''
void diag::log(const char* format,...) {
 assert(!mux.held);
 char line[512]; va_list args; va_start(args,format);
 vsnprintf(line,sizeof(line),format,args); va_end(args); lines.emplace_back(line);
}
int main(){
 __wrap_log_printf("[%s:%u] %s\n","sd_diskio.cpp",805,"f_mount failed");
 assert(forwarded==0 && lines.empty());
 diag::drainDriverLogs(); assert(lines.size()==1 && lines[0].find("f_mount failed")!=std::string::npos);
 __wrap_log_printf("unrelated log\n"); assert(forwarded==1);
 for(int i=0;i<1000;++i) __wrap_log_printf("[sd_diskio.cpp:10] failure %d\n",i);
 diag::drainDriverLogs();
 assert(std::any_of(lines.begin(),lines.end(),[](const std::string& s){return s.find("log bytes dropped")!=std::string::npos;}));
 puts("SD driver deferred-logging/overflow checks passed");
}
'''
subprocess.run([str(build('driver', driver))], check=True)

# Serial SD retrieval must reject missing/repeated chunks, even if a final
# marker claims success. Other tasks may log between the framed chunks.
import importlib.util
spec = importlib.util.spec_from_file_location('power_sd_decode', ROOT / 'tools/power_sd_decode.py')
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)
capture = ('[sdlog] begin path=/logs/test.log start=10 end=14 encoding=hex\n'
           '[sdlog] data 0000000a 6162\nother task output\n'
           '[sdlog] data 0000000c 630a\n'
           '[sdlog] end offset=14 expected=14 complete=1\n')
assert decoder.decode(capture) == b'abc\n'
for broken in [capture.replace('0000000c', '0000000d'),
               capture.replace('0000000c', '0000000a'),
               capture.replace('complete=1', 'complete=0'),
               capture.split('[sdlog] end')[0]]:
    try:
        decoder.decode(broken)
    except ValueError:
        pass
    else:
        raise AssertionError('Incomplete/corrupt serial SD transfer accepted')
print('Serial SD decoder interleaving/gap/overlap/truncation checks passed')

# Exercise the real patched idle loop with a binary-semaphore mock. A signal
# arriving before the wait must survive; stale signals must not create polling.
spec = importlib.util.spec_from_file_location('epd_idle_patch', ROOT / 'tools/epd_idle_patch.py')
epd_patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(epd_patch)
epd = ROOT / '.pio/libdeps/t5s3-painter-ble-xtal/EPD Painter/src'
header, source = epd_patch.patched((epd / 'EPD_Painter.h').read_text(), (epd / 'EPD_Painter.cpp').read_text())
assert epd_patch.patched(header, source) == (header, source)
assert source.count('xSemaphoreGive(_paint_work_sem);') == 6
idle = re.search(r'while \(paintStage == 0\) \{\s*xSemaphoreTake\(_paint_work_sem, portMAX_DELAY\);\s*}', source)[0]
idle_test = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
struct Semaphore { std::mutex m; std::condition_variable cv; bool token=false; } semaphore;
Semaphore* _paint_work_sem=&semaphore;
constexpr int portMAX_DELAY=0;
std::atomic<int> paintStage{0}, takes{0};
void xSemaphoreTake(Semaphore* s,int) {
 std::unique_lock<std::mutex> lock(s->m); ++takes;
 s->cv.wait(lock,[&]{return s->token;}); s->token=false;
}
void give() { std::lock_guard<std::mutex> lock(semaphore.m); semaphore.token=true; semaphore.cv.notify_one(); }
void idle() {
''' + idle + r'''
}
void waitForTake(int n) {
 auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(takes<n && std::chrono::steady_clock::now()<end) std::this_thread::yield();
 assert(takes>=n);
}
int main() {
 // Submission before entering idle: stage check sees work; token remains.
 paintStage=2; give(); idle(); assert(takes==0);
 // Next idle consumes the old token then truly blocks until new work.
 paintStage=0; std::atomic<bool> done{false};
 std::thread worker([&]{idle();done=true;}); waitForTake(2);
 std::this_thread::sleep_for(std::chrono::milliseconds(20)); assert(!done && takes==2);
 paintStage=2; give(); worker.join(); assert(done);
 // A wake without new work must recheck the predicate and block again.
 paintStage=0; done=false;
 std::thread next([&]{idle();done=true;}); waitForTake(3);
 give(); waitForTake(4); assert(!done);
 paintStage=1; give(); next.join(); assert(done);
}
'''
subprocess.run([str(build('epd-idle', idle_test))], check=True)
print('Display work-wait early-submit/stale-wake/idle-block checks passed')

interval_test = r'''
#include <cassert>
#include "ble_interval_policy.h"
int main() {
 using R = BleIntervalPolicy::Request;
 BleIntervalPolicy p; p.reset(100);
 assert(p.update(15099, 15000, false, 24)==R::None);
 assert(p.update(15100, 15000, false, 24)==R::Relaxed);
 // A queued request is not an accepted interval; retry with a bounded budget.
 assert(p.update(75099, 75000, false, 24)==R::None);
 assert(p.update(75100, 75000, false, 24)==R::Relaxed);
 assert(p.update(135100,135000,false,24)==R::Relaxed);
 assert(p.update(195100,195000,false,24)==R::None);
 assert(p.requestAttempts()==3);
 // Acceptance and later central override are observed, not assumed.
 assert(p.update(195200,195100,false,240)==R::None);
 assert(p.requestAttempts()==0);
 assert(p.update(210200,210100,false,24)==R::Relaxed);
 assert(p.update(210300,210200,false,240)==R::None);
 // Bulk traffic switches back, respecting the three-second request guard.
 assert(p.update(210301,0,true,240)==R::None);
 assert(p.update(213200,0,true,240)==R::Fast);
 assert(p.update(213201,0,true,24)==R::None);
 assert(p.update(223200,7999,false,24)==R::None);
 assert(p.update(228200,8000,false,24)==R::Relaxed);
 // An active OTA must never relax even if writes pause.
 p.reset(0);
 assert(p.update(30000,30000,true,24)==R::None);
 assert(!p.wantsRelaxed());
 assert(p.update(30001,30001,false,24)==R::Relaxed);
 // Reconnect starts a fresh grace window. Unsigned timers survive wrap.
 p.reset(UINT32_MAX-9999);
 assert(p.update(4999,15000,false,24)==R::None);
 assert(p.update(5000,15000,false,24)==R::Relaxed);
 assert(p.update(5001,15001,false,120)==R::None);
}
'''
subprocess.run([str(build('ble-interval-policy', interval_test))], check=True)
print('BLE interval grace/quiet/retry/negotiation/bulk/OTA/reconnect/wrap checks passed')

audit_test = r'''
#include "gps_sentence_audit.h"
#include <cassert>
#include <string>
#include <cstdio>
void sentence(GpsSentenceAudit& a, const char* body) {
 unsigned ck=0; for(const char* p=body;*p;++p) ck^=(unsigned char)*p;
 char tail[8]; snprintf(tail,sizeof(tail),"*%02X\r\n",ck);
 for(char c:std::string("$")+body+tail) a.feed(c);
}
int main() {
 GpsSentenceAudit a;
 sentence(a,"GNGGA,235959.00,,,,"); sentence(a,"GPGGA,000000.00,,,,");
 assert(a.counts.gga==2 && a.counts.missingGga==0);
 sentence(a,"GNGGA,000003.00,,,,"); assert(a.counts.missingGga==2);
 sentence(a,"GNRMC,000001.00,A,,,,"); sentence(a,"GNRMC,000001.00,A,,,,");
 sentence(a,"GNRMC,000004.00,A,,,,"); assert(a.counts.rmc==3 && a.counts.missingRmc==2);
 for(char c:std::string("$GNRMC,000005*00\n")) a.feed(c);
 assert(a.counts.bad==1 && a.counts.rmc==3);
 for(char c:std::string("$partial")) a.feed(c);
 sentence(a,"GNRMC,000005.00,A,,,,"); assert(a.counts.truncated==1);
 a.feed('$'); for(int i=0;i<200;i++) a.feed('x');
 assert(a.counts.truncated==2);
 sentence(a,"GNGGA,992000.00,,,,"); // invalid UTC does not alter epoch
 sentence(a,"GNGGA,000004.00,,,,"); assert(a.counts.missingGga==2);
}
'''
subprocess.run([str(build('gps-sentence-audit', audit_test))], check=True)
print('GPS sentence checksum/truncation/epoch-gap/duplicate/day-wrap checks passed')

rx_guard_test = common + r'''
#include "gps_rx_guard.h"
#define PM_GPS_RX_GUARD 1
#define PM_BLE_XTAL 1
#define CONFIG_IDF_TARGET_ESP32S3 1
#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(4,4,6)
#define IRAM_ATTR
#define DRAM_ATTR
#define BOARD_GPS_RXD 44
constexpr int GPIO_INTR_LOW_LEVEL=4;
constexpr int ESP_SLEEP_WAKEUP_GPIO=7;
using gpio_num_t=int;
int setupFailure=0, wakeCause=0, permanentHold=0;
int64_t microsNow=0;
bool (*skipCallback)()=nullptr;
int64_t esp_timer_get_time(){return microsNow;}
int esp_pm_register_skip_light_sleep_callback(bool(*cb)()) {
 if(setupFailure==1) return ESP_ERR_NOT_SUPPORTED;
 skipCallback=cb;return ESP_OK;
}
int gpio_wakeup_enable(int,int){return setupFailure==2?ESP_ERR_INVALID_STATE:ESP_OK;}
int esp_sleep_enable_gpio_wakeup(){return setupFailure==3?ESP_ERR_INVALID_STATE:ESP_OK;}
int esp_sleep_get_wakeup_cause(){return wakeCause;}
struct {struct {uint32_t val=1<<12;} in1;} GPIO;
struct {struct {uint32_t rxfifo_cnt=0;} status; struct {uint32_t st_urx_out=0;} fsm_status;} UART2;
namespace power_mgmt {void busyAcquire(){++permanentHold;}}
namespace diag {void log(const char*,...) {}}
''' + production('gps_rx_guard.cpp') + r'''
int main(int argc,char** argv) {
 if(argc>1) setupFailure=atoi(argv[1]);
 gps_rx_guard::begin();
 if(setupFailure){assert(permanentHold==1);return 0;}
 assert(!skipCallback());
 GPIO.in1.val=0; assert(!gps_rx_guard::beforeSleep());
 GPIO.in1.val=1<<12; microsNow=49999; gps_rx_guard::tick(false); assert(skipCallback());
 // Repeated callbacks must not extend the quiet deadline indefinitely.
 microsNow=50000; gps_rx_guard::tick(false); assert(!skipCallback());
 // The wake level may already be high when the wrapper returns.
 wakeCause=ESP_SLEEP_WAKEUP_GPIO; gps_rx_guard::afterSleep(); assert(skipCallback());
 wakeCause=0; microsNow=100000; UART2.status.rxfifo_cnt=1;
 gps_rx_guard::tick(false); assert(skipCallback());
 UART2.status.rxfifo_cnt=0; microsNow=149999; gps_rx_guard::tick(false); assert(skipCallback());
 microsNow=150000; gps_rx_guard::tick(false); assert(!skipCallback());
 // A UART frame in progress is protected even while RX is temporarily high.
 UART2.fsm_status.st_urx_out=1; assert(!gps_rx_guard::beforeSleep());
 UART2.fsm_status.st_urx_out=0; microsNow=200000;
 gps_rx_guard::tick(true); assert(skipCallback());
 microsNow=250000; gps_rx_guard::tick(false); assert(!skipCallback());
 gps_rx_guard::afterSleep(); assert(!skipCallback());
 gps_rx_guard::report();
}
'''
rx_guard_binary=build('gps-rx-guard',rx_guard_test)
for scenario in ['0','1','2','3']:
    subprocess.run([str(rx_guard_binary),scenario],check=True)
print('GPS RX guard wake/entry-race/FIFO/frame/quiet/setup-failure checks passed')
