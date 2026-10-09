/* FABRICATED independent source vectors; actual source/guest byte seam. */
#define main t1074_fixture_main
#include "test_xinput_record.c"
#undef main
static bool trailing;
static xinput_pad_state vector(unsigned i)
{
    xinput_pad_state s={0};
    if(i==0){s.digital_buttons=0x60u;for(unsigned a=0;a<8;a++)s.analog[a]=(uint8_t)(1+a*31);s.thumb_left_x=-32768;s.thumb_left_y=32767;s.thumb_right_x=-91;s.thumb_right_y=17;}
    if(i>=1&&i<=3){s.digital_buttons=0x86u;s.analog[1]=253;s.analog[7]=111;s.thumb_left_y=-63;s.thumb_right_x=32767;}
    if(i>=4&&i<=6){s.digital_buttons=0x29u;s.analog[4]=13;s.thumb_right_y=-32768;}
    return s;
}
static bool independent_source(uint64_t poll,xinput_pad_state *out,void *unused)
{
    (void)unused;
    if(poll==2||poll==3||(trailing&&poll>=2))return false;
    *out=vector((unsigned)poll);return true;
}
static const char own_xbe[]="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char own_flags[]="fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
static void write_fixture(const char *path,const char *body)
{
    char header[512];size_t n=xinput_record_header(header,sizeof(header),own_xbe,own_flags,"--native-xmv --ac97-ready");
    FILE *f=fopen(path,"wb");CHECK(f);if(!f)return;CHECK(fwrite(header,1,n,f)==n);fputs(body,f);fclose(f);
}
static void parser_domain(const char *path)
{
    char error[600];uint64_t total=999;
    CHECK(!xinput_record_open("/nonexistent-t1076-owned-directory/file",own_xbe,own_flags,"",error,sizeof(error)));
    CHECK(strstr(error,"cannot create"));
    CHECK(!xinput_replay_load("/nonexistent-t1076-owned-directory/file",own_xbe,own_flags,"",error,sizeof(error),&total));
    CHECK(total==999&&strstr(error,"cannot open"));
    write_fixture(path,"100 BACK BLACK=87 RY=181\n# polls: 100\n");
    CHECK(xinput_replay_load(path,own_xbe,own_flags,"",error,sizeof(error),NULL));
    CHECK_EQ_U32(xinput_pad_state_read(HANDLE,OUT),0u);uint8_t preserved[22];memcpy(preserved,kernel_guest_at(OUT,22),22);
    const char *bad[]={"2 RY=32768\n","1 WHITE=256\n","-1 BACK\n","4 INVALID\n","2 LX=\n"};
    for(unsigned i=0;i<5;i++){write_fixture(path,bad[i]);CHECK(!xinput_replay_load(path,own_xbe,own_flags,"",error,sizeof(error),NULL));CHECK(strstr(error,"line"));CHECK_EQ_U32(xinput_pad_state_read(HANDLE,OUT),0u);CHECK(memcmp(preserved,kernel_guest_at(OUT,22),22)==0);}
    write_fixture(path,"3 RTHUMB LT=31 RX=-123\n# polls: 3\n");
    CHECK(!xinput_replay_load(path,"different-xbe",own_flags,"",error,sizeof(error),NULL));CHECK(strstr(error,"different XBE"));
    CHECK(!xinput_replay_load(path,own_xbe,"different-flags","--other",error,sizeof(error),NULL));CHECK(strstr(error,"different flag set"));
    /* DIAGNOSTIC: syntactically complete truncated records have no completeness gate. */
    write_fixture(path,"3 RTHUMB LT=31 RX=-123\n");
    CHECK(xinput_replay_load(path,own_xbe,own_flags,"",error,sizeof(error),&total));CHECK(total==3);
    printf("LIMIT missing-trailer accepted total=%llu\n",(unsigned long long)total);
    write_fixture(path,"3 RTHUMB LT=31 RX=-123\n# polls: 999\n");
    CHECK(xinput_replay_load(path,own_xbe,own_flags,"",error,sizeof(error),&total));CHECK(total==3);
    printf("LIMIT inconsistent-trailer accepted total=%llu\n",(unsigned long long)total);
    char header[512];size_t header_len=xinput_record_header(header,sizeof(header),own_xbe,own_flags,"--native-xmv --ac97-ready");
    CHECK(!xinput_record_check_header(header,8,own_xbe,own_flags,"",error,sizeof(error)));
    CHECK(!xinput_record_check_header(header,header_len-12,own_xbe,"different","",error,sizeof(error)));
    CHECK(!xinput_record_check_header("# tsfp-input v9\n",16,own_xbe,own_flags,"",error,sizeof(error)));
    char *argv[]={"host","title.xbe","--native-xmv","--hdd","private-hdd","--ac97-ready","--replay-input","record","--audio-mute"};char identity[128];
    CHECK(xinput_record_identity_flags(9,argv,identity,sizeof(identity)));CHECK(strcmp(identity,"--native-xmv --ac97-ready")==0);
}
int main(int argc,char **argv)
{
    trailing=argc>1&&strcmp(argv[1],"trailing")==0;
    char path[]="/tmp/t1076-record-XXXXXX",error[600];int fd=mkstemp(path);CHECK(fd>=0);if(fd<0)return 1;close(fd);
    initialise();xinput_source_install(independent_source,NULL);
    CHECK(xinput_record_open(path,own_xbe,own_flags,"--native-xmv --ac97-ready",error,sizeof(error)));
    uint8_t captured[8][22];unsigned count=trailing?5:8;
    for(unsigned i=0;i<count;i++){
        memset(kernel_guest_at(OUT-8,38),0xA5,38);CHECK_EQ_U32(xinput_pad_state_read(HANDLE,OUT),0u);
        memcpy(captured[i],kernel_guest_at(OUT,22),22);
        for(unsigned j=0;j<8;j++){CHECK(((uint8_t*)kernel_guest_at(OUT-8,8))[j]==0xA5);CHECK(((uint8_t*)kernel_guest_at(OUT+22,8))[j]==0xA5);}
    }
    CHECK(xinput_source_port_poll_count(0)==count);xinput_record_close();uint64_t recorded=xinput_record_poll_count();
    printf("polls source=%u recorded=%llu\n",count,(unsigned long long)recorded);
    CHECK(recorded==count);finish();initialise();
    uint64_t total=0;CHECK(xinput_replay_load(path,own_xbe,own_flags,"--native-xmv --ac97-ready",error,sizeof(error),&total));CHECK(total==count);
    for(unsigned i=0;i<count;i++){CHECK_EQ_U32(xinput_pad_state_read(HANDLE,OUT),0u);CHECK(memcmp(captured[i],kernel_guest_at(OUT,22),22)==0);}
    if(!trailing)parser_domain(path);
    finish();unlink(path);
    printf("T1076: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
