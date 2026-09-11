#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Audit raw input independently of TinyGPS: a lost whole sentence need not
// increment its checksum-error count. Track complete GGA/RMC epochs as well.
class GpsSentenceAudit {
public:
    struct Counts { uint32_t gga=0, rmc=0, bad=0, truncated=0, missingGga=0, missingRmc=0; } counts;
    void feed(char c) {
        if (c == '$') {
            if (collecting && used) ++counts.truncated;
            collecting=true; used=0; return;
        }
        if (!collecting) return;
        if (c == '\r') return;
        if (c == '\n') { finish(); collecting=false; return; }
        if (used + 1 >= sizeof(line)) { ++counts.truncated; collecting=false; return; }
        line[used++]=c;
    }
private:
    char line[160] = {};
    size_t used=0;
    bool collecting=false;
    int previousGga=-1, previousRmc=-1;
    static int hex(char c) {
        if(c>='0' && c<='9') return c-'0';
        if(c>='A' && c<='F') return c-'A'+10;
        if(c>='a' && c<='f') return c-'a'+10;
        return -1;
    }
    void epoch(int& previous, uint32_t& missing) {
        if(used<13) return;
        for(int i=6;i<12;++i) if(line[i]<'0'||line[i]>'9') return;
        int h=(line[6]-'0')*10+line[7]-'0';
        int m=(line[8]-'0')*10+line[9]-'0';
        int s=(line[10]-'0')*10+line[11]-'0';
        if(h>23||m>59||s>59) return;
        int now=h*3600+m*60+s;
        if(previous>=0) {
            int delta=(now-previous+86400)%86400;
            // Large time corrections/reset and duplicates are not RX loss.
            if(delta>1 && delta<=60) missing+=delta-1;
        }
        previous=now;
    }
    void finish() {
        if(used<4 || line[used-3]!='*') { ++counts.bad; return; }
        int hi=hex(line[used-2]), lo=hex(line[used-1]);
        uint8_t ck=0;
        for(size_t i=0;i<used-3;++i) ck^=(uint8_t)line[i];
        if(hi<0||lo<0||ck!=(hi*16+lo)) { ++counts.bad; return; }
        if(used>12 && line[5]==',') {
            if(!memcmp(line+2,"GGA",3)) { ++counts.gga; epoch(previousGga,counts.missingGga); }
            if(!memcmp(line+2,"RMC",3)) { ++counts.rmc; epoch(previousRmc,counts.missingRmc); }
        }
    }
};
