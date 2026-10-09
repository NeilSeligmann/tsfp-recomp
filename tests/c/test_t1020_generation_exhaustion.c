/* SPDX-License-Identifier: GPL-3.0-or-later
 * FABRICATED adverse counter state injected only into an isolated source copy. */
#include "guest_mem.h"
#include <stdio.h>
kernel_log_fn kernel_hle_log(void) { return printf; }
int main(void)
{
    const guest_region_request request={.bytes=4096u,.lowest_physical=0u,
        .highest_physical=0xFFFFFFu,.alignment=4096u,.protect=PAGE_READWRITE,
        .state=MEM_COMMIT,.contiguous=true};
    nt_status status=0x12345678u;
    kernel_guest_ptr result=guest_region_alloc(&request,&status);
    printf("EXHAUSTION address=%08x status=%08x mapped=%llu regions=%zu\n",result,status,
        (unsigned long long)guest_mem_mapped_bytes(),guest_mem_region_count());
    return result==0u && status==STATUS_NO_MEMORY && guest_mem_mapped_bytes()==0u &&
        guest_mem_region_count()==0u ? 0 : 1;
}
