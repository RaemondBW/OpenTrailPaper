#include "crash_report.h"
#include "memfault_service.h"
#include "crash_record.h"
#include "diag.h"
#include "sd_bus.h"
#include "ride_recorder.h"
#include "usb_storage.h"
#include "power_mgmt.h"
#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_core_dump.h>
#include <stdarg.h>
#include <time.h>
#include <atomic>

namespace {
constexpr int SLOTS=4;
// The NVS partition is only 0x5000 bytes and also holds settings, sensor
// pairings and NimBLE bonds. Four full-size reports (4 KiB each) would starve
// those, so the whole queue is capped in bytes as well as in slots.
constexpr size_t NVS_BYTE_CAP=6144;
// While anything is undelivered, retry quickly: in a boot loop the device may
// only live ~15 s. Back off once it is clear the card is not coming.
constexpr uint32_t FAST_RETRY_MS=3000, SLOW_RETRY_MS=30000;
constexpr unsigned FAST_RETRIES=20;
unsigned failedAttempts=0;
RTC_NOINIT_ATTR crash_record::Line tail[crash_record::TAIL_COUNT];
portMUX_TYPE tailMux=portMUX_INITIALIZER_UNLOCKED;
bool captureReady=false;
uint32_t sequence=0;
crash_record::Record current, scratch;
bool currentPending=false, currentDurable=false, coreAwaitingDurability=false;
uint32_t nextRetry=0;
std::atomic<int> statusRequested{0};

void add(const char* format,...) {
    if(current.length>=sizeof(current.text)-1)return;
    va_list args;va_start(args,format);
    size_t available=sizeof(current.text)-current.length;
    int n=vsnprintf(current.text+current.length,available,format,args);va_end(args);
    if(n>0)current.length+=size_t(n)<available?size_t(n):available-1;
}
void keyFor(int slot,char* key){snprintf(key,12,"report%d",slot);}
// Queue order of a slot; a missing key (older firmware) sorts as oldest.
void orderKeyFor(int slot,char* key){snprintf(key,12,"order%d",slot);}
size_t storedBytes(const crash_record::Record& r){return offsetof(crash_record::Record,text)+r.length+1;}
bool readSlot(Preferences& prefs,int slot,crash_record::Record& out) {
    char key[12];keyFor(slot,key);
    size_t n=prefs.getBytesLength(key);
    if(n<=offsetof(crash_record::Record,text) || n>sizeof(out))return false;
    out={};
    return prefs.getBytes(key,&out,n)==n && out.valid() && n==offsetof(crash_record::Record,text)+out.length+1;
}
void acknowledgeCore() {
    if(!coreAwaitingDurability)return;
#if !OT_MEMFAULT
    esp_err_t result=esp_core_dump_image_erase();
    diag::log("crash report: core summary durable; dump erase -> %s",esp_err_to_name(result));
    if(result==ESP_OK)coreAwaitingDurability=false;
#endif
}
bool persistCurrent() {
    if(!currentPending || currentDurable){if(currentDurable)acknowledgeCore();return true;}
    Preferences prefs;
    if(!prefs.begin("crashdiag",false))return false;
    int freeSlot=-1, oldest=-1, newest=-1, occupied=0;
    uint32_t oldestOrder=UINT32_MAX, newestOrder=0, nextOrder=1;
    size_t used=0, newestBytes=0;
    for(int i=0;i<SLOTS && !currentDurable;++i){
        char key[12];keyFor(i,key);
        size_t n=prefs.getBytesLength(key);
        if(n==0){if(freeSlot<0)freeSlot=i;continue;}
        // A previous interrupted acknowledgement may already contain this ID.
        if(readSlot(prefs,i,scratch) && !memcmp(&scratch,&current,sizeof(current))){currentDurable=true;break;}
        char okey[12];orderKeyFor(i,okey);
        uint32_t order=prefs.getUInt(okey,0);
        ++occupied;used+=n;
        if(order>=nextOrder)nextOrder=order+1;
        if(oldest<0 || order<oldestOrder){oldest=i;oldestOrder=order;}
        if(newest<0 || order>=newestOrder){newest=i;newestOrder=order;newestBytes=n;}
    }
    size_t n=storedBytes(current);
    int slot=-1;
    if(!currentDurable){
        if(freeSlot>=0 && used+n<=NVS_BYTE_CAP)slot=freeSlot;
        // Full (slots or bytes): keep the OLDEST report, which in a crash loop
        // usually describes the root cause, and replace the newest with this one.
        else if(occupied>=2 && newest!=oldest && used-newestBytes+n<=NVS_BYTE_CAP){
            // Overwritten in place (not removed first): if the write fails the
            // newest report survives and this one stays in RAM.
            slot=newest;
            if(readSlot(prefs,slot,scratch))
                diag::log("crash report: NVS queue full (%d slots, %u bytes); replacing newest queued report %08lx with %08lx, oldest kept",
                    occupied,unsigned(used),(unsigned long)scratch.id,(unsigned long)current.id);
            else diag::log("crash report: NVS queue full; replacing unreadable newest slot %d with %08lx, oldest kept",slot,(unsigned long)current.id);
        }
    }
    if(slot>=0){
        char key[12];keyFor(slot,key);
        char okey[12];orderKeyFor(slot,okey);
        // Order first: an orphan order key without a report is harmless.
        prefs.putUInt(okey,nextOrder);
        currentDurable=prefs.putBytes(key,&current,n)==n &&
            readSlot(prefs,slot,scratch) && memcmp(&current,&scratch,sizeof(current))==0;
    } else if(!currentDurable){
        diag::log("crash report: NVS queue full (%d slots, %u/%u bytes); %08lx not queued",
            occupied,unsigned(used),unsigned(NVS_BYTE_CAP),(unsigned long)current.id);
    }
    prefs.end();
    if(currentDurable)acknowledgeCore();
    return currentDurable;
}
// SD lock is held; no serial printing or NVS access under this lock.
bool fileMatches(const char* path,const crash_record::Record& record) {
    File f=SD.open(path,FILE_READ);
    if(!f)return false;
    bool ok=f.size()==record.length;size_t offset=0;uint8_t bytes[128];
    while(ok && offset<record.length){
        size_t want=record.length-offset;if(want>sizeof(bytes))want=sizeof(bytes);
        size_t n=f.read(bytes,want);
        ok=n==want && !memcmp(bytes,record.text+offset,n);offset+=n;
    }
    f.close();return ok;
}
bool saveSD(const crash_record::Record& record) {
    char path[64];snprintf(path,sizeof(path),"/logs/crash-%08lx.log",(unsigned long)record.id);
    char temporary[64];snprintf(temporary,sizeof(temporary),"/logs/crash-%08lx.tmp",(unsigned long)record.id);
    bool ok=false;
    sdLock();
    if(ride_recorder::sdMounted() && !usb_storage::hostActive()){
        if(SD.exists(path))ok=fileMatches(path,record); // interrupted NVS acknowledgement: idempotent
        else {
            SD.mkdir("/logs");
            // Retry a partial temporary file without replacing a completed report.
            File f=SD.open(temporary,FILE_WRITE);
            if(f){ok=f.write(reinterpret_cast<const uint8_t*>(record.text),record.length)==record.length;f.flush();f.close();}
            ok=ok && fileMatches(temporary,record) && SD.rename(temporary,path) && fileMatches(path,record);
        }
    }
    sdUnlock();
    if(ok)diag::log("crash report saved and verified: %s (%lu bytes)",path,(unsigned long)record.length);
    else diag::log("crash report: SD write/readback failed for %s; retained for retry",path);
    return ok;
}
}

