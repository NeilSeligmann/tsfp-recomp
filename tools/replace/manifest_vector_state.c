/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exact TLS storage for metadata-only registry extraction. Matches runtime_min.c;
 * this executable never runs guest adapters and supplies no vector proof capability.
 * Strong storage supplied by a custom manifest runtime takes precedence. */
#include <stdint.h>

typedef union ManifestXmm {
    float f[4];
    double d[2];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
} ManifestXmm;
_Static_assert(sizeof(ManifestXmm) == 16u, "raw XMM ABI requires 128 bits");
_Static_assert(_Alignof(ManifestXmm) == 8u, "match native runtime XMM TLS alignment");
#define MANIFEST_VECTOR_TLS __attribute__((weak, aligned(8)))
__thread ManifestXmm g_xmm0 MANIFEST_VECTOR_TLS, g_xmm1 MANIFEST_VECTOR_TLS;
__thread ManifestXmm g_xmm2 MANIFEST_VECTOR_TLS, g_xmm3 MANIFEST_VECTOR_TLS;
__thread ManifestXmm g_xmm4 MANIFEST_VECTOR_TLS, g_xmm5 MANIFEST_VECTOR_TLS;
__thread ManifestXmm g_xmm6 MANIFEST_VECTOR_TLS, g_xmm7 MANIFEST_VECTOR_TLS;
__thread uint32_t g_harness_mxcsr __attribute__((weak, aligned(4)));
