#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <atomic>
#include <ctime>
#include <string>
#include <map>
#include <algorithm>
#include "crash_record.h"
#include "crash_report.h"
#include "memfault_service.h"
int memfaultDeliveries=0;
#if OT_MEMFAULT
namespace memfault_service {bool pending(size_t* p){if(p)*p=4096;return true;}const char* pendingId(){return "test-id";}void deliverNow(const char*){++memfaultDeliveries;}}
#endif
#define RTC_NOINIT_ATTR
#define CONFIG_IDF_TARGET_ESP32S3 1
#define CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH 1
#define CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF 1
constexpr int ESP_RST_PANIC=4, ESP_RST_INT_WDT=5, ESP_RST_TASK_WDT=6,ESP_RST_WDT=7,ESP_RST_BROWNOUT=9;
[[maybe_unused]] constexpr int ESP_OK=0,ESP_ERR_NO_MEM=257;
constexpr int FILE_READ=0,FILE_WRITE=1;
using esp_err_t=int;
const char* esp_err_to_name(int n){return n==0?"ESP_OK":"ESP_ERR_NOT_FOUND";}
struct portMUX_TYPE {bool held=false;};
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) do{assert(!(m)->held);(m)->held=true;}while(0)
#define portEXIT_CRITICAL(m) do{assert((m)->held);(m)->held=false;}while(0)
uint32_t nowMs=100,randomId=10;
uint32_t millis(){return nowMs;} uint32_t esp_random(){return randomId++;}
struct App {uint8_t app_elf_sha256[32]{};} app;
const App* esp_ota_get_app_description(){return &app;}
struct esp_core_dump_summary_t {
 char exc_task[16]{};uint32_t exc_pc=0;uint8_t app_elf_sha256[65]{};
 struct {bool corrupted=false;uint32_t depth=0;uint32_t bt[16]{};} exc_bt_info;
 struct {uint32_t exc_cause=0,exc_vaddr=0,exc_a[16]{},epcx[7]{};uint8_t epcx_reg_bits=0;} ex_info;
};
int coreResult=1,erases=0;
int esp_core_dump_get_summary(esp_core_dump_summary_t* s){strcpy(s->exc_task,"GPS");s->exc_pc=0x42012345;s->exc_bt_info.depth=1;s->exc_bt_info.bt[0]=0x42012345;return coreResult;}
int esp_core_dump_image_erase(){++erases;return 0;}
std::string logs;
struct Console {void print(const char* p){logs+=p;}} Serial;
namespace diag {void log(const char* fmt,...){char b[512];va_list a;va_start(a,fmt);vsnprintf(b,sizeof(b),fmt,a);va_end(a);logs+=b;logs+='\n';}}
bool recording=false;
bool mounted=false,host=false,nvsOK=true,writeOK=true,removeOK=true,renameOK=true;
size_t sdLimit=SIZE_MAX;
int sdDepth=0,powerDepth=0;
namespace power_mgmt {void busyAcquire(){++powerDepth;}void busyRelease(){assert(powerDepth>0);--powerDepth;}}
namespace ride_recorder {bool sdMounted(){return mounted;}bool isRecording(){return recording;}}
namespace usb_storage {bool hostActive(){return host;}}
void sdLock(){++sdDepth;}void sdUnlock(){assert(sdDepth>0);--sdDepth;}
std::map<std::string,std::string> nvs,files;
struct Preferences {
 bool begin(const char*,bool){assert(!sdDepth);return nvsOK;}
 size_t getBytesLength(const char* k){assert(!sdDepth);return nvs.count(k)?nvs[k].size():0;}
 size_t getBytes(const char* k,void* p,size_t n){assert(!sdDepth);n=std::min(n,nvs[k].size());memcpy(p,nvs[k].data(),n);return n;}
 size_t putBytes(const char* k,const void* p,size_t n){assert(!sdDepth);if(!writeOK)return 0;nvs[k]=std::string((const char*)p,n);return n;}
 uint32_t getUInt(const char* k,uint32_t d){assert(!sdDepth);if(!nvs.count(k)||nvs[k].size()!=4)return d;uint32_t v;memcpy(&v,nvs[k].data(),4);return v;}
 size_t putUInt(const char* k,uint32_t v){assert(!sdDepth);if(!writeOK)return 0;nvs[k]=std::string((const char*)&v,4);return 4;}
 bool remove(const char* k){assert(!sdDepth);if(!removeOK)return false;nvs.erase(k);return true;}
 void end(){assert(!sdDepth);}
};
struct File {
 std::string path;size_t offset=0;bool valid=false;
 operator bool()const{return valid;}
 size_t size(){return files[path].size();}
 size_t write(const uint8_t* p,size_t n){assert(sdDepth);n=std::min(n,sdLimit);files[path].append((const char*)p,n);return n;}
 size_t read(uint8_t* p,size_t n){assert(sdDepth);n=std::min(n,files[path].size()-offset);memcpy(p,files[path].data()+offset,n);offset+=n;return n;}
 void close(){}void flush(){}
};
struct Disk {
 bool exists(const char* p){return files.count(p);}
 bool mkdir(const char*){return true;}
 File open(const char* p,int mode){assert(sdDepth);if(mode==FILE_WRITE)files[p]="";return File{p,0,bool(files.count(p))};}
 bool rename(const char* a,const char* b){assert(sdDepth);if(!renameOK)return false;files[b]=files[a];files.erase(a);return true;}
} SD;
// PRODUCTION
void reboot(int reason=1){captureReady=false;sequence=0;current={};scratch={};currentPending=currentDurable=coreAwaitingDurability=false;nextRetry=0;failedAttempts=0;nowMs=100;crash_report::begin(reason,reason==1?"power-on":"interrupt watchdog","test");}
void tick(){nowMs+=30001;crash_report::tick();assert(!powerDepth&&!sdDepth);}
int reports(){int n=0;for(auto& kv:nvs)if(kv.first.rfind("report",0)==0)++n;return n;}
size_t reportBytes(){size_t n=0;for(auto& kv:nvs)if(kv.first.rfind("report",0)==0)n+=kv.second.size();return n;}
bool delivered(uint32_t id){char p[64];snprintf(p,sizeof(p),"/logs/crash-%08lx.log",(unsigned long)id);return files.count(p);}
void clean(){nvs.clear();files.clear();memset(tail,0,sizeof(tail));mounted=host=false;nvsOK=writeOK=removeOK=renameOK=true;sdLimit=SIZE_MAX;coreResult=1;erases=0;reboot();}
int main(){
 clean();tick();assert(nvs.empty()&&files.empty());
 recording=true;crash_report::requestTestPanic();tick();assert(nvs.empty());recording=false;
 crash_report::recordLine("last GPS window clean\n",22);reboot(5);
 assert(reports()==1&&current.valid());assert(std::string(current.text).find("last GPS window clean")!=std::string::npos);assert(std::string(current.text).find(OT_MEMFAULT ? "crash_backend=Memfault" : "No decodable core dump")!=std::string::npos);
 tick();assert(reports()==1);reboot();mounted=true;tick();assert(nvs.empty()&&files.size()==1);
 // A short SD write and a failed rename preserve NVS. Retry verifies content.
 clean();reboot(5);mounted=true;sdLimit=10;tick();assert(reports()==1);sdLimit=SIZE_MAX;renameOK=false;tick();assert(reports()==1);renameOK=true;tick();assert(nvs.empty()&&files.size()==1);
 // Completed file + failed NVS remove is replayed without duplicate append.
 clean();reboot(5);mounted=true;removeOK=false;tick();assert(reports()==1);auto saved=files;reboot();removeOK=true;tick();assert(nvs.empty()&&saved==files);
 // Missing NVS still permits direct SD persistence; no premature core erase.
 clean();nvsOK=false;coreResult=0;reboot(5);assert(erases==0&&currentPending);mounted=true;tick();assert(erases==(OT_MEMFAULT ? 0 : 1)&&!currentPending&&files.size()==1);
 // A durable flash report protects summary before acknowledging core payload.
 clean();coreResult=0;reboot(4);assert(erases==(OT_MEMFAULT ? 0 : 1)&&reports()==1);assert(std::string(current.text).find(OT_MEMFAULT ? "crash_backend=Memfault" : "42012345")!=std::string::npos);
 // Four crashes with no card fill four slots; a fifth keeps the OLDEST and
 // replaces the newest, so it survives a reset instead of staying RAM-only.
 clean();uint32_t ids[5];for(int i=0;i<5;++i){reboot(5);ids[i]=current.id;}
 assert(reports()==4&&currentPending&&currentDurable);reboot();mounted=true;tick();
 assert(nvs.empty()&&files.size()==4&&delivered(ids[0])&&delivered(ids[1])&&delivered(ids[2])&&!delivered(ids[3])&&delivered(ids[4]));
 // Byte cap: large reports never exceed NVS_BYTE_CAP; still oldest + latest kept.
 clean();{uint32_t bigIds[6];std::string big(180,'x');big+='\n';
  for(int i=0;i<6;++i){for(int j=0;j<10;++j)crash_report::recordLine(big.c_str(),big.size());reboot(5);bigIds[i]=current.id;
   assert(reportBytes()<=NVS_BYTE_CAP&&currentDurable);}
  assert(reports()==2);mounted=true;tick();assert(files.size()==2&&delivered(bigIds[0])&&delivered(bigIds[5]));}
 // Mount hook delivers immediately (no 30 s wait) and also kicks Memfault.
 clean();reboot(5);memfaultDeliveries=0;mounted=true;crash_report::onSdMounted("boot mount");assert(nvs.empty()&&files.size()==1&&memfaultDeliveries==OT_MEMFAULT&&!sdDepth&&!powerDepth);
 // Mount hook never touches the card while a USB host owns it.
 clean();reboot(5);mounted=host=true;crash_report::onSdMounted("late mount");assert(files.empty()&&reports()==1);host=false;
 // Pending work retries every ~3 s, then backs off to 30 s.
 clean();reboot(5);for(int i=0;i<25;++i){nowMs+=3001;crash_report::tick();}
 assert(int32_t(nextRetry-nowMs)>3001);mounted=true;nowMs+=3001;crash_report::tick();assert(reports()==1);
 nowMs+=30001;crash_report::tick();assert(nvs.empty()&&files.size()==1);
 clean();reboot(5);nowMs+=3001;crash_report::tick();mounted=true;nowMs+=3001;crash_report::tick();assert(nvs.empty()&&files.size()==1);
 // USB host ownership prohibits SD access; report persists across normal reboot.
 clean();reboot(5);mounted=host=true;tick();assert(files.empty()&&reports()==1);reboot();host=false;tick();assert(nvs.empty()&&files.size()==1);
 // CRC rejects corrupted headers, body, RTC tail and an interrupted tail write.
 crash_record::Record r{};r.id=1;strcpy(r.text,"test");r.length=4;r.seal();assert(r.valid());r.id^=1;assert(!r.valid());r.id^=1;r.text[0]^=1;assert(!r.valid());
 crash_record::Line l{};l.write(1,5,"test",4);assert(l.valid());l.sequence=0;assert(!l.valid());l.write(1,5,"test",4);l.text[0]^=1;assert(!l.valid());
 puts("Crash report boot/NVS/SD/short-write/reboot/CRC/queue/byte-cap/mount-hook/retry tests passed");
}
