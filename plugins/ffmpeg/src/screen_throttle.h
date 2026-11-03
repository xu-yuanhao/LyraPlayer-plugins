#pragma once
#include <cstring>
// config_text returns normalized TOML scalar assignments. Missing, malformed or
// unsupported values must never opt an older host into stream reconstruction.
inline bool screen_throttle_enabled(const char *text) {
    if (!text) return false;
    while (*text) {
        const char *end=std::strchr(text,'\n');
        if (!end) end=text+std::strlen(text);
        const char *start=text;
        while(start<end && (*start==' ' || *start=='\t'))++start;
        const char key[]="screen_off_throttle";
        if(end-start>=int(sizeof(key)-1) && !std::memcmp(start,key,sizeof(key)-1)) {
            start+=sizeof(key)-1;
            while(start<end && (*start==' ' || *start=='\t'))++start;
            if(start<end && *start++=='=') {
                while(start<end && (*start==' ' || *start=='\t'))++start;
                while(end>start && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r'))--end;
                return end-start==4 && !std::memcmp(start,"true",4);
            }
        }
        text=*end ? end+1 : end;
    }
    return false;
}
