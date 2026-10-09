/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_pgraph.h"
#include <stdio.h>
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %u: %s\n", __LINE__, #x); } } while (0)
static void capture_identity(void *context,gpu_pgraph_query *event)
{
    event->report_address = 0x81234000u;
    event->report_generation = *(uint64_t *)context;
}
int main(void)
{
    gpu_pgraph *p = gpu_pgraph_create();
    CHECK(p != NULL);
    if (p == NULL) return 1;
    const gpu_pgraph_command original[] = {{0x17C8u,1u},{0x17CCu,1u},{0x17CCu,0u},{0x17D0u,0x010FF000u}};
    CHECK(gpu_pgraph_decode(p, original, 4u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_query_count(p) == 0u);
    gpu_pgraph_reset(p); gpu_pgraph_set_strict(p,true);
    CHECK(gpu_pgraph_decode(p,original,1u) == GPU_PGRAPH_ERR_UNMEASURED);
    gpu_pgraph_reset(p); gpu_pgraph_set_visibility(p,true);
    CHECK(gpu_pgraph_decode(p,original,4u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_query_count(p) == 4u);
    for (unsigned i=0; i<4u; i++) {
        const gpu_pgraph_query *q=gpu_pgraph_query_at(p,i);
        CHECK(q != NULL);
        CHECK(q->method == original[i].method && q->data == original[i].data);
        CHECK(q->before_draw == 0u && q->command == i);
    }
    CHECK(gpu_pgraph_query_at(p,4u) == NULL);
    CHECK(gpu_pgraph_begin_frame(p) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_query_count(p) == 0u);
    CHECK(gpu_pgraph_decode(p,original,4u) == GPU_PGRAPH_OK);
    gpu_pgraph_reset(p);
    const gpu_pgraph_command bad[]={{0x17C8u,0u},{0x17CCu,2u},{0x17D0u,0x020FF000u},{0x17D0u,0x010FF004u}};
    for(unsigned i=0;i<4u;i++) {
        CHECK(gpu_pgraph_decode(p,&bad[i],1u) == GPU_PGRAPH_ERR_UNMEASURED);
        CHECK(gpu_pgraph_query_count(p) == 0u);
        gpu_pgraph_reset(p);
    }
    for(unsigned i=0;i<GPU_PGRAPH_MAX_QUERY_EVENTS;i++)
        CHECK(gpu_pgraph_decode(p,original,1u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_decode(p,original,1u) == GPU_PGRAPH_ERR_FULL);
    CHECK(gpu_pgraph_query_count(p) == GPU_PGRAPH_MAX_QUERY_EVENTS);
    gpu_pgraph_reset(p);
    uint64_t generation=17u;
    gpu_pgraph_set_report_identity(p,capture_identity,&generation);
    CHECK(gpu_pgraph_decode(p,original,4u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_query_at(p,0u)->report_generation == 0u);
    CHECK(gpu_pgraph_query_at(p,3u)->report_address == 0x81234000u);
    generation=18u;
    CHECK(gpu_pgraph_query_at(p,3u)->report_generation == 17u);
    gpu_pgraph_reset(p);
    CHECK(gpu_pgraph_decode(p,original,4u) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_query_at(p,3u)->report_generation == 18u);
    gpu_pgraph_destroy(p);
    printf("visibility decode: %u checks, %u failures\n",checks,failures);
    return failures != 0u;
}
