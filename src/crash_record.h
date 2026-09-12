#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace crash_record {
inline uint32_t crc(const void* data, size_t size) {
    auto* p=static_cast<const uint8_t*>(data);uint32_t value=~0u;
    while(size--){value^=*p++;for(int i=0;i<8;++i)value=(value>>1)^(0xedb88320u&(0u-(value&1)));}
    return ~value;
}
constexpr size_t TEXT_SIZE=4096, TAIL_COUNT=10, LINE_SIZE=192;
struct Record {
    uint32_t magic=0, id=0, length=0, checksum=0;
    char text[TEXT_SIZE]{};
    bool valid() const {return magic==0x43525331 && id && length>0 && length<TEXT_SIZE && text[length]==0 && checksum==(crc(&id,sizeof(id)+sizeof(length))^crc(text,length));}
    void seal(){checksum=crc(&id,sizeof(id)+sizeof(length))^crc(text,length);magic=0x43525331;}
};
struct Line {
    uint32_t sequence, uptime, length, checksum;
    char text[LINE_SIZE];
    bool valid() const {return sequence && length>0 && length<LINE_SIZE && checksum==((crc(&uptime,sizeof(uptime)+sizeof(length))^crc(text,LINE_SIZE))^sequence);}
    void write(uint32_t seq,uint32_t ms,const char* p,size_t n) {
        sequence=0;uptime=ms;length=n<LINE_SIZE?n:LINE_SIZE-1;
        memset(text,0,sizeof(text));memcpy(text,p,length);
        checksum=(crc(&uptime,sizeof(uptime)+sizeof(length))^crc(text,LINE_SIZE))^seq;sequence=seq;
    }
};
}
