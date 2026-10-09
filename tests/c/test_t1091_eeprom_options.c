/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_options.h"
#include <assert.h>
#include <string.h>
int main(void)
{
    options options;
    char *plain[]={(char *)"host",(char *)"title.xbe"};
    assert(parse_options(2,plain,&options) && options.eeprom_path==NULL && !options.xnet_scheduler);
    char *source[]={(char *)"host",(char *)"--eeprom",(char *)"actual.bin",(char *)"title.xbe"};
    assert(parse_options(4,source,&options) && strcmp(options.eeprom_path,"actual.bin")==0);
    assert(!options.xnet_local_entropy && options.eeprom_key_path==NULL);
    char *missing[]={(char *)"host",(char *)"title.xbe",(char *)"--eeprom"};
    assert(!parse_options(3,missing,&options));
    char *empty[]={(char *)"host",(char *)"--eeprom",(char *)"",(char *)"title.xbe"};
    assert(!parse_options(4,empty,&options));
    char *next[]={(char *)"host",(char *)"--eeprom",(char *)"--disc",(char *)"title.xbe"};
    assert(!parse_options(4,next,&options));
    char *duplicate[]={(char *)"host",(char *)"--eeprom",(char *)"first.bin",(char *)"--eeprom",(char *)"second.bin",(char *)"title.xbe"};
    assert(!parse_options(6,duplicate,&options));
    char *keyed[]={(char *)"host",(char *)"--eeprom",(char *)"actual.bin",(char *)"--eeprom-key",(char *)"key.bin",(char *)"title.xbe"};
    assert(parse_options(6,keyed,&options) && strcmp(options.eeprom_key_path,"key.bin")==0);
    char *orphan[]={(char *)"host",(char *)"--eeprom-key",(char *)"key.bin",(char *)"title.xbe"};
    assert(!parse_options(4,orphan,&options));
    char *missing_key[]={(char *)"host",(char *)"--eeprom",(char *)"actual.bin",(char *)"title.xbe",(char *)"--eeprom-key"};
    assert(!parse_options(5,missing_key,&options));
    char *scheduler[]={(char *)"host",(char *)"--xnet-scheduler",(char *)"title.xbe"};
    assert(parse_options(3,scheduler,&options) && options.xnet_scheduler && !options.xnet_local_entropy);
    return 0;
}
