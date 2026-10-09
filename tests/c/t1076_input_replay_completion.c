/* Independent strict completion domain, separate from immutable baseline diagnostics. */
#define main t1074_fixture_main
#include "test_xinput_record.c"
#undef main
static const char xbe[]="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char flags_hash[]="fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
static void put(const char *path,const char *body)
{
    char header[512];size_t n=xinput_record_header(header,sizeof(header),xbe,flags_hash,"--ac97-ready");
    FILE *f=fopen(path,"wb");CHECK(f);if(!f)return;CHECK(fwrite(header,1,n,f)==n);fputs(body,f);fclose(f);
}
static bool decline_first(uint64_t poll,xinput_pad_state *out,void *unused)
{
    (void)unused;if(poll<2)return false;*out=(xinput_pad_state){0};return true;
}
int main(void)
{
    char path[]="/tmp/t1076-completion-XXXXXX",error[600];int fd=mkstemp(path);CHECK(fd>=0);if(fd<0)return 1;close(fd);
    initialise();uint64_t total=0;
    put(path,"1 RTHUMB Y=254 LX=-32768\n2 UP WHITE=1 RY=32767\n# lossy: note\n# polls: 3\n");
    CHECK(xinput_replay_load(path,xbe,flags_hash,"--ac97-ready",error,sizeof(error),&total));CHECK(total==3);
    put(path,"# polls: 0\n");CHECK(xinput_replay_load(path,xbe,flags_hash,"",error,sizeof(error),&total));CHECK(total==0);
    put(path,"100 LEFT BLACK=13 LT=252 RX=-32768\n# polls: 100\n");
    CHECK(xinput_replay_load(path,xbe,flags_hash,"",error,sizeof(error),&total));CHECK(total==100);
    CHECK_EQ_U32(xinput_pad_state_read(HANDLE,OUT),0u);uint8_t expected[22];memcpy(expected,kernel_guest_at(OUT,22),22);
    const char *bad[]={
      "3 BACK\n", "3 BACK\n# polls: 2\n", "3 BACK\n# polls: 4\n",
      "3 BACK\n# polls: 3\n# polls: 3\n", "3 BACK\n# polls: 3\n1 UP\n",
      "3 BACK\n# polls: 3", "3 BACK\n# polls: +3\n", "3 BACK\n# polls: -3\n",
      "3 BACK\n# polls: 18446744073709551616\n", "3 BACK\n# polls: 3junk\n",
      "1 LX=-32769\n# polls: 1\n", "1 RT=256\n# polls: 1\n", "2 WAT\n# polls: 2\n"
    };
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        put(path,bad[i]);uint64_t unchanged=0x123456789ABCDEF0ull;uint64_t poll=xinput_source_poll_count();
        CHECK(!xinput_replay_load(path,xbe,flags_hash,"",error,sizeof(error),&unchanged));CHECK(unchanged==0x123456789ABCDEF0ull);
        CHECK(error[0]!=0);CHECK(xinput_source_poll_count()==poll);
        CHECK_EQ_U32(xinput_pad_state_read(HANDLE,OUT),0u);CHECK(memcmp(expected,kernel_guest_at(OUT,22),22)==0);
    }
    put(path,"3 BACK\n# polls: 3\n");FILE *f=fopen(path,"ab");CHECK(f);if(f){fputc(0,f);fclose(f);}
    CHECK(!xinput_replay_load(path,xbe,flags_hash,"",error,sizeof(error),NULL));
    CHECK(!xinput_record_open("/dev/full",xbe,flags_hash,"",error,sizeof(error)));CHECK(strstr(error,"write"));
    CHECK(xinput_record_open(path,xbe,flags_hash,"",error,sizeof(error)));
    CHECK(!xinput_record_open(path,xbe,flags_hash,"",error,sizeof(error)));CHECK(strstr(error,"already"));
    xinput_pad_state a={.digital_buttons=0x84,.analog={0,0,0,0,99,0,17,0},.thumb_right_y=-13};
    xinput_record_observe(0,1,&a);CHECK(xinput_record_poll_count()==0);
    xinput_record_observe(0,0,&a);xinput_record_observe(1,0,NULL);CHECK(xinput_record_poll_count()==2);xinput_record_close();
    CHECK(xinput_replay_load(path,xbe,flags_hash,"",error,sizeof(error),&total));CHECK(total==2);
    finish();initialise();
    /* DIAGNOSTIC only: recording starts at rest, not a snapshot of preexisting pad state. */
    CHECK(xinput_hle_set_synthetic_pad_state(0,a));xinput_source_install(decline_first,NULL);
    CHECK(xinput_record_open(path,xbe,flags_hash,"",error,sizeof(error)));
    uint8_t held[22];CHECK_EQ_U32(xinput_pad_state_read(HANDLE,OUT),0u);memcpy(held,kernel_guest_at(OUT,22),22);
    CHECK_EQ_U32(xinput_pad_state_read(HANDLE,OUT),0u);CHECK(memcmp(held,kernel_guest_at(OUT,22),22)==0);
    CHECK_EQ_U32(xinput_pad_state_read(HANDLE,OUT),0u);xinput_record_close();CHECK(xinput_record_poll_count()==3);
    finish();initialise();CHECK(xinput_replay_load(path,xbe,flags_hash,"",error,sizeof(error),&total));CHECK(total==3);
    CHECK_EQ_U32(xinput_pad_state_read(HANDLE,OUT),0u);CHECK(memcmp(held,kernel_guest_at(OUT,22),22)!=0);
    printf("LIMIT leading declined polls with preexisting nonrest pad have no initial snapshot\n");
    finish();unlink(path);printf("T1076 strict: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
