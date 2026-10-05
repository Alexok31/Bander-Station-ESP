#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
static constexpr char kNs[]="bende";
static bool saved=false,canOpen=true,canWrite=true;
static unsigned writes=0,closed=0;
struct Preferences {
    bool begin(const char* name,bool){assert(!strcmp(name,kNs));return canOpen;}
    bool getBool(const char* key,bool fallback){assert(!strcmp(key,"wake_voice"));return saved;}
    size_t putBool(const char* key,bool value){
        assert(!strcmp(key,"wake_voice"));++writes;
        if(!canWrite)return 0;saved=value;return 1;
    }
    void end(){++closed;}
};
#include "../artifacts/wake-runtime/wake_preference_under_test.h"
int main(){
    assert(!nvsLoadWakeVoiceEnabled());
    assert(nvsSaveWakeVoiceEnabled(true));assert(nvsLoadWakeVoiceEnabled());
    assert(writes==1);assert(nvsSaveWakeVoiceEnabled(true));assert(writes==1);
    assert(nvsSaveWakeVoiceEnabled(false));assert(!nvsLoadWakeVoiceEnabled());
    canWrite=false;assert(!nvsSaveWakeVoiceEnabled(true));assert(!nvsLoadWakeVoiceEnabled());
    canOpen=false;assert(!nvsSaveWakeVoiceEnabled(true));assert(!nvsLoadWakeVoiceEnabled());
    canOpen=true;canWrite=true;assert(nvsSaveWakeVoiceEnabled(true));
    assert(nvsLoadWakeVoiceEnabled());assert(closed>0);
    puts("Wake preference: restore on/off, unchanged writes, storage failures passed");
}
