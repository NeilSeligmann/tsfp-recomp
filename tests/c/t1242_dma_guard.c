/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent synthetic descriptor controls against pinned assertion domains. */
#include "../../src/audio/dsound_effects_dsp.c"
#include <assert.h>
int main(void) {
    dsound_effects_dsp d={0};d.core.is_gp=true;dsp56k_reset_cpu(&d.core);
    d.dma.next_block=0x100u;
    const uint32_t node[7]={0x4000u,0x19E0u,4u,0x200u,0u,0u,0xFFFFu};
    for(unsigned i=0;i<7;i++)dsp56k_write_memory(&d.core,DSP_SPACE_X,0x100u+i,node[i]);
    assert(dma_supported(&d));
    dsp56k_write_memory(&d.core,DSP_SPACE_X,0x101u,0x19E4u);assert(!dma_supported(&d));
    dsp56k_write_memory(&d.core,DSP_SPACE_X,0x101u,0x0DE0u);assert(!dma_supported(&d));
    dsp56k_write_memory(&d.core,DSP_SPACE_X,0x101u,node[1]);
    dsp56k_write_memory(&d.core,DSP_SPACE_X,0x103u,0x17FFu);assert(!dma_supported(&d));
    dsp56k_write_memory(&d.core,DSP_SPACE_X,0x103u,node[3]);
    dsp56k_write_memory(&d.core,DSP_SPACE_X,0x100u,0x100u);assert(!dma_supported(&d));
    dsp56k_write_memory(&d.core,DSP_SPACE_X,0x100u,node[0]);
    d.dma.next_block=0x17FAu;assert(!dma_supported(&d));
    d.dma.next_block=0x2000u;assert(!dma_supported(&d));
    d.dma.next_block=0x4000u;assert(dma_supported(&d));
    return 0;
}
