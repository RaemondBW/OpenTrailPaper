#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <atomic>
#include <string>
#include <map>
#include <algorithm>
#include "memfault_service.h"
#include "memfault/core/data_export.h"
#include "memfault/core/data_packetizer.h"
#include "memfault/core/data_packetizer_source.h"
#include "memfault/panics/coredump.h"
#include "memfault/panics/platform/coredump.h"
#include "memfault/core/platform/debug_log.h"

uint32_t nowMs=0; uint32_t millis(){return nowMs;} void vTaskDelay(int){}
std::string fixture() {
 std::string s;
 auto word=[&](uint32_t n){for(int i=0;i<4;++i)s+=char(n>>(i*8));};
 word(0x45524f43);word(2);word(1064);
 word(1);word(0x3fc00000);word(1024);s+=std::string(1024,'x');
 word(0x504d5544);word(0);word(0);word(0);return s;
}
std::string serial, logs, flashData=fixture();
bool flashValid=true, readOK=true, clearOK=true, mounted=false, host=false, renameOK=true, corruptClose=false;
size_t limit=SIZE_MAX;
int cleared=0, sdDepth=0, powerDepth=0;
std::map<std::string,std::string> files;
constexpr int FILE_READ=0,FILE_WRITE=1;
namespace diag {void flushToSD(){} void log(const char* fmt,...){char s[512];va_list a;va_start(a,fmt);vsnprintf(s,sizeof(s),fmt,a);va_end(a);logs+=s;logs+='\n';}}
struct Console {
 void println(const char* p){serial+=p;serial+='\n';}
 void printf(const char* fmt,...){char s[256];va_list a;va_start(a,fmt);vsnprintf(s,sizeof(s),fmt,a);va_end(a);serial+=s;}
 size_t write(const uint8_t* p,size_t n){serial.append((const char*)p,n);return n;}
} Serial;
namespace power_mgmt {void busyAcquire(){++powerDepth;} void busyRelease(){assert(powerDepth>0);--powerDepth;}}
bool recording=false;
namespace ride_recorder {bool sdMounted(){return mounted;}bool isRecording(){return recording;}}
namespace usb_storage {bool hostActive(){return host;}}
void sdLock(){assert(!sdDepth);++sdDepth;} void sdUnlock(){assert(sdDepth);--sdDepth;}
struct File {
 std::string path; size_t offset=0; bool valid=false, writable=false;
 operator bool() const {return valid;}
 size_t size(){return files[path].size();}
 size_t write(const uint8_t* p,size_t n){assert(sdDepth);n=std::min(n,limit);files[path].append((const char*)p,n);return n;}
 size_t write(uint8_t b){return write(&b,1);}
 size_t read(uint8_t* p,size_t n){assert(sdDepth);n=std::min(n,files[path].size()-offset);memcpy(p,files[path].data()+offset,n);offset+=n;return n;}
 int read(){uint8_t b;return read(&b,1)?b:-1;}
 void flush(){} void close(){if(writable&&corruptClose&&!files[path].empty())files[path][0]^=1;}
};
struct Disk {
 bool exists(const char* p){assert(sdDepth);return files.count(p);}
 bool mkdir(const char*){return true;}
 File open(const char* p,int mode){assert(sdDepth);if(mode==FILE_WRITE)files[p]="";return {p,0,bool(files.count(p)),mode==FILE_WRITE};}
 bool rename(const char* a,const char* b){assert(sdDepth);if(!renameOK)return false;files[b]=files[a];files.erase(a);return true;}
} SD;
// Hash primitive is substituted; storage/packetization/export logic is production.
struct mbedtls_sha256_context {};
void mbedtls_sha256_init(mbedtls_sha256_context*){}
int mbedtls_sha256_starts_ret(mbedtls_sha256_context*,int){return 0;}
int mbedtls_sha256_update_ret(mbedtls_sha256_context*,const uint8_t*,size_t){return 0;}
int mbedtls_sha256_finish_ret(mbedtls_sha256_context*,uint8_t* p){memset(p,0x5a,32);return 0;}
void mbedtls_sha256_free(mbedtls_sha256_context*){}
extern "C" {
void memfault_platform_log(eMemfaultPlatformLogLevel,const char*,...){}
void memfault_sdk_assert_func(){abort();}
bool memfault_coredump_has_valid_coredump(size_t* n){if(n)*n=flashData.size();return flashValid;}
void memfault_platform_coredump_storage_get_info(sMfltCoredumpStorageInfo* p){*p={65536,4096};}
bool memfault_platform_coredump_storage_read(uint32_t o,void* p,size_t n){if(!readOK||o>flashData.size()||n>flashData.size()-o)return false;memcpy(p,flashData.data()+o,n);return true;}
void __wrap_memfault_platform_coredump_storage_clear();
bool memfault_coredump_read(uint32_t,void*,size_t);
void __real_memfault_platform_coredump_storage_clear(){assert(!sdDepth);++cleared;if(clearOK)flashValid=false;}
extern const sMemfaultDataSourceImpl g_memfault_coredump_data_source={memfault_coredump_has_valid_coredump,memfault_coredump_read,__wrap_memfault_platform_coredump_storage_clear};
}
// PRODUCTION
std::string downloadedPath;
uint8_t notification=0;
void streamFileWindowed(const char* path,const char*){downloadedPath=path;}
void notifyByte(uint8_t b){notification=b;}
// BLE LOG DOWNLOAD
void reboot(){id[0]=0;retryAt=0;requested=0;consumed=false;memfault_packetizer_abort();}
void reset(){files.clear();flashValid=readOK=clearOK=renameOK=true;mounted=host=corruptClose=false;limit=SIZE_MAX;cleared=0;serial.clear();logs.clear();reboot();}
void tick(){nowMs+=30001;memfault_service::tick();assert(!sdDepth&&!powerDepth);}
int main(int argc,char** argv){
 reset();tick();assert(flashValid&&files.empty()&&!cleared);
 // Real SDK packetizer/exporter: export twice produces identical chunks and
 // neither run consumes flash, including its final-chunk acknowledgement.
 memfault_service::requestStatus(true);tick();auto first=serial;assert(first.find("MC:")!=std::string::npos&&first.find("complete=1")!=std::string::npos&&flashValid&&!cleared);
 if(argc>1){FILE* f=fopen(argv[1],"wb");assert(f);assert(fwrite(first.data(),1,first.size(),f)==first.size());fclose(f);}
 serial.clear();memfault_service::requestStatus(true);tick();assert(serial==first&&flashValid&&!cleared);
 // Reboot without SD keeps the dump available.
 reboot();tick();assert(flashValid&&!cleared);mounted=host=true;tick();assert(files.empty());host=false;
 // Short write, bad readback and failed rename never acknowledge the dump.
 limit=5;tick();assert(flashValid&&!cleared);limit=SIZE_MAX;
 corruptClose=true;tick();assert(flashValid&&!cleared);corruptClose=false;
 renameOK=false;tick();assert(flashValid&&!cleared);renameOK=true;
 clearOK=false;tick();assert(flashValid&&cleared==1&&files.size()==1);auto saved=files;
 reboot();clearOK=true;tick();assert(!flashValid&&cleared==2&&files==saved);
 // Export from SD works after flash acknowledgement.
 serial.clear();memfault_service::requestStatus(true);tick();assert(serial.find("source=SD")!=std::string::npos&&serial.find("complete=1")!=std::string::npos);
 // Conflicting final file is never overwritten or treated as durable.
 reset();mounted=true;std::string name="/logs/memfault-"+std::string(memfault_service::pendingId())+".log";files[name]="conflict";tick();assert(flashValid&&!cleared&&files[name]=="conflict");
 reset();readOK=false;mounted=true;tick();assert(flashValid&&!cleared&&files.empty());
 // A packetizer-time flash read error must not archive its 0xEF substitute.
 reset();mounted=true;memfault_service::pendingId();readOK=false;tick();assert(flashValid&&!cleared);
 reset();flashValid=false;tick();assert(files.empty()&&!cleared);
 // Phone download must preserve the full 45-byte Memfault filename.
 mounted=true;const char* filename="memfault-e5cdc8033ebd9bfcd91ba1b95b35c269.log";
 sendLogFile(filename);assert(downloadedPath==std::string("/logs/")+filename);
 downloadedPath.clear();recording=true;sendLogFile(filename);assert(downloadedPath.empty()&&notification==0x1f);recording=false;
 puts("Memfault SDK packetizer/export, SD fault/retry/readback, reboot, USB, non-destructive serial and acknowledgement tests passed");
}
