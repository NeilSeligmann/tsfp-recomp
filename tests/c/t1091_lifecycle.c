/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test-only dependency provider. Synthetic lower results qualify outer control
 * flow, not a NIC. A separate PIE process avoids Python's low executable map. */
#include "xnet_hle.h"
#include "xnet_eeprom.h"
#include "kernel_crypto.h"
#include "kernel_call.h"
#include "kernel_sync.h"
#include "xnet_collector.h"
#include "xnet_collector_kernel.h"
#include "kernel_clock.h"
#include "kernel_config.h"
#include "kernel_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define OBJ 0x20000000u
#define SOCK 0x20001000u
#define OUT 0xa01000u
#define STK 0xa00000u
#define SINGLE 0x7715ecu
static unsigned allocation_failure, initialization_result;
static uint32_t allocate(uint32_t size, uint32_t tag)
{
    printf("allocation_irql %u\n",kernel_sync_current_irql());
    printf("event allocate %u %u\n", size, tag);
    return allocation_failure ? 0u : size == 0xd50u ? OBJ : SOCK;
}
static void release(uint32_t object)
{ printf("release_irql %u\n",kernel_sync_current_irql()); printf("event free %u\n", object); }
static uint32_t initialize(uint32_t object, uint32_t params)
{
    printf("event initialize %u %u\n", object, params);
    memcpy((void *)(uintptr_t)(object + 0xd48u), &(uint32_t){0x1234u}, 4);
    return initialization_result;
}
static void close_sockets(uint32_t object) { printf("event close %u\n", object); }
static void destroy(uint32_t object, bool final_cleanup)
{ printf("event destroy %u %u\n", object, (unsigned)final_cleanup); }
static bool error(uint32_t value) { printf("event error %u\n", value); return true; }
static void dump(const char *label, uint32_t address, size_t size)
{
    printf("%s ", label);
    const unsigned char *bytes = (const unsigned char *)(uintptr_t)address;
    for (size_t i=0; i<size; i++) printf("%02x", bytes[i]);
    putchar('\n');
}

typedef struct { bool volume_ok, nv_ok; } collector_fixture;
static bool collector_disk_ready(void *context)
{
    (void)context;
    return allocation_failure != 41u;
}
static bool collector_kernel(void *context, unsigned ordinal, const uint32_t *args,
                              size_t count, uint64_t *result)
{
    const collector_fixture *fixture=context;
    printf("event kernel %u %zu\n",ordinal,count);
    *result=0u;
    if (allocation_failure==ordinal) return false;
    if (ordinal==128u) memcpy((void *)(uintptr_t)args[0],"sysTIME!",8u);
    else if (ordinal==126u) *result=UINT64_C(0x0706050403020100);
    else if (ordinal==181u) {
        unsigned char *statistics=(unsigned char *)(uintptr_t)args[0];
        for (unsigned i=0u;i<36u;i++) statistics[i]=(unsigned char)i;
        *result=initialization_result;
    } else if (ordinal==24u) {
        if (fixture->nv_ok) {
            unsigned char *value=(unsigned char *)(uintptr_t)args[2];
            for (unsigned i=0u;i<256u;i++) value[i]=(unsigned char)i;
            memcpy((void *)(uintptr_t)args[4],&(uint32_t){256u},4u);
        }
        *result=fixture->nv_ok?0u:0xc0000001u;
    } else return false;
    return true;
}
static bool collector_volume(void *context, uint32_t path, const uint32_t output[3],
                              uint32_t *result)
{
    const collector_fixture *fixture=context;
    printf("event volume %u 3\n",path);
    memcpy((void *)(uintptr_t)output[0],"volFREE!",8u);
    memcpy((void *)(uintptr_t)output[2],"volSIZE!",8u);
    *result=(uint32_t)fixture->volume_ok;
    return true;
}
static void collector_imports(void)
{
    memcpy((void *)(uintptr_t)0x475868u,&(uint32_t){OUT+0x300u},4u);
    memcpy((void *)(uintptr_t)(OUT+0x300u),"tick",4u);
    const uint32_t slots[]={0x4759b8u,0x4759b4u};
    const char *names[]={"disk-model","disk-serial"};
    for (unsigned i=0u;i<2u;i++) {
        const uint32_t address=OUT+0x340u+i*0x40u, buffer=address+8u;
        const uint16_t length=(uint16_t)strlen(names[i]);
        memcpy((void *)(uintptr_t)slots[i],&address,4u);
        memcpy((void *)(uintptr_t)address,&length,2u);
        memcpy((void *)(uintptr_t)(address+4u),&buffer,4u);
        memcpy((void *)(uintptr_t)buffer,names[i],length);
    }
}

