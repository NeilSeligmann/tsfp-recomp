/* SPDX-License-Identifier: GPL-3.0-or-later
 * Synthetic literal point coverage through the real live translator and renderer.
 * No retail bytes; fixed state must override both missing and explicit oPts. */
#define VK_NO_PROTOTYPES
#include "live_vk_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)
#include "live_vk_scenes.h"
#include "live_vk_rig.h"

static uint32_t *point_modules[2];
static size_t point_counts[2];
static const char *const point_names[] = {
    "static_d7e398d2b2d641bb529e33252688cd47a1f93a3bfa56e083886ee43904210f6c",
    "static_a9bcec9032357a15fe2b9a26a6256e3a142d4b54adf4df4797853587e112f5bb"
};
static const struct gpu_vsh_table point_table = {0u,0u,NULL,0u,NULL,2u,point_names};
static const uint32_t point_program[] = {
    0,0x0020021B,0x0836186C,0x0000F800,
    0,0x0020041B,0x0836186C,0x0000F818,
    0,0x0020041B,0x0836186C,0x0000F831
};
static bool point_load(void *context, uint32_t index, const uint32_t **words, size_t *count)
{
    (void)context;
    if (index >= 2u) return false;
    *words = point_modules[index]; *count = point_counts[index];
    return *words != NULL;
}
static bool read_shader(const char *directory, unsigned index)
{
    char name[1024];
    if (snprintf(name,sizeof name,"%s/%s.spv",directory,index ? "with_point" : "without_point") >= (int)sizeof name) return false;
    FILE *file=fopen(name,"rb");
    if (file==NULL) return false;
    if (fseek(file,0,SEEK_END)!=0) { fclose(file); return false; }
    long bytes=ftell(file);
    if (bytes<=0 || bytes%4!=0 || fseek(file,0,SEEK_SET)!=0) { fclose(file); return false; }
    point_modules[index]=malloc((size_t)bytes);
    const bool okay=point_modules[index]!=NULL && fread(point_modules[index],1u,(size_t)bytes,file)==(size_t)bytes;
    fclose(file); point_counts[index]=(size_t)bytes/4u;
    return okay;
}
static gpu_pgraph *point_model(bool program_point, uint32_t size, uint32_t parameters)
{
    stream_builder stream={0};
    const float offset[4]={0,0,0,0}, scale[4]={1,1,1,1};
    stream_viewport(&stream,offset,scale);
    stream_pair(&stream,GPU_PGRAPH_EXECUTION_MODE,6u);
    uint32_t words[12]; memcpy(words,point_program,sizeof words);
    if (!program_point) words[7] |= 1u;
    stream_program(&stream,0u,words,program_point ? 3u : 2u);
    stream_pair(&stream,GPU_PGRAPH_PROGRAM_START,0u);
    stream_pair(&stream,0x0318u,parameters);
    stream_pair(&stream,0x031Cu,0u);
    stream_pair(&stream,0x043Cu,size);
    stream_array(&stream,1u,MEMORY_BASE,array_format(28u,3u,GPU_PGRAPH_TYPE_F));
    stream_array(&stream,2u,MEMORY_BASE+12u,array_format(28u,4u,GPU_PGRAPH_TYPE_F));
    stream_draw_arrays(&stream,GPU_PGRAPH_OP_POINTS,0u,1u);
    gpu_pgraph *model=gpu_pgraph_create();
    CHECK(model!=NULL);
    gpu_pgraph_set_output_groups(model,ALL_GROUPS | GPU_PGRAPH_OUTPUT_POLYGON_OFFSET);
    CHECK(gpu_pgraph_decode(model,stream.pairs,stream.count)==GPU_PGRAPH_OK);
    stream_free(&stream); return model;
}
static unsigned coloured(const gpu_image *image)
{
    unsigned count=0u;
    for (uint32_t y=0u;y<image->height;y++) for (uint32_t x=0u;x<image->width;x++) {
        const uint8_t *pixel=image->pixels+(size_t)y*image->stride_bytes+x*4u;
        if (pixel[0] || pixel[1] || pixel[2]) count++;
    }
    return count;
}
static void one_case(rig *r, bool program_point, uint32_t size, uint32_t parameters,
                     bool large_points, uint32_t inferences, unsigned expected, const char *refusal)
{
    fake_guest guest={MEMORY_BASE,memory,sizeof memory};
    gpu_pgraph_backend backend=make_backend(&guest,inferences);
    backend.table=&point_table; backend.load_module=point_load; backend.live_raster_modules=true;
    live_vk_device device=r->device; device.large_points=large_points;
    char error[256]="";
    live_vk_renderer *renderer=live_vk_renderer_create(&device,r->colour_pass,&backend,error,sizeof error);
    CHECK(renderer!=NULL);
    if (renderer==NULL) return;
    gpu_pgraph *model=point_model(program_point,size,parameters);
    const float clear[4]={0,0,0,1}; frame_result result;
    render_live(r,renderer,LIVE_VK_PASS_WINDOW,model,clear,&result);
    if (refusal==NULL) {
        CHECK(result.drawn==1u && result.refused==0u);
        CHECK(coloured(&result.image)==expected);
    } else {
        CHECK(result.drawn==0u && result.refused==1u);
        CHECK(strstr(result.first_refusal,refusal)!=NULL);
        CHECK(coloured(&result.image)==0u);
    }
    printf("point program=%u word=%u params=%u large=%u pixels=%u refusal=%s\n",
           program_point,size,parameters,large_points,coloured(&result.image),result.first_refusal);
    gpu_image_free(&result.image); gpu_pgraph_destroy(model); live_vk_renderer_destroy(renderer);
}
int main(int argc,char **argv)
{
    if (argc!=2 || !read_shader(argv[1],0u) || !read_shader(argv[1],1u)) return 2;
    gpu_device *device=NULL;
    if (gpu_device_create_selected("software",&device)!=GPU_OK) { puts("SKIP: no software Vulkan device"); return 77; }
    rig r; CHECK(rig_init(&r,device));
    float_vertex vertex={{20.5f,20.5f,0.5f},{1,1,1,1}};
    memcpy(memory,&vertex,sizeof vertex);
    for (unsigned program=0u;program<2u;program++) {
        one_case(&r,program,0u,0u,r.device.large_points,EVERYTHING,1u,NULL);
        one_case(&r,program,8u,0u,r.device.large_points,EVERYTHING,1u,NULL);
        if (r.device.large_points) {
            one_case(&r,program,16u,0u,true,EVERYTHING,4u,NULL);
            one_case(&r,program,32u,0u,true,EVERYTHING,16u,NULL);
        }
    }
    one_case(&r,false,16u,0u,false,EVERYTHING,0u,"largePoints");
    one_case(&r,false,8u,0u,false,EVERYTHING,1u,NULL);
    one_case(&r,false,512u,0u,r.device.large_points,EVERYTHING,0u,"0..511");
    one_case(&r,false,8u,1u,r.device.large_points,EVERYTHING,0u,"point parameters");
    one_case(&r,false,8u,0u,r.device.large_points,EVERYTHING & ~GPU_PGRAPH_INFER_POINT_SIZE,0u,"point");
    r.fn.vkDestroyRenderPass(r.native.device,r.colour_pass,NULL);
    gpu_device_destroy(device);free(point_modules[0]);free(point_modules[1]);
    printf("T1039 point size: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
