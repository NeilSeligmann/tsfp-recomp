/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_lock.h"
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb"), *output = fopen(argv[2], "wb");
    if (!input || !output) return 2;
    uint32_t args[9];
    uint8_t parent[32], surface[32], rectangle[16], metadata[64], output_initial[16];
    if (fread(args, sizeof(args), 1u, input) != 1u || fread(parent,sizeof(parent),1u,input)!=1u ||
        fread(surface,sizeof(surface),1u,input)!=1u || fread(rectangle,sizeof(rectangle),1u,input)!=1u ||
        fread(metadata,sizeof(metadata),1u,input)!=1u || fread(output_initial,sizeof(output_initial),1u,input)!=1u) return 2;
    fclose(input);
    /* Reserve only the required fixed regions. The general test environment
     * allocates a random scratch arena that can collide with the captured VA. */
    /* args[8] is the guest address of the parent (texture) header, which the XMV movie player keeps
     * on its stack (T755): the original lock never reads where it lives, the port must not either. */
    const uint32_t parent_header = args[8];
    const uint32_t parent_page = parent_header & ~0xFFFu;
    guest_mem_reset();
    d3d8_resource_reset();
    map_fixed(D3D_REGION_BASE,D3D_REGION_BYTES);
    map_fixed(0x00665000u,0x2000u); /* the level-0 surface header stays here */
    if (parent_page != 0x00665000u) map_fixed(parent_page,0x2000u);
    map_fixed(0x00A00000u,0x1000u);
    guest_region_request request = {.bytes=args[4],.fixed_base=args[5],.contiguous=true,
                                    .state=MEM_COMMIT,.protect=PAGE_READWRITE};
    nt_status status;
    if (guest_region_alloc(&request,&status) != args[5]) return 2;
    memcpy(kernel_guest_at(parent_header,sizeof(parent)),parent,sizeof(parent));
    memcpy(kernel_guest_at(args[0],sizeof(surface)),surface,sizeof(surface));
    memcpy(kernel_guest_at(0xA00080u,sizeof(rectangle)),rectangle,sizeof(rectangle));
    memcpy(kernel_guest_at(0x3E1828u,sizeof(metadata)),metadata,sizeof(metadata));
    (void)d3d8_register_resource(parent_header,args[5]);
    store(0x3E3F58u,args[6]);
    store(parent_header+8u,args[7]);
    memcpy(kernel_guest_at(args[1],sizeof(output_initial)),output_initial,sizeof(output_initial));
    const uint32_t result=d3d8_surface_lock_rect(args[0],args[1],args[2],args[3]);
    if (fwrite(&result,sizeof(result),1u,output)!=1u ||
        fwrite(kernel_guest_at(args[1],16u),16u,1u,output)!=1u ||
        fwrite(kernel_guest_at(args[0],32u),32u,1u,output)!=1u ||
        fwrite(kernel_guest_at(parent_header,32u),32u,1u,output)!=1u) return 2;
    fclose(output);
    guest_mem_reset();
    return 0;
}
