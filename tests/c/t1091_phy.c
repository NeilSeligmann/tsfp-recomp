/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xnet_phy.h"
#include <stddef.h>
#include <string.h>
typedef struct {
    xnet_nvnet device;
    xnet_phy phy;
    uint64_t microseconds;
    uint32_t delay_calls;
} fixture;
static void delay(void *context,uint32_t microseconds)
{
    fixture *f=context;
    f->microseconds+=microseconds;
    ++f->delay_calls;
}
size_t phy_fixture_bytes(void){return sizeof(fixture);}
void phy_fixture_init(fixture *f,uint32_t revision,uint32_t initialized,uint32_t flags,
    uint32_t busy,uint32_t control,uint32_t status,uint32_t local,uint32_t peer)
{
    memset(f,0,sizeof(*f));
    (void)xnet_nvnet_reset_no_peer(&f->device);
    (void)xnet_phy_context_init(&f->phy,&f->device,delay,f,(uint8_t)revision);
    atomic_store(&f->phy.initialized,initialized);atomic_store(&f->phy.flags,flags);
    atomic_store(&f->phy.busy,busy);
    f->device.phy[0]=control;f->device.phy[1]=status;
    f->device.phy[4]=local;f->device.phy[5]=peer;
}
int phy_fixture_call(fixture *f,uint32_t kind,uint32_t arg0,uint32_t arg1,uint32_t *out)
{
    return kind?xnet_phy_get_link_state(&f->phy,arg0,out):xnet_phy_initialize(&f->phy,arg0,arg1,out);
}
uint32_t phy_fixture_flags(fixture *f){return f->phy.flags;}
uint32_t phy_fixture_ready(fixture *f){return (uint32_t)f->phy.initialized;}
uint32_t phy_fixture_busy(fixture *f){return atomic_load(&f->phy.busy);}
uint64_t phy_fixture_time(fixture *f){return f->microseconds;}
uint32_t phy_fixture_delays(fixture *f){return f->delay_calls;}
void *phy_fixture_device(fixture *f){return &f->device;}
