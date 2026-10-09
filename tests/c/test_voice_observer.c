/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "voice_observer.h"
#include "xinput_record.h"
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (0)
#include <string.h>
static uint32_t tables[8], values[4], stack[9];
static bool read_memory(uint32_t address, void *out, size_t bytes)
{
    if (bytes != 4u) return false;
    if (address >= 0x7E7200u && address < 0x7E7220u) memcpy(out, &tables[(address-0x7E7200u)/4u], bytes);
    else if (address >= 0x1000u && address < 0x1010u) memcpy(out, &values[(address-0x1000u)/4u], bytes);
    else if (address >= 0x2000u && address < 0x2024u) memcpy(out, &stack[(address-0x2000u)/4u], bytes);
    else return false;
    return true;
}
int main(void)
{
    char canonical[128];
    CHECK(xinput_record_canonical_line("--gpu-live --voice-log private.log",canonical,sizeof canonical));
    CHECK(strcmp(canonical,"--gpu-live")==0);
    tables[0]=0x1000u; tables[1]=2u; tables[2]=10u;
    tables[4]=0x1008u; tables[5]=2u; tables[6]=10u;
    values[0]=55u; values[1]=UINT32_MAX; values[2]=66u; values[3]=77u;
    CHECK(voice_observer_sample(0u,read_memory)==-1);
    CHECK(voice_observer_sample(9u,read_memory)==-1);
    CHECK(voice_observer_sample(10u,read_memory)==55);
    CHECK(voice_observer_sample(11u,read_memory)==77);
    CHECK(voice_observer_sample(12u,read_memory)==-1);
    tables[0]=UINT32_MAX-1u;
    CHECK(voice_observer_sample(11u,read_memory)==77);
    tables[0]=0x1000u;
    FILE *log=tmpfile(); CHECK(log);
    stack[0]=0x164C67u; stack[2]=10u; stack[4]=11u; stack[8]=0x12345678u;
    voice_observer_note(log,0x123u,0x2000u,3u,4u,100u,1000u,480u,read_memory);
    CHECK(ftell(log)==0);
    voice_observer_note(log,0x164C40u,0x2000u,3u,4u,100u,1000u,480u,read_memory);
    voice_observer_note(log,0x148A90u,0x2000u,3u,4u,100u,1000u,480u,read_memory);
    voice_observer_note(log,0x461C0u,UINT32_MAX,3u,4u,100u,1000u,480u,read_memory);
    rewind(log); char text[8192]={0}; CHECK(fread(text,1,sizeof text-1u,log)>0);
    CHECK(strstr(text,"audio_frames=480")); CHECK(strstr(text,"label=10 sample_lookup=55"));
    CHECK(strstr(text,"handle=12345678")); CHECK(strstr(text,"valid=000"));
    CHECK(strstr(text,"return=UNAVAILABLE"));
    FILE *dump=tmpfile(); CHECK(dump);
    uint32_t previous[12]={0};
    voice_observer_dump(dump,previous,read_memory);
    const long first=ftell(dump); CHECK(first>0);
    voice_observer_dump(dump,previous,read_memory); CHECK(ftell(dump)==first);
    tables[1]=UINT32_MAX;
    voice_observer_dump(dump,previous,read_memory); CHECK(ftell(dump)>first);
    rewind(dump); char receipt[8192]={0}; CHECK(fread(receipt,1,sizeof receipt-1u,dump)>0);
    CHECK(strstr(receipt,"refused")); CHECK(strstr(receipt,"complete=0"));
    fclose(dump);
    fclose(log); return 0;
}