int main(int argc, char **argv)
{
    if (argc != 8 && argc != 9 && argc != 10) return 2;
    const uint32_t entry=(uint32_t)strtoul(argv[1], NULL, 0);
    const unsigned mode=(unsigned)strtoul(argv[2], NULL, 0);
    const uint32_t xref=(uint32_t)strtoul(argv[3], NULL, 0);
    const uint32_t wref=(uint32_t)strtoul(argv[4], NULL, 0);
    initialization_result=(unsigned)strtoul(argv[5], NULL, 0);
    allocation_failure=(unsigned)strtoul(argv[6], NULL, 0);
    const uint32_t version=(uint32_t)strtoul(argv[7], NULL, 0);
    const uint32_t bases[]={0x771000u, OBJ, STK, 0x475000u};
    const size_t lengths[]={0x1000u, 0x100000u, 0x4000u, 0x1000u};
    for (unsigned i=0;i<4;i++) {
        void *p=mmap((void *)(uintptr_t)bases[i],lengths[i],PROT_READ|PROT_WRITE,
                    MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        if (p != (void *)(uintptr_t)bases[i]) return 3;
    }
    const xnet_surface_entry rows[]={
        {0x431bb1u,"XNetStartup",1},{0x431bc8u,"WSAStartup",1},
        {0x431cb6u,"XNetCleanup",1},{0x431cc3u,"WSACleanup",1},
        {0x4317b1u,"socket",1},{0x4317d2u,"ioctlsocket",1},{0x431802u,"bind",1},
        {0x4317bcu,"closesocket",1},{0x43184cu,"recv",1},{0x431866u,"recvfrom",1}};
    if (!xnet_hle_init(rows,10)) return 4;
    if (xnet_hle_register_lifecycle()!=0) return 5;
    xnet_lifecycle_provider empty={0};
    if (xnet_hle_set_lifecycle_provider(&empty)) return 6;
    if (xnet_hle_prepare_object(0)) return 7;
    const xnet_lifecycle_provider provider={allocate,release,initialize,close_sockets,destroy,error};
    if (!xnet_hle_set_lifecycle_provider(&provider) || xnet_hle_register_lifecycle()!=4) return 8;
    if (xnet_hle_set_lifecycle_provider(NULL)) return 13;
    memset((void *)(uintptr_t)OUT,0xcc,0x200);
    if (mode==1) {
        memset((void *)(uintptr_t)OBJ,0xa5,0xd50);
        memcpy((void *)(uintptr_t)(OBJ+0xd30),&xref,2);
        memcpy((void *)(uintptr_t)(OBJ+0xd32),&wref,2);
        memcpy((void *)(uintptr_t)SINGLE,&(uint32_t){OBJ},4);
    }
    uint32_t args[6]={version,OUT,0u,0u,0u,0u};
    if (mode==2 || mode==3 || mode==4 || mode==6 || mode==7 || mode==5 || mode==6 || mode==7) {
        unsigned char *obj=(unsigned char *)(uintptr_t)OBJ;
        obj[0xa]=4;obj[0xb]=10;obj[0xc]=12;obj[0x36]=6;obj[0x38]=8;
        memcpy(obj+0xd32,&(uint16_t){1},2);
        memcpy(obj+0xd38,&(uint32_t){OBJ+0xd38},4);
        memcpy(obj+0xd3c,&(uint32_t){OBJ+0xd38},4);
        memcpy((void *)(uintptr_t)SINGLE,&(uint32_t){OBJ},4);
        args[0]=xref;args[1]=wref;args[2]=version;
        if (xnet_hle_register_local_sockets()!=6) return 12;
        if (mode==3 || mode==4 || mode==6 || mode==7) {
            unsigned char *sock=(unsigned char *)(uintptr_t)SOCK;
            memcpy(sock+8,&(uint32_t){allocation_failure==1?0x2a434f53u:
                                     allocation_failure==2?0xdeadu:0x2b434f53u},4);
            memcpy(sock+0xc,&(uint32_t){wref},4);
            memcpy(sock+0x24,&(uint32_t){SOCK+0x24},4);
            memcpy(sock+0x28,&(uint32_t){SOCK+0x24},4);
            memcpy((void *)(uintptr_t)OUT,&initialization_result,4);
            args[0]=xref==1?0u:xref==2?UINT32_MAX:SOCK;
            args[1]=version;args[2]=OUT;
            if (mode==6) {
                args[0]=SOCK;args[1]=OUT+0x100;args[2]=16;args[3]=0;
                args[4]=OUT;args[5]=OUT+0x80;
            }
            if (mode==4 || mode==7) {
                memcpy(obj+0xd36,&(uint16_t){version>>16?(uint16_t)(version>>16):0x400},2);
                memcpy(sock,&(uint32_t){OBJ+0xd38},4);
                memcpy(sock+4,&(uint32_t){OBJ+0xd38},4);
                memcpy(obj+0xd38,&(uint32_t){SOCK},4);
                memcpy(obj+0xd3c,&(uint32_t){SOCK},4);
                memcpy(obj+0xd34,&(uint16_t){1},2);
                memcpy((void *)(uintptr_t)(OUT+2),&(uint16_t){(uint16_t)version},2);
                memcpy((void *)(uintptr_t)(OUT+4),&initialization_result,4);
                args[1]=OUT;args[2]=0u;
                if (mode==7) {
                    const uint32_t other=SOCK+0x100;
                    const uint32_t head=OBJ+0xd38;
                    memcpy(obj+0xd34,&(uint16_t){2},2);
                    memcpy(obj+0xd3c,&other,4);
                    memcpy(sock,&other,4);
                    memcpy((void *)(uintptr_t)other,&head,4);
                    memcpy((void *)(uintptr_t)(other+4),&(uint32_t){SOCK},4);
                    memcpy((void *)(uintptr_t)(other+0xc),&xref,4);
                    const uint16_t otherport=(uint16_t)version? (uint16_t)version:0x0004;
                    memcpy((void *)(uintptr_t)(other+0x4a),&otherport,2);
                    args[0]=SOCK;
                }
            }
        }
        if (mode==5) {
            const uint32_t create_args[]={2u,wref,version};
            kernel_call_frame create_frame;
            if (!kernel_frame_build(&create_frame,STK,0x100,create_args,3)) return 14;
            if (xnet_hle_call(0x4317b1u,&create_frame)!=SOCK) return 15;
            args[0]=SOCK;
        }
    }
    unsigned count=entry==0x4317bcu?1u:entry==0x43184cu?4u:entry==0x431866u?6u:(entry==0x4317b1u || entry==0x4317d2u || entry==0x431802u)?3u:entry==0x431bb1u?1u:entry==0x431bc8u?2u:0u;
    kernel_call_frame frame;
    if (!kernel_frame_build(&frame,STK,0x100,args,count)) return 9;
    if (mode==12) {
        memset((void *)(uintptr_t)OUT,0xee,600u);
        memset((void *)(uintptr_t)(OUT+0x800u),0xa5,0x200u);
        collector_imports();
        collector_fixture fixture={xref!=0u,wref!=0u};
        const xnet_collector_source source={collector_kernel,collector_volume,&fixture,collector_disk_ready};
        uint32_t written=0xeeeeeeeeu;
        const bool okay=xnet_collect_entropy(OUT,version,OUT+0x800u,&source,&written);
        printf("result %u\n",(unsigned)okay);
        printf("written %u\n",written);
        dump("output",OUT,600u);
        return 0;
    }
    if (mode==22) {
        if (argc<9) return 36;
        kernel_hle_init(); kernel_config_reset(); (void)kernel_config_register();
        const xnet_eeprom_result loaded=xnet_eeprom_load_keyed(argv[8],argc==10?argv[9]:NULL);
        unsigned char key[16],digest[20]; memset(key,0xa5,sizeof(key));
        const bool available=kernel_config_eeprom_key(version,key);
        printf("result %u\navailable %u\nsettings %u\nkeys %u\n",(unsigned)loaded,
               (unsigned)kernel_config_eeprom_available(),kernel_config_setting_count(),(unsigned)available);
        kernel_crypto_sha1(key,sizeof(key),digest);
        memcpy((void *)(uintptr_t)OUT,digest,sizeof(digest)); dump("digest",OUT,20u);
        return 0;
    }
    if (mode==21) {
        if (argc!=9) return 31;
        kernel_hle_init(); kernel_config_reset(); (void)kernel_config_register();
        unsigned char report[5436],eeprom[256],key[16],output[16],digest[20];
        FILE *source=fopen(argv[8],"rb");
        if (!source || fread(report,1u,sizeof(report),source)!=sizeof(report) ||
            fclose(source)!=0) return 32;
        memcpy(eeprom,report+2136u+660u+340u+16u,256u);
        memcpy(key,report+2120u,16u);
        if (wref==1u && !kernel_config_set_eeprom_keyed(eeprom,256u,key,16u)) return 33;
        if (xref==1u) key[0]^=1u;
        if (xref==2u) eeprom[0]^=1u;
        if (xref==3u) eeprom[28u]^=1u;
        if (xref==4u) eeprom[0x40u]^=1u;
        const bool okay=kernel_config_set_eeprom_keyed(eeprom,256u,key,xref==5u?15u:16u);
        if (wref==2u && !kernel_config_set_eeprom(eeprom,256u)) return 34;
        if (wref==3u) kernel_config_reset();
        if (wref==4u && !kernel_config_set_setting(0xffffu,eeprom,1u)) return 35;
        memset(output,0xa5,sizeof(output));
        const bool available=kernel_config_eeprom_key(version,output);
        printf("result %u\navailable %u\nsettings %u\nkeys %u\n",(unsigned)okay,
               (unsigned)kernel_config_eeprom_available(),kernel_config_setting_count(),(unsigned)available);
        kernel_crypto_sha1(output,sizeof(output),digest);
        memcpy((void *)(uintptr_t)OUT,digest,sizeof(digest));
        dump("digest",OUT,20u);
        return 0;
    }
    if (mode==20) {
        if (argc!=9) return 29;
        unsigned char report[5436];
        FILE *source=fopen(argv[8],"rb");
        if (!source || fread(report,1u,sizeof(report),source)!=sizeof(report) ||
            fclose(source)!=0) return 30;
        unsigned char eeprom[256],key[16],output[16],digest[20];
        memcpy(eeprom,report+2136u+660u+340u+16u,256u);
        memcpy(key,report+2120u,16u);
        memset(output,0xa5,sizeof(output));
        if (xref==1u) key[0]^=1u;
        if (xref==2u) eeprom[0]^=1u;
        if (xref==3u) eeprom[20u]^=1u;
        if (xref==4u) eeprom[28u]^=1u;
        if (xref==5u) eeprom[47u]^=1u;
        const bool okay=xnet_eeprom_derive_hd_key(eeprom,key,output);
        printf("result %u\n",(unsigned)okay);
        kernel_crypto_sha1(output,sizeof(output),digest);
        memcpy((void *)(uintptr_t)OUT,digest,sizeof(digest));
        dump("digest",OUT,20u);
        return 0;
    }
    if (mode==19) {
        if (argc!=9) return 25;
        kernel_hle_init(); kernel_config_reset(); (void)kernel_config_register();
        if (wref==1u) {
            unsigned char previous[256];
            for (unsigned i=0u;i<256u;++i) previous[i]=(unsigned char)(i*3u+9u);
            if (!kernel_config_set_eeprom(previous,sizeof(previous))) return 27;
        }
        if (wref==2u || wref==3u || wref==4u) {
            const uint32_t setting=0x12345678u;
            const unsigned count=wref==2u?32u:wref==3u?31u:30u;
            for (unsigned i=0u;i<count;++i)
                if (!kernel_config_set_setting(0x1000u+i,&setting,4u)) return 28;
        }
        const xnet_eeprom_result loaded=xnet_eeprom_load(argv[8]);
        printf("result %u\n",(unsigned)loaded);
        printf("available %u\n",(unsigned)kernel_config_eeprom_available());
        printf("settings %u\n",kernel_config_setting_count());
        memset((void *)(uintptr_t)OUT,0xa5,320u);
        const uint32_t query[]={xref,OUT+0x180u,OUT+16u,version,OUT+0x184u};
        (void)kernel_guest_write_u32(OUT+0x180u,0xeeeeeeeeu);
        (void)kernel_guest_write_u32(OUT+0x184u,0xeeeeeeeeu);
        uint32_t value=0xeeeeeeeeu;
        if (kernel_config_eeprom_available()) {
            kernel_call_frame call;
            if (!kernel_frame_build(&call,STK,0x100u,query,5u)) return 26;
            value=kernel_config_query_stored(&call);
        }
        printf("value %u\n",value);
        uint32_t kind,length;
        (void)kernel_guest_read_u32(OUT+0x180u,&kind);
        (void)kernel_guest_read_u32(OUT+0x184u,&length);
        printf("type %u\nlength %u\nfabricated %u\n",kind,length,kernel_config_fabricated_count());
        dump("output",OUT,320u);
        return 0;
    }
    if (mode==18) {
        kernel_hle_init(); kernel_config_reset(); (void)kernel_config_register();
        unsigned char data[256];
        for (unsigned i=0u;i<256u;++i) data[i]=(unsigned char)(i*7u+3u);
        memset(data+0x30u,0,4u);
        uint64_t sum=0u;
        for (unsigned i=0x30u;i<0x60u;i+=4u) {
            uint32_t word; memcpy(&word,data+i,4u); sum+=word;
        }
        sum=(sum&UINT32_MAX)+(sum>>32); sum=(sum&UINT32_MAX)+(sum>>32);
        const uint32_t checksum=~(uint32_t)sum;
        memcpy(data+0x30u,&checksum,4u);
        if (wref) data[0x40u]^=1u;
        if (!kernel_config_set_eeprom(data,sizeof(data))) return 22;
        memset((void *)(uintptr_t)OUT,0xa5,320u);
        const uint32_t query[]={xref,OUT+0x180u,OUT+16u,version,OUT+0x184u};
        if (!kernel_guest_write_u32(OUT+0x180u,0xeeeeeeeeu) ||
            !kernel_guest_write_u32(OUT+0x184u,0xeeeeeeeeu)) return 23;
        kernel_call_frame call;
        if (!kernel_frame_build(&call,STK,0x100u,query,5u)) return 24;
        printf("result %u\n",kernel_config_query_stored(&call));
        uint32_t kind,length;
        (void)kernel_guest_read_u32(OUT+0x180u,&kind);
        (void)kernel_guest_read_u32(OUT+0x184u,&length);
        printf("type %u\nlength %u\nfabricated %u\n",kind,length,kernel_config_fabricated_count());
        dump("output",OUT,320u);
        return 0;
    }
    if (mode==17) {
        const uint32_t allocation=OBJ+0x2000u;
        const uint32_t bytes=(version+2u)*2048u;
        if (version>255u || xref>255u) return 21;
        ((unsigned char *)(uintptr_t)OBJ)[7]=(unsigned char)version;
        ((unsigned char *)(uintptr_t)OBJ)[0x10]=(unsigned char)xref;
        memset((void *)(uintptr_t)allocation,0xa5,bytes);
        const bool okay=xnet_hle_prepare_nic_dma(OBJ,allocation,bytes,0x81000000u);
        printf("result %u\n",(unsigned)okay);
        dump("object",OBJ,0xd50u);
        dump("output",allocation,4096u);
        unsigned char digest[20];
        kernel_crypto_sha1((void *)(uintptr_t)allocation,bytes,digest);
        memcpy((void *)(uintptr_t)OUT,digest,20u);
        dump("digest",OUT,20u);
        return 0;
    }
    if (mode==23 || mode==24) {
        unsigned char *payload=(unsigned char *)(uintptr_t)OUT;
        unsigned char *key=(unsigned char *)(uintptr_t)(OUT+0x800u);
        for (unsigned i=0u;i<492u;++i) payload[i]=(unsigned char)(i*17u+3u);
        for (unsigned i=0u;i<16u;++i) key[i]=(unsigned char)(i*7u+5u);
        const unsigned char domain[4]={0x32u,0x56u,0x42u,0x58u};
        kernel_crypto_xc_hmac(key,16u,payload+0x3cu,432u,domain,4u,payload+0x28u);
        if (xref==1u) payload[0x28u]^=1u;
        if (xref==2u) payload[0x3cu]^=1u;
        if (xref==3u) key[3]^=1u;
        bool needs=false;
        bool okay;
        if (mode==23) okay=xnet_hle_authenticate_config_payload(OUT,OUT+0x800u,&needs);
        else okay=xnet_hle_encode_config_sector(OUT,OUT+0x800u,OUT+0x1000u);
        printf("result %u\n",(unsigned)okay);
        printf("needs %u\n",(unsigned)needs);
        dump("output",OUT,492u);
        if (mode==24) dump("sector",OUT+0x1000u,512u);
        return 0;
    }
    if (mode==16) {
        unsigned char *sector=(unsigned char *)(uintptr_t)(OUT+0x1000u);
        memset((void *)(uintptr_t)OUT,0xa5,512u);
        uint64_t sum=0u;
        for (unsigned i=0u;i<128u;++i) {
            uint32_t word=i==0u?0x79132568u:i==1u?1u:i==2u?2u:
                          i==126u?0u:i==127u?0xaa550000u:0xfffffff0u+i;
            memcpy(sector+4u*i,&word,4u); sum+=word;
        }
        sum=(sum&UINT32_MAX)+(sum>>32);
        sum=(sum&UINT32_MAX)+(sum>>32);
        const uint32_t checksum=~(uint32_t)sum;
        memcpy(sector+504u,&checksum,4u);
        if (xref==1u) sector[0]^=1u;
        if (xref==2u) sector[508u]^=1u;
        if (xref==3u) memset(sector+4u,0,4u);
        if (xref==4u) memset(sector+8u,0,4u);
        if (xref==5u) sector[504u]^=1u;
        if (xref==6u) sector[200u]^=1u;
        const bool okay=xnet_hle_decode_config_sector(OUT+0x1000u,OUT);
        printf("result %u\n",(unsigned)okay);
        dump("output",OUT,512u);
        dump("sector",OUT+0x1000u,512u);
        return 0;
    }
    if (mode==15) {
        unsigned char *parameters=(unsigned char *)(uintptr_t)OUT;
        for (unsigned i=0u;i<36u;++i) parameters[i]=(unsigned char)(i*3u+1u);
        for (unsigned i=0u;i<512u;++i)
            ((unsigned char *)(uintptr_t)(OUT+0x1000u))[i]=(unsigned char)(i*7u+2u);
        memset((void *)(uintptr_t)(OUT+0x800u),0xa5,512u);
        memset((void *)(uintptr_t)(OUT+0xa00u),0xee,20u);
        collector_imports();
        collector_fixture fixture={xref!=0u,wref!=0u};
        const xnet_collector_source source={collector_kernel,collector_volume,&fixture,collector_disk_ready};
        const bool okay=xnet_hle_collect_seed_state(OBJ,OUT,OUT+0x1000u,OUT+0x800u,
                                                    OUT+0xa00u,&source);
        printf("result %u\n",(unsigned)okay);
        dump("object",OBJ,0xd50u);
        dump("output",OUT+0x1000u,512u);
        dump("seed",OUT+0x10u,20u);
        dump("digest",OUT+0xa00u,20u);
        return 0;
    }
    if (mode==13) {
        kernel_hle_init(); kernel_clock_reset(); kernel_event_reset(); kernel_config_reset();
        (void)kernel_clock_register(); (void)kernel_event_register(); (void)kernel_config_register();
        collector_fixture fixture={false,false};
        xnet_collector_kernel backend={OUT+0xa00u,32u,collector_volume,&fixture,collector_disk_ready,&fixture};
        xnet_collector_source source;
        if (!xnet_collector_kernel_source(&backend,&source)) return 18;
        const uint32_t query[]={0xffffu,OUT+0x200u,OUT,256u,OUT+0x204u};
        memset((void *)(uintptr_t)OUT,0xee,0x208u);
        uint64_t result=UINT64_C(0xeeeeeeeeeeeeeeee);
        if (xref) {
            unsigned char provided[256];
            for (unsigned i=0u;i<256u;++i) provided[i]=(unsigned char)i;
            if (!kernel_config_set_eeprom(provided,sizeof(provided))) return 19;
        }
        if (allocation_failure) (void)kernel_clock_advance_to(UINT64_C(0x100000001));
        const uint32_t time_query[]={OUT};
        const bool okay=source.kernel_call(source.context,version,version==128u?time_query:query,version==24u?5u:version==128u?1u:0u,&result);
        printf("result %u\n",(unsigned)okay);
        printf("value %llu\n",(unsigned long long)result);
        printf("fabricated %u\n",kernel_config_fabricated_count());
        printf("clock %llu\n",(unsigned long long)kernel_clock_peek());
        dump("output",OUT,0x208u);
        if (wref) {
            kernel_call_frame frame;
            if (!kernel_frame_build(&frame,OUT+0xa00u,32u,query,5u)) return 20;
            (void)kernel_hle_call(24u,&frame);
            printf("default_fabricated %u\n",kernel_config_fabricated_count());
        }
        return 0;
    }
    if (mode==11) {
        unsigned char *object=(unsigned char *)(uintptr_t)OBJ;
        const uint32_t pool=OBJ+0x2000u;
        const uint32_t pool_bytes=version*4096u;
        if (pool_bytes > 0xf0000u) return 17;
        memset((void *)(uintptr_t)pool,0xa5,pool_bytes);
        object[6]=(unsigned char)version;
        const bool okay=xnet_hle_prepare_pool(OBJ,xref?0u:pool,pool_bytes+(wref?32u:0u));
        printf("result %u\n",(unsigned)okay);
        dump("object",OBJ,0xd50u);
        unsigned char digest[20];
        kernel_crypto_sha1((void *)(uintptr_t)pool,pool_bytes,digest);
        printf("seed ");
        for (unsigned i=0u;i<20u;i++) printf("%02x",digest[i]);
        putchar('\n');
        return 0;
    }
    if (mode==10) {
        unsigned char *configuration=(unsigned char *)(uintptr_t)OUT;
        for (unsigned i=0u;i<76u;i++) configuration[i]=(unsigned char)(xref==0u?0u:xref==1u?1u:xref==2u?255u:i*7u);
        configuration[0]=(unsigned char)version;
        const bool okay=xnet_hle_prepare_config(OBJ,allocation_failure?0u:OUT);
        printf("result %u\n",(unsigned)okay);
        printf("object ");
        for (unsigned i=0u;i<0xd50u;i++) printf("%02x",((unsigned char *)(uintptr_t)OBJ)[i]);
        putchar('\n');
        return 0;
    }
    if (mode==8 || mode==9) {
        unsigned char *parameters=(unsigned char *)(uintptr_t)OUT;
        unsigned char *entropy=(unsigned char *)(uintptr_t)(OUT+0x200u);
        for (unsigned i=0u;i<36u;i++) parameters[i]=(unsigned char)(i*3u+1u);
        for (unsigned i=0u;i<512u;i++) entropy[i]=(unsigned char)(i*7u+2u);
        unsigned char digest[20];
        memset(digest,0xee,sizeof(digest));
        const bool okay=mode==8 ? xnet_hle_seed_digest(xref?0u:OUT,OUT+0x200u,version,digest)
            : xnet_hle_seed_state(xref?0u:OBJ,OUT,OUT+0x200u,version);
        if (mode==9) {
            memcpy(digest,parameters+0x10u,sizeof(digest));
            printf("object ");
            for (unsigned i=0u;i<0xd50u;i++) printf("%02x",((unsigned char *)(uintptr_t)OBJ)[i]);
            putchar('\n');
        }
        printf("result %u\n",(unsigned)okay);
        printf("seed ");
        for (unsigned i=0u;i<20u;i++) printf("%02x",digest[i]);
        putchar('\n');
        return 0;
    }
    const uint32_t result=xnet_hle_call(entry,&frame);
    if (kernel_sync_current_irql()!=0) return 16;
    printf("result %u\n",result);
    printf("singleton %u\n",*(uint32_t *)(uintptr_t)SINGLE);
    printf("lock %u\n",*(uint32_t *)(uintptr_t)(SINGLE-4));
    dump("object",OBJ,0xd50);
    dump("output",OUT,0x200);
    dump("socketbytes",SOCK,0x300);
    xnet_hle_shutdown();
    if (!xnet_hle_set_lifecycle_provider(NULL)) return 10;
    for (unsigned i=0;i<3;i++) if (munmap((void *)(uintptr_t)bases[i],lengths[i])) return 11;
    return 0;
}
