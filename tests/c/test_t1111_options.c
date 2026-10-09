/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_options.h"
#include <stdio.h>
#include <string.h>
int main(void)
{
    options result;
    memset(&result, 0xA5, sizeof(result));
    char *plain[]={"host","input.xbe"};
    if(!parse_options(2,plain,&result) || result.xnet_local_entropy) return 1;
    char *enabled[]={"host","--xnet-local-entropy","input.xbe"};
    if(!parse_options(3,enabled,&result) || !result.xnet_local_entropy) return 2;
    char *wrong[]={"host","--xnet-local-entropy-typo","input.xbe"};
    if(parse_options(3,wrong,&result)) return 3;
    puts("T1111 host option: default off, explicit on, typo refused");
    return 0;
}