void crash_report::recordLine(const char* line,size_t length) {
    if(!captureReady || !length)return;
    portENTER_CRITICAL(&tailMux);
    ++sequence;if(!sequence)++sequence;
    tail[(sequence-1)%crash_record::TAIL_COUNT].write(sequence,millis(),line,length);
    portEXIT_CRITICAL(&tailMux);
}
void crash_report::begin(int reason,const char* reasonName,const char* firmware) {
    bool crash=reason==ESP_RST_PANIC || reason==ESP_RST_INT_WDT || reason==ESP_RST_TASK_WDT ||
               reason==ESP_RST_WDT || reason==ESP_RST_BROWNOUT;
    if(crash){
        current={};current.id=esp_random();if(!current.id)current.id=1;
        add("OpenTrailPaper crash report\nid=%08lx reset=%s [%d]\nboot_firmware=%s build=%s %s\n",
            (unsigned long)current.id,reasonName,reason,firmware,__DATE__,__TIME__);
        add("boot_unix_utc=%lld (may precede RTC/GPS clock restoration)\n",(long long)time(nullptr));
        const auto* app=esp_ota_get_app_description();
        add("boot_image_elf_sha256=");for(auto b:app->app_elf_sha256)add("%02x",b);add("\n");
#if OT_MEMFAULT
        size_t memfaultSize=0;
        bool memfaultPending=memfault_service::pending(&memfaultSize);
        add("crash_backend=Memfault pending=%d bytes=%u id=%s\n",memfaultPending,
            unsigned(memfaultSize),memfault_service::pendingId());
        add("Memfault chunks contain registers, stacks, and logs; retained in flash until SD export verifies.\n");
        add("A previously pending dump is preserved if another crash occurs before export; it may describe an earlier crash.\n");
#elif CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
        auto* summary=static_cast<esp_core_dump_summary_t*>(calloc(1,sizeof(esp_core_dump_summary_t)));
        esp_err_t result=summary?esp_core_dump_get_summary(summary):ESP_ERR_NO_MEM;
        add("core_summary=%s (%d)\n",esp_err_to_name(result),int(result));
        if(result==ESP_OK){
            coreAwaitingDurability=true;
            add("task=%.16s PC=0x%08lx crashed_elf_sha256=%.*s\nbacktrace%s:",summary->exc_task,
                (unsigned long)summary->exc_pc,int(sizeof(summary->app_elf_sha256)),summary->app_elf_sha256,
                summary->exc_bt_info.corrupted?" (corrupt)":"");
            size_t maxDepth=sizeof(summary->exc_bt_info.bt)/sizeof(summary->exc_bt_info.bt[0]);
            for(size_t i=0;i<summary->exc_bt_info.depth && i<maxDepth;++i)add(" 0x%08lx",(unsigned long)summary->exc_bt_info.bt[i]);
            add("\n");
#if CONFIG_IDF_TARGET_ESP32S3
            add("exception_cause=%lu exception_address=0x%08lx SP=0x%08lx\n",
                (unsigned long)summary->ex_info.exc_cause,(unsigned long)summary->ex_info.exc_vaddr,
                (unsigned long)summary->ex_info.exc_a[1]);
            for(size_t i=0;i<sizeof(summary->ex_info.epcx)/sizeof(summary->ex_info.epcx[0]);++i)
                if(summary->ex_info.epcx_reg_bits&(1u<<i))add("EPC%u=0x%08lx ",unsigned(i+1),(unsigned long)summary->ex_info.epcx[i]);
            add("\n");
#endif
        } else add("No decodable core dump; reset cause and breadcrumbs do not identify the faulting function.\n");
        free(summary);
#else
        add("core_summary=not enabled in this framework\n");
#endif
        add("Prior boot RTC log tail (uptime in ms; lines may be truncated):\n");
        // This is before application tasks start. Validate each slot independently
        // so a watchdog during a write loses one entry, not the entire tail.
        uint32_t last=0;
        for(const auto& line:tail)if(line.valid() && line.sequence>last)last=line.sequence;
        for(uint32_t age=crash_record::TAIL_COUNT;age>0;--age){
            uint32_t wanted=last>=age?last-age+1:0;
            if(!wanted)continue;
            for(const auto& line:tail)if(line.valid() && line.sequence==wanted)
                add("+%lums %.*s%s",(unsigned long)line.uptime,int(line.length),line.text,
                    line.text[line.length-1]=='\n'?"":"\n");
        }
        if(!last)add("unavailable (first boot with this feature, lost RTC power, or corrupt entries)\n");
        current.seal();currentPending=true;
        bool durable=persistCurrent();
        diag::log("CRASH report %08lx: reset=%s; durable_NVS=%d; SD delivery pending",
            (unsigned long)current.id,reasonName,durable);
        if(!durable)diag::log("crash report: NVS unavailable/full; current report remains in RAM; existing reports retained");
    }
    memset(tail,0,sizeof(tail));sequence=0;captureReady=true;
}
void crash_report::requestStatus(){statusRequested=1;}
void crash_report::requestTestPanic(){statusRequested=2;}
namespace {
// Main task only. Returns true when nothing remains to deliver.
bool deliver(bool status,const char* why) {
    power_mgmt::busyAcquire();
    persistCurrent();
    Preferences prefs;int queued=0,delivered=0;
    bool sd=ride_recorder::sdMounted() && !usb_storage::hostActive();
    bool nvsAvailable=prefs.begin("crashdiag",false);
    if(nvsAvailable){
        for(int i=0;i<SLOTS;++i){
            char key[12];keyFor(i,key);
            if(!prefs.getBytesLength(key))continue;
            if(!readSlot(prefs,i,scratch)){
                // Not counted as pending: an unreadable slot would otherwise keep
                // the fast retry running forever.
                if(status)diag::log("crash report: invalid NVS slot %d retained for investigation",i);
                continue;
            }
            ++queued;
            if(status)Serial.print(scratch.text);
            if(sd && saveSD(scratch)){
                if(currentPending && scratch.id==current.id)currentPending=false;
                if(prefs.remove(key)){
                    --queued;++delivered;
                    char okey[12];orderKeyFor(i,okey);prefs.remove(okey);
                    diag::log("crash report %08lx delivered to SD; NVS slot %d cleared",(unsigned long)scratch.id,i);
                }
                else diag::log("crash report: SD verified; NVS acknowledgement failed, will retry");
            }
        }
        prefs.end();
    }
    if(currentPending && !currentDurable){
        if(status)Serial.print(current.text);
        if(sd && saveSD(current)){
            currentPending=false;acknowledgeCore();++delivered;
            diag::log("crash report %08lx (RAM-only) delivered to SD",(unsigned long)current.id);
        }
    }
    power_mgmt::busyRelease();
    bool remaining=queued>0 || (currentPending && !currentDurable);
    if(why && (delivered || remaining))
        diag::log("crash report: %s: delivered=%d still_queued_NVS=%d current_RAM=%d SD=%d host=%d",
            why,delivered,queued,currentPending && !currentDurable,ride_recorder::sdMounted(),usb_storage::hostActive());
    if(status && current.valid() && !currentPending)Serial.print(current.text);
    if(status)diag::log("crash report: NVS_available=%d queued_NVS=%d current_RAM=%d SD=%d (crash-*.log appears in phone log list)",
        nvsAvailable,queued,currentPending,ride_recorder::sdMounted());
    return !remaining;
}
void schedule(bool clear) {
    if(clear)failedAttempts=0;
    else if(failedAttempts<FAST_RETRIES)++failedAttempts;
    nextRetry=millis()+(clear || failedAttempts>=FAST_RETRIES?SLOW_RETRY_MS:FAST_RETRY_MS);
}
}
void crash_report::onSdMounted(const char* why) {
    // Runs on the main task right after a successful mount and BEFORE the card
    // can be exposed to a USB host, so a boot-looping device still gets its
    // reports and dump out within the few seconds it lives.
    failedAttempts=0;
    schedule(deliver(false,why));
    memfault_service::deliverNow(why);
}
void crash_report::tick() {
    int action=statusRequested.exchange(0);
    if(action==2){
        if(ride_recorder::isRecording()){diag::log("crash report test refused: a ride is recording");return;}
        diag::log("CRASH TEST: deliberate panic requested from serial; no ride is recording");
        abort();
    }
    bool status=action==1;
    if(!status && int32_t(millis()-nextRetry)<0)return;
    schedule(deliver(status,nullptr));
}
