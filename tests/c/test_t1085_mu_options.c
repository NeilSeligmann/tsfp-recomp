/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_options.h"
#include <stdio.h>
#include <string.h>
static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL line%d: %s\n",__LINE__,#c); } } while(0)
int main(void)
{
    options out;
    char *defaults[]={"host","retail.xbe"};
    CHECK(parse_options(2,defaults,&out)); CHECK(out.mu_image_count==0u);
    char *ok[]={"host","retail.xbe","--mu-image","3","1","image with spaces.bin","--mu-image","0","0","other.bin"};
    CHECK(parse_options(10,ok,&out)); CHECK(out.mu_image_count==2u);
    CHECK(out.mu_images[0].port==3u && out.mu_images[0].slot==1u);
    CHECK(out.mu_images[0].path != NULL && strcmp(out.mu_images[0].path,"image with spaces.bin")==0);
    CHECK(out.mu_images[1].port==0u && out.mu_images[1].slot==0u);
    for (int argc=3;argc<6;argc++) CHECK(!parse_options(argc,ok,&out));
    const char *ports[]={"4","-1","+1","01"," 1","1x","4294967296",""};
    char *one[]={"host","retail.xbe","--mu-image","0","0","existing.bin"};
    for(unsigned i=0;i<sizeof(ports)/sizeof(ports[0]);i++) {one[3]=(char *)ports[i];CHECK(!parse_options(6,one,&out));}
    one[3]="0";
    const char *slots[]={"2","-1","+1","01"," 1","1x","4294967296",""};
    for(unsigned i=0;i<sizeof(slots)/sizeof(slots[0]);i++) {one[4]=(char *)slots[i];CHECK(!parse_options(6,one,&out));}
    one[4]="0"; one[5]=""; CHECK(!parse_options(6,one,&out));
    char *duplicate[]={"host","retail.xbe","--mu-image","2","1","a.bin","--mu-image","2","1","b.bin"};
    CHECK(!parse_options(10,duplicate,&out));
    char *all[34]={"host","retail.xbe"}; const char *digits[]={"0","1","2","3"};
    for(unsigned i=0;i<8u;i++) {all[2u+4u*i]="--mu-image";all[3u+4u*i]=(char *)digits[i/2u];all[4u+4u*i]=(char *)digits[i%2u];all[5u+4u*i]="image.bin";}
    CHECK(parse_options(34,all,&out)); CHECK(out.mu_image_count==8u);
    CHECK(parse_options(2,defaults,&out)); CHECK(out.mu_image_count==0u);
    printf("MU options: %d checks, %d failures\n",checks,failures);return failures!=0;
}
