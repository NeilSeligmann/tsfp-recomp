/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent boundary/refusal controls, reusing the published Vulkan fixture.
 * Literal footprints are finite native controls, not retail pixel acceptance. */
#define main t1039_fixture_main
#include "test_t1039_point_size.c"
#undef main
int main(int argc, char **argv)
{
    if (argc != 2 || !read_shader(argv[1],0u) || !read_shader(argv[1],1u)) return 2;
    gpu_device *device=NULL;
    if (gpu_device_create_selected("software",&device)!=GPU_OK) return 77;
    rig r; CHECK(rig_init(&r,device));
    float_vertex vertex={{40.5f,40.5f,0.5f},{1,1,1,1}};
    memcpy(memory,&vertex,sizeof vertex);
    for(unsigned writer=0;writer<2;writer++) {
        one_case(&r,writer,8u,0u,false,EVERYTHING,1u,NULL);
        one_case(&r,writer,7u,0u,false,EVERYTHING,0u,"largePoints");
        one_case(&r,writer,0xffffffffu,0u,r.device.large_points,EVERYTHING,0u,"0..511");
        one_case(&r,writer,8u,2u,r.device.large_points,EVERYTHING,0u,"point parameters");
        one_case(&r,writer,8u,0u,r.device.large_points,EVERYTHING & ~GPU_PGRAPH_INFER_POINT_SIZE,0u,"point");
        if(r.device.large_points) one_case(&r,writer,64u,0u,true,EVERYTHING,64u,NULL);
    }
    r.fn.vkDestroyRenderPass(r.native.device,r.colour_pass,NULL);
    gpu_device_destroy(device);free(point_modules[0]);free(point_modules[1]);
    printf("T1042 independent point review: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
