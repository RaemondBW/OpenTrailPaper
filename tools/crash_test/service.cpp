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
#if OT_MEMFAULT
namespace memfault_service {bool pending(size_t* p){if(p)*p=4096;return true;}const char* pendingId(){return "test-id";}}
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
void reboot(int reason=1){captureReady=false;sequence=0;current={};scratch={};currentPending=currentDurable=coreAwaitingDurability=false;nextRetry=0;nowMs=100;crash_report::begin(reason,reason==1?"power-on":"interrupt watchdog","test");}
void tick(){nowMs+=30001;crash_report::tick();assert(!powerDepth&&!sdDepth);}
void clean(){nvs.clear();files.clear();memset(tail,0,sizeof(tail));mounted=host=false;nvsOK=writeOK=removeOK=renameOK=true;sdLimit=SIZE_MAX;coreResult=1;erases=0;reboot();}
int main(){
 clean();tick();assert(nvs.empty()&&files.empty());
 recording=true;crash_report::requestTestPanic();tick();assert(nvs.empty());recording=false;
 crash_report::recordLine("last GPS window clean\n",22);reboot(5);
 assert(nvs.size()==1&&current.valid());assert(std::string(current.text).find("last GPS window clean")!=std::string::npos);assert(std::string(current.text).find(OT_MEMFAULT ? "crash_backend=Memfault" : "No decodable core dump")!=std::string::npos);
 tick();assert(nvs.size()==1);reboot();mounted=true;tick();assert(nvs.empty()&&files.size()==1);
 // A short SD write and a failed rename preserve NVS. Retry verifies content.
 clean();reboot(5);mounted=true;sdLimit=10;tick();assert(nvs.size()==1);sdLimit=SIZE_MAX;renameOK=false;tick();assert(nvs.size()==1);renameOK=true;tick();assert(nvs.empty()&&files.size()==1);
 // Completed file + failed NVS remove is replayed without duplicate append.
 clean();reboot(5);mounted=true;removeOK=false;tick();assert(nvs.size()==1);auto saved=files;reboot();removeOK=true;tick();assert(nvs.empty()&&saved==files);
 // Missing NVS still permits direct SD persistence; no premature core erase.
 clean();nvsOK=false;coreResult=0;reboot(5);assert(erases==0&&currentPending);mounted=true;tick();assert(erases==(OT_MEMFAULT ? 0 : 1)&&!currentPending&&files.size()==1);
 // A durable flash report protects summary before acknowledging core payload.
 clean();coreResult=0;reboot(4);assert(erases==(OT_MEMFAULT ? 0 : 1)&&nvs.size()==1);assert(std::string(current.text).find(OT_MEMFAULT ? "crash_backend=Memfault" : "42012345")!=std::string::npos);
 // Four crashes with no card retain four reports; a fifth stays in RAM.
 clean();for(int i=0;i<5;++i)reboot(5);assert(nvs.size()==4&&currentPending&&!currentDurable);mounted=true;tick();assert(nvs.empty()&&files.size()==5);
 // USB host ownership prohibits SD access; report persists across normal reboot.
 clean();reboot(5);mounted=host=true;tick();assert(files.empty()&&nvs.size()==1);reboot();host=false;tick();assert(nvs.empty()&&files.size()==1);
 // CRC rejects corrupted headers, body, RTC tail and an interrupted tail write.
 crash_record::Record r{};r.id=1;strcpy(r.text,"test");r.length=4;r.seal();assert(r.valid());r.id^=1;assert(!r.valid());r.id^=1;r.text[0]^=1;assert(!r.valid());
 crash_record::Line l{};l.write(1,5,"test",4);assert(l.valid());l.sequence=0;assert(!l.valid());l.write(1,5,"test",4);l.text[0]^=1;assert(!l.valid());
 puts("Crash report boot/NVS/SD/short-write/reboot/CRC/queue tests passed");
}
