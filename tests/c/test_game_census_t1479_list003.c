/* SPDX-License-Identifier: GPL-3.0-or-later
 * Expected values measured by original directed execution, independent of C drafts.
 */
#include "game_replace.h"
#include <stdio.h>
#ifndef T1479_EMBEDDED
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_esi, g_edi, g_ebx, g_ebp;
__thread int g_df;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x810000];
static unsigned checks, failures;
#define CHECK(e) do {++checks;if(!(e)){++failures;fprintf(stderr,"line %d\n",__LINE__);}}while(0)
#endif
static unsigned char census003_before[0x810000];
static unsigned char census003_written[0x810000];
struct census003_byte {
    uint32_t address;
    uint8_t value;
};
void sub_002A9B70(void);
void sub_002A9BE0(void);
void sub_002A9C40(void);
void sub_002A9CA0(void);
void sub_002A9E10(void);
void sub_002BE600(void);
void sub_002FBB10(void);
void sub_0031DD10(void);
void sub_00359F40(void);
void sub_0036E8C0(void);
void sub_00377730(void);
static const struct census003_byte census003_initial_0[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20038u, 0x11u
    }, {
        0x20039u, 0x11u
    }, {
        0x2003au, 0x0u
    }, {
        0x2003bu, 0x0u
    }, {
        0x2003cu, 0x22u
    }, {
        0x2003du, 0x22u
    }, {
        0x2003eu, 0x0u
    }, {
        0x2003fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_0[] = {
    {
        0xff4u, 0x88u
    }, {
        0xff5u, 0x77u
    }, {
        0xff6u, 0x66u
    }, {
        0xff7u, 0x55u
    }, {
        0xff8u, 0x1u
    }, {
        0xff9u, 0xefu
    }, {
        0xffau, 0xcdu
    }, {
        0xffbu, 0xabu
    }, {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x0u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x1u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_1[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20038u, 0x11u
    }, {
        0x20039u, 0x11u
    }, {
        0x2003au, 0x0u
    }, {
        0x2003bu, 0x0u
    }, {
        0x2003cu, 0x22u
    }, {
        0x2003du, 0x22u
    }, {
        0x2003eu, 0x0u
    }, {
        0x2003fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_1[] = {
    {
        0xff4u, 0x88u
    }, {
        0xff5u, 0x77u
    }, {
        0xff6u, 0x66u
    }, {
        0xff7u, 0x55u
    }, {
        0xff8u, 0x1u
    }, {
        0xff9u, 0xefu
    }, {
        0xffau, 0xcdu
    }, {
        0xffbu, 0xabu
    }, {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x0u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x1u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_2[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0xf8u
    }, {
        0x100du, 0xfu
    }, {
        0x100eu, 0x0u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20038u, 0x11u
    }, {
        0x20039u, 0x11u
    }, {
        0x2003au, 0x0u
    }, {
        0x2003bu, 0x0u
    }, {
        0x2003cu, 0x22u
    }, {
        0x2003du, 0x22u
    }, {
        0x2003eu, 0x0u
    }, {
        0x2003fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_2[] = {
    {
        0xff4u, 0x88u
    }, {
        0xff5u, 0x77u
    }, {
        0xff6u, 0x66u
    }, {
        0xff7u, 0x55u
    }, {
        0xff8u, 0x11u
    }, {
        0xff9u, 0x11u
    }, {
        0xffau, 0x0u
    }, {
        0xffbu, 0x0u
    }, {
        0xffcu, 0x22u
    }, {
        0xffdu, 0x22u
    }, {
        0xffeu, 0x0u
    }, {
        0xfffu, 0x0u
    }, {
        0x50000u, 0x0u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x1u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_3[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0xf8u
    }, {
        0x100du, 0xfu
    }, {
        0x100eu, 0x0u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20038u, 0x11u
    }, {
        0x20039u, 0x11u
    }, {
        0x2003au, 0x0u
    }, {
        0x2003bu, 0x0u
    }, {
        0x2003cu, 0x22u
    }, {
        0x2003du, 0x22u
    }, {
        0x2003eu, 0x0u
    }, {
        0x2003fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_3[] = {
    {
        0xff4u, 0x88u
    }, {
        0xff5u, 0x77u
    }, {
        0xff6u, 0x66u
    }, {
        0xff7u, 0x55u
    }, {
        0xff8u, 0x11u
    }, {
        0xff9u, 0x11u
    }, {
        0xffau, 0x0u
    }, {
        0xffbu, 0x0u
    }, {
        0xffcu, 0x22u
    }, {
        0xffdu, 0x22u
    }, {
        0xffeu, 0x0u
    }, {
        0xfffu, 0x0u
    }, {
        0x50000u, 0x0u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x1u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_4[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20100u, 0x11u
    }, {
        0x20101u, 0x11u
    }, {
        0x20102u, 0x0u
    }, {
        0x20103u, 0x0u
    }, {
        0x20104u, 0x22u
    }, {
        0x20105u, 0x22u
    }, {
        0x20106u, 0x0u
    }, {
        0x20107u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_4[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x0u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x6u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_5[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20100u, 0x11u
    }, {
        0x20101u, 0x11u
    }, {
        0x20102u, 0x0u
    }, {
        0x20103u, 0x0u
    }, {
        0x20104u, 0x22u
    }, {
        0x20105u, 0x22u
    }, {
        0x20106u, 0x0u
    }, {
        0x20107u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_5[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x0u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x6u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_6[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0xf8u
    }, {
        0x100du, 0xfu
    }, {
        0x100eu, 0x0u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20100u, 0x11u
    }, {
        0x20101u, 0x11u
    }, {
        0x20102u, 0x0u
    }, {
        0x20103u, 0x0u
    }, {
        0x20104u, 0x22u
    }, {
        0x20105u, 0x22u
    }, {
        0x20106u, 0x0u
    }, {
        0x20107u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_6[] = {
    {
        0xff8u, 0x11u
    }, {
        0xff9u, 0x11u
    }, {
        0xffau, 0x0u
    }, {
        0xffbu, 0x0u
    }, {
        0xffcu, 0x22u
    }, {
        0xffdu, 0x22u
    }, {
        0xffeu, 0x0u
    }, {
        0xfffu, 0x0u
    }, {
        0x50000u, 0x0u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x6u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_7[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0xf8u
    }, {
        0x100du, 0xfu
    }, {
        0x100eu, 0x0u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20100u, 0x11u
    }, {
        0x20101u, 0x11u
    }, {
        0x20102u, 0x0u
    }, {
        0x20103u, 0x0u
    }, {
        0x20104u, 0x22u
    }, {
        0x20105u, 0x22u
    }, {
        0x20106u, 0x0u
    }, {
        0x20107u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_7[] = {
    {
        0xff8u, 0x11u
    }, {
        0xff9u, 0x11u
    }, {
        0xffau, 0x0u
    }, {
        0xffbu, 0x0u
    }, {
        0xffcu, 0x22u
    }, {
        0xffdu, 0x22u
    }, {
        0xffeu, 0x0u
    }, {
        0xfffu, 0x0u
    }, {
        0x50000u, 0x0u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x6u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_8[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20268u, 0x11u
    }, {
        0x20269u, 0x11u
    }, {
        0x2026au, 0x0u
    }, {
        0x2026bu, 0x0u
    }, {
        0x2026cu, 0x22u
    }, {
        0x2026du, 0x22u
    }, {
        0x2026eu, 0x0u
    }, {
        0x2026fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_8[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x2u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x1u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_9[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20268u, 0x11u
    }, {
        0x20269u, 0x11u
    }, {
        0x2026au, 0x0u
    }, {
        0x2026bu, 0x0u
    }, {
        0x2026cu, 0x22u
    }, {
        0x2026du, 0x22u
    }, {
        0x2026eu, 0x0u
    }, {
        0x2026fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_9[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x2u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x1u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_10[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0xf8u
    }, {
        0x100du, 0xfu
    }, {
        0x100eu, 0x0u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20268u, 0x11u
    }, {
        0x20269u, 0x11u
    }, {
        0x2026au, 0x0u
    }, {
        0x2026bu, 0x0u
    }, {
        0x2026cu, 0x22u
    }, {
        0x2026du, 0x22u
    }, {
        0x2026eu, 0x0u
    }, {
        0x2026fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_10[] = {
    {
        0xff8u, 0x11u
    }, {
        0xff9u, 0x11u
    }, {
        0xffau, 0x0u
    }, {
        0xffbu, 0x0u
    }, {
        0xffcu, 0x22u
    }, {
        0xffdu, 0x22u
    }, {
        0xffeu, 0x0u
    }, {
        0xfffu, 0x0u
    }, {
        0x50000u, 0x2u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x1u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_11[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0xf8u
    }, {
        0x100du, 0xfu
    }, {
        0x100eu, 0x0u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20268u, 0x11u
    }, {
        0x20269u, 0x11u
    }, {
        0x2026au, 0x0u
    }, {
        0x2026bu, 0x0u
    }, {
        0x2026cu, 0x22u
    }, {
        0x2026du, 0x22u
    }, {
        0x2026eu, 0x0u
    }, {
        0x2026fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_11[] = {
    {
        0xff8u, 0x11u
    }, {
        0xff9u, 0x11u
    }, {
        0xffau, 0x0u
    }, {
        0xffbu, 0x0u
    }, {
        0xffcu, 0x22u
    }, {
        0xffdu, 0x22u
    }, {
        0xffeu, 0x0u
    }, {
        0xfffu, 0x0u
    }, {
        0x50000u, 0x2u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x1u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_12[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20088u, 0x11u
    }, {
        0x20089u, 0x11u
    }, {
        0x2008au, 0x0u
    }, {
        0x2008bu, 0x0u
    }, {
        0x2008cu, 0x22u
    }, {
        0x2008du, 0x22u
    }, {
        0x2008eu, 0x0u
    }, {
        0x2008fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_12[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x0u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x3u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_13[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20088u, 0x11u
    }, {
        0x20089u, 0x11u
    }, {
        0x2008au, 0x0u
    }, {
        0x2008bu, 0x0u
    }, {
        0x2008cu, 0x22u
    }, {
        0x2008du, 0x22u
    }, {
        0x2008eu, 0x0u
    }, {
        0x2008fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_13[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x0u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x3u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_14[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0xf8u
    }, {
        0x100du, 0xfu
    }, {
        0x100eu, 0x0u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20088u, 0x11u
    }, {
        0x20089u, 0x11u
    }, {
        0x2008au, 0x0u
    }, {
        0x2008bu, 0x0u
    }, {
        0x2008cu, 0x22u
    }, {
        0x2008du, 0x22u
    }, {
        0x2008eu, 0x0u
    }, {
        0x2008fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_14[] = {
    {
        0xff8u, 0x11u
    }, {
        0xff9u, 0x11u
    }, {
        0xffau, 0x0u
    }, {
        0xffbu, 0x0u
    }, {
        0xffcu, 0x22u
    }, {
        0xffdu, 0x22u
    }, {
        0xffeu, 0x0u
    }, {
        0xfffu, 0x0u
    }, {
        0x50000u, 0x0u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x3u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_15[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0xf8u
    }, {
        0x100du, 0xfu
    }, {
        0x100eu, 0x0u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20088u, 0x11u
    }, {
        0x20089u, 0x11u
    }, {
        0x2008au, 0x0u
    }, {
        0x2008bu, 0x0u
    }, {
        0x2008cu, 0x22u
    }, {
        0x2008du, 0x22u
    }, {
        0x2008eu, 0x0u
    }, {
        0x2008fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_15[] = {
    {
        0xff8u, 0x11u
    }, {
        0xff9u, 0x11u
    }, {
        0xffau, 0x0u
    }, {
        0xffbu, 0x0u
    }, {
        0xffcu, 0x22u
    }, {
        0xffdu, 0x22u
    }, {
        0xffeu, 0x0u
    }, {
        0xfffu, 0x0u
    }, {
        0x50000u, 0x0u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x3u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_16[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x4u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20498u, 0x11u
    }, {
        0x20499u, 0x11u
    }, {
        0x2049au, 0x0u
    }, {
        0x2049bu, 0x0u
    }, {
        0x2049cu, 0x22u
    }, {
        0x2049du, 0x22u
    }, {
        0x2049eu, 0x0u
    }, {
        0x2049fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_16[] = {
    {
        0xff4u, 0x88u
    }, {
        0xff5u, 0x77u
    }, {
        0xff6u, 0x66u
    }, {
        0xff7u, 0x55u
    }, {
        0xff8u, 0x1u
    }, {
        0xff9u, 0xefu
    }, {
        0xffau, 0xcdu
    }, {
        0xffbu, 0xabu
    }, {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x4u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x1u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_17[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x4u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20498u, 0x11u
    }, {
        0x20499u, 0x11u
    }, {
        0x2049au, 0x0u
    }, {
        0x2049bu, 0x0u
    }, {
        0x2049cu, 0x22u
    }, {
        0x2049du, 0x22u
    }, {
        0x2049eu, 0x0u
    }, {
        0x2049fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_17[] = {
    {
        0xff4u, 0x88u
    }, {
        0xff5u, 0x77u
    }, {
        0xff6u, 0x66u
    }, {
        0xff7u, 0x55u
    }, {
        0xff8u, 0x1u
    }, {
        0xff9u, 0xefu
    }, {
        0xffau, 0xcdu
    }, {
        0xffbu, 0xabu
    }, {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x4u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x1u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_18[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x6u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x6u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20790u, 0x11u
    }, {
        0x20791u, 0x11u
    }, {
        0x20792u, 0x0u
    }, {
        0x20793u, 0x0u
    }, {
        0x20794u, 0x22u
    }, {
        0x20795u, 0x22u
    }, {
        0x20796u, 0x0u
    }, {
        0x20797u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_18[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x6u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x6u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_19[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x6u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x6u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20790u, 0x11u
    }, {
        0x20791u, 0x11u
    }, {
        0x20792u, 0x0u
    }, {
        0x20793u, 0x0u
    }, {
        0x20794u, 0x22u
    }, {
        0x20795u, 0x22u
    }, {
        0x20796u, 0x0u
    }, {
        0x20797u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_19[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x6u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x6u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_20[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x2u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20268u, 0x11u
    }, {
        0x20269u, 0x11u
    }, {
        0x2026au, 0x0u
    }, {
        0x2026bu, 0x0u
    }, {
        0x2026cu, 0x22u
    }, {
        0x2026du, 0x22u
    }, {
        0x2026eu, 0x0u
    }, {
        0x2026fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_20[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x2u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x1u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_21[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x2u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x1u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x20268u, 0x11u
    }, {
        0x20269u, 0x11u
    }, {
        0x2026au, 0x0u
    }, {
        0x2026bu, 0x0u
    }, {
        0x2026cu, 0x22u
    }, {
        0x2026du, 0x22u
    }, {
        0x2026eu, 0x0u
    }, {
        0x2026fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_21[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x2u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x1u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_22[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x3u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x4u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x203d0u, 0x11u
    }, {
        0x203d1u, 0x11u
    }, {
        0x203d2u, 0x0u
    }, {
        0x203d3u, 0x0u
    }, {
        0x203d4u, 0x22u
    }, {
        0x203d5u, 0x22u
    }, {
        0x203d6u, 0x0u
    }, {
        0x203d7u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_22[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x3u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x3u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_23[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x0u
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x4u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0x0u
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x5u
    }, {
        0x1013u, 0x0u
    }, {
        0x30000u, 0x3u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x4u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x203d0u, 0x11u
    }, {
        0x203d1u, 0x11u
    }, {
        0x203d2u, 0x0u
    }, {
        0x203d3u, 0x0u
    }, {
        0x203d4u, 0x22u
    }, {
        0x203d5u, 0x22u
    }, {
        0x203d6u, 0x0u
    }, {
        0x203d7u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_23[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x40000u, 0x11u
    }, {
        0x40001u, 0x11u
    }, {
        0x40002u, 0x0u
    }, {
        0x40003u, 0x0u
    }, {
        0x40004u, 0x22u
    }, {
        0x40005u, 0x22u
    }, {
        0x40006u, 0x0u
    }, {
        0x40007u, 0x0u
    }, {
        0x50000u, 0x3u
    }, {
        0x50001u, 0x0u
    }, {
        0x50002u, 0x0u
    }, {
        0x50003u, 0x0u
    }, {
        0x50004u, 0x3u
    }, {
        0x50005u, 0x0u
    }, {
        0x50006u, 0x0u
    }, {
        0x50007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_24[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x0u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_24[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_25[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x0u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_25[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_26[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x20000u, 0x1u
    }, {
        0x20001u, 0x0u
    }, {
        0x20002u, 0x0u
    }, {
        0x20003u, 0x0u
    }, {
        0x20004u, 0x2u
    }, {
        0x20005u, 0x0u
    }, {
        0x20006u, 0x0u
    }, {
        0x20007u, 0x0u
    }, {
        0x30000u, 0x1u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x3u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_26[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_27[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x20000u, 0x1u
    }, {
        0x20001u, 0x0u
    }, {
        0x20002u, 0x0u
    }, {
        0x20003u, 0x0u
    }, {
        0x20004u, 0x2u
    }, {
        0x20005u, 0x0u
    }, {
        0x20006u, 0x0u
    }, {
        0x20007u, 0x0u
    }, {
        0x30000u, 0x1u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x3u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_27[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_28[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x20000u, 0x1u
    }, {
        0x20001u, 0x0u
    }, {
        0x20002u, 0x0u
    }, {
        0x20003u, 0x0u
    }, {
        0x20004u, 0x2u
    }, {
        0x20005u, 0x0u
    }, {
        0x20006u, 0x0u
    }, {
        0x20007u, 0x0u
    }, {
        0x30000u, 0x3u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x2u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_28[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_29[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x20000u, 0x1u
    }, {
        0x20001u, 0x0u
    }, {
        0x20002u, 0x0u
    }, {
        0x20003u, 0x0u
    }, {
        0x20004u, 0x2u
    }, {
        0x20005u, 0x0u
    }, {
        0x20006u, 0x0u
    }, {
        0x20007u, 0x0u
    }, {
        0x30000u, 0x3u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x2u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_29[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_30[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x20000u, 0x1u
    }, {
        0x20001u, 0x0u
    }, {
        0x20002u, 0x0u
    }, {
        0x20003u, 0x0u
    }, {
        0x20004u, 0x2u
    }, {
        0x20005u, 0x0u
    }, {
        0x20006u, 0x0u
    }, {
        0x20007u, 0x0u
    }, {
        0x30000u, 0x3u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x4u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_30[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_31[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x20000u, 0x1u
    }, {
        0x20001u, 0x0u
    }, {
        0x20002u, 0x0u
    }, {
        0x20003u, 0x0u
    }, {
        0x20004u, 0x2u
    }, {
        0x20005u, 0x0u
    }, {
        0x20006u, 0x0u
    }, {
        0x20007u, 0x0u
    }, {
        0x30000u, 0x3u
    }, {
        0x30001u, 0x0u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x4u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_31[] = {
    {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_32[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x7bu
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x0u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x5u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x0u
    }, {
        0x100bu, 0x0u
    }, {
        0x521a1cu, 0x7bu
    }, {
        0x521a1du, 0x0u
    }, {
        0x521a1eu, 0x0u
    }, {
        0x521a1fu, 0x0u
    }, {
        0x521a4cu, 0xffu
    }, {
        0x521a4du, 0xffu
    }, {
        0x521a4eu, 0xffu
    }, {
        0x521a4fu, 0xffu
    }, {
        0x521a7cu, 0xffu
    }, {
        0x521a7du, 0xffu
    }, {
        0x521a7eu, 0xffu
    }, {
        0x521a7fu, 0xffu
    }, {
        0x521aacu, 0xffu
    }, {
        0x521aadu, 0xffu
    }, {
        0x521aaeu, 0xffu
    }, {
        0x521aafu, 0xffu
    }, {
        0x521adcu, 0xffu
    }, {
        0x521addu, 0xffu
    }, {
        0x521adeu, 0xffu
    }, {
        0x521adfu, 0xffu
    }, {
        0x521b0cu, 0xffu
    }, {
        0x521b0du, 0xffu
    }, {
        0x521b0eu, 0xffu
    }, {
        0x521b0fu, 0xffu
    }, {
        0x521b3cu, 0xffu
    }, {
        0x521b3du, 0xffu
    }, {
        0x521b3eu, 0xffu
    }, {
        0x521b3fu, 0xffu
    }, {
        0x521b6cu, 0xffu
    }, {
        0x521b6du, 0xffu
    }, {
        0x521b6eu, 0xffu
    }, {
        0x521b6fu, 0xffu
    }, {
        0x521b9cu, 0xffu
    }, {
        0x521b9du, 0xffu
    }, {
        0x521b9eu, 0xffu
    }, {
        0x521b9fu, 0xffu
    }, {
        0x521bccu, 0xffu
    }, {
        0x521bcdu, 0xffu
    }, {
        0x521bceu, 0xffu
    }, {
        0x521bcfu, 0xffu
    }, {
        0x521bfcu, 0xffu
    }, {
        0x521bfdu, 0xffu
    }, {
        0x521bfeu, 0xffu
    }, {
        0x521bffu, 0xffu
    }, {
        0x521c2cu, 0xffu
    }, {
        0x521c2du, 0xffu
    }, {
        0x521c2eu, 0xffu
    }, {
        0x521c2fu, 0xffu
    }, {
        0x521c5cu, 0xffu
    }, {
        0x521c5du, 0xffu
    }, {
        0x521c5eu, 0xffu
    }, {
        0x521c5fu, 0xffu
    }, {
        0x521c8cu, 0xffu
    }, {
        0x521c8du, 0xffu
    }, {
        0x521c8eu, 0xffu
    }, {
        0x521c8fu, 0xffu
    }, {
        0x521cbcu, 0xffu
    }, {
        0x521cbdu, 0xffu
    }, {
        0x521cbeu, 0xffu
    }, {
        0x521cbfu, 0xffu
    }, {
        0x521cecu, 0xffu
    }, {
        0x521cedu, 0xffu
    }, {
        0x521ceeu, 0xffu
    }, {
        0x521cefu, 0xffu
    }, {
        0x521d1cu, 0xffu
    }, {
        0x521d1du, 0xffu
    }, {
        0x521d1eu, 0xffu
    }, {
        0x521d1fu, 0xffu
    }, {
        0x521d4cu, 0xffu
    }, {
        0x521d4du, 0xffu
    }, {
        0x521d4eu, 0xffu
    }, {
        0x521d4fu, 0xffu
    }, {
        0x521d7cu, 0xffu
    }, {
        0x521d7du, 0xffu
    }, {
        0x521d7eu, 0xffu
    }, {
        0x521d7fu, 0xffu
    }, {
        0x521dacu, 0xffu
    }, {
        0x521dadu, 0xffu
    }, {
        0x521daeu, 0xffu
    }, {
        0x521dafu, 0xffu
    }, {
        0x521ddcu, 0xffu
    }, {
        0x521dddu, 0xffu
    }, {
        0x521ddeu, 0xffu
    }, {
        0x521ddfu, 0xffu
    }, {
        0x521e0cu, 0xffu
    }, {
        0x521e0du, 0xffu
    }, {
        0x521e0eu, 0xffu
    }, {
        0x521e0fu, 0xffu
    }, {
        0x521e3cu, 0xffu
    }, {
        0x521e3du, 0xffu
    }, {
        0x521e3eu, 0xffu
    }, {
        0x521e3fu, 0xffu
    }, {
        0x521e6cu, 0xffu
    }, {
        0x521e6du, 0xffu
    }, {
        0x521e6eu, 0xffu
    }, {
        0x521e6fu, 0xffu
    }, {
        0x521e9cu, 0xffu
    }, {
        0x521e9du, 0xffu
    }, {
        0x521e9eu, 0xffu
    }, {
        0x521e9fu, 0xffu
    }, {
        0x521eccu, 0xffu
    }, {
        0x521ecdu, 0xffu
    }, {
        0x521eceu, 0xffu
    }, {
        0x521ecfu, 0xffu
    }, {
        0x521efcu, 0xffu
    }, {
        0x521efdu, 0xffu
    }, {
        0x521efeu, 0xffu
    }, {
        0x521effu, 0xffu
    }, {
        0x521f2cu, 0xffu
    }, {
        0x521f2du, 0xffu
    }, {
        0x521f2eu, 0xffu
    }, {
        0x521f2fu, 0xffu
    }, {
        0x521f5cu, 0xffu
    }, {
        0x521f5du, 0xffu
    }, {
        0x521f5eu, 0xffu
    }, {
        0x521f5fu, 0xffu
    }, {
        0x521f8cu, 0xffu
    }, {
        0x521f8du, 0xffu
    }, {
        0x521f8eu, 0xffu
    }, {
        0x521f8fu, 0xffu
    }, {
        0x521fbcu, 0xffu
    }, {
        0x521fbdu, 0xffu
    }, {
        0x521fbeu, 0xffu
    }, {
        0x521fbfu, 0xffu
    }, {
        0x521fecu, 0xffu
    }, {
        0x521fedu, 0xffu
    }, {
        0x521feeu, 0xffu
    }, {
        0x521fefu, 0xffu
    }, {
        0x52201cu, 0xffu
    }, {
        0x52201du, 0xffu
    }, {
        0x52201eu, 0xffu
    }, {
        0x52201fu, 0xffu
    }, {
        0x52204cu, 0xffu
    }, {
        0x52204du, 0xffu
    }, {
        0x52204eu, 0xffu
    }, {
        0x52204fu, 0xffu
    }, {
        0x52207cu, 0xffu
    }, {
        0x52207du, 0xffu
    }, {
        0x52207eu, 0xffu
    }, {
        0x52207fu, 0xffu
    }, {
        0x5220acu, 0xffu
    }, {
        0x5220adu, 0xffu
    }, {
        0x5220aeu, 0xffu
    }, {
        0x5220afu, 0xffu
    }, {
        0x5220dcu, 0xffu
    }, {
        0x5220ddu, 0xffu
    }, {
        0x5220deu, 0xffu
    }, {
        0x5220dfu, 0xffu
    }, {
        0x52210cu, 0xffu
    }, {
        0x52210du, 0xffu
    }, {
        0x52210eu, 0xffu
    }, {
        0x52210fu, 0xffu
    }, {
        0x52213cu, 0xffu
    }, {
        0x52213du, 0xffu
    }, {
        0x52213eu, 0xffu
    }, {
        0x52213fu, 0xffu
    }, {
        0x52216cu, 0xffu
    }, {
        0x52216du, 0xffu
    }, {
        0x52216eu, 0xffu
    }, {
        0x52216fu, 0xffu
    }, {
        0x52219cu, 0xffu
    }, {
        0x52219du, 0xffu
    }, {
        0x52219eu, 0xffu
    }, {
        0x52219fu, 0xffu
    }, {
        0x5221ccu, 0xffu
    }, {
        0x5221cdu, 0xffu
    }, {
        0x5221ceu, 0xffu
    }, {
        0x5221cfu, 0xffu
    }, {
        0x5221fcu, 0xffu
    }, {
        0x5221fdu, 0xffu
    }, {
        0x5221feu, 0xffu
    }, {
        0x5221ffu, 0xffu
    }, {
        0x52222cu, 0xffu
    }, {
        0x52222du, 0xffu
    }, {
        0x52222eu, 0xffu
    }, {
        0x52222fu, 0xffu
    }, {
        0x52225cu, 0xffu
    }, {
        0x52225du, 0xffu
    }, {
        0x52225eu, 0xffu
    }, {
        0x52225fu, 0xffu
    }, {
        0x52228cu, 0xffu
    }, {
        0x52228du, 0xffu
    }, {
        0x52228eu, 0xffu
    }, {
        0x52228fu, 0xffu
    }, {
        0x5222bcu, 0xffu
    }, {
        0x5222bdu, 0xffu
    }, {
        0x5222beu, 0xffu
    }, {
        0x5222bfu, 0xffu
    }, {
        0x5222ecu, 0xffu
    }, {
        0x5222edu, 0xffu
    }, {
        0x5222eeu, 0xffu
    }, {
        0x5222efu, 0xffu
    }, {
        0x52231cu, 0xffu
    }, {
        0x52231du, 0xffu
    }, {
        0x52231eu, 0xffu
    }, {
        0x52231fu, 0xffu
    }, {
        0x52234cu, 0xffu
    }, {
        0x52234du, 0xffu
    }, {
        0x52234eu, 0xffu
    }, {
        0x52234fu, 0xffu
    }, {
        0x52237cu, 0xffu
    }, {
        0x52237du, 0xffu
    }, {
        0x52237eu, 0xffu
    }, {
        0x52237fu, 0xffu
    }, {
        0x5223acu, 0xffu
    }, {
        0x5223adu, 0xffu
    }, {
        0x5223aeu, 0xffu
    }, {
        0x5223afu, 0xffu
    }, {
        0x5223dcu, 0xffu
    }, {
        0x5223ddu, 0xffu
    }, {
        0x5223deu, 0xffu
    }, {
        0x5223dfu, 0xffu
    }, {
        0x52240cu, 0xffu
    }, {
        0x52240du, 0xffu
    }, {
        0x52240eu, 0xffu
    }, {
        0x52240fu, 0xffu
    }, {
        0x52243cu, 0xffu
    }, {
        0x52243du, 0xffu
    }, {
        0x52243eu, 0xffu
    }, {
        0x52243fu, 0xffu
    }, {
        0x52246cu, 0xffu
    }, {
        0x52246du, 0xffu
    }, {
        0x52246eu, 0xffu
    }, {
        0x52246fu, 0xffu
    }, {
        0x52249cu, 0xffu
    }, {
        0x52249du, 0xffu
    }, {
        0x52249eu, 0xffu
    }, {
        0x52249fu, 0xffu
    }, {
        0x5224ccu, 0xffu
    }, {
        0x5224cdu, 0xffu
    }, {
        0x5224ceu, 0xffu
    }, {
        0x5224cfu, 0xffu
    }, {
        0x5224fcu, 0xffu
    }, {
        0x5224fdu, 0xffu
    }, {
        0x5224feu, 0xffu
    }, {
        0x5224ffu, 0xffu
    }, {
        0x52252cu, 0xffu
    }, {
        0x52252du, 0xffu
    }, {
        0x52252eu, 0xffu
    }, {
        0x52252fu, 0xffu
    }, {
        0x52255cu, 0xffu
    }, {
        0x52255du, 0xffu
    }, {
        0x52255eu, 0xffu
    }, {
        0x52255fu, 0xffu
    }, {
        0x52258cu, 0xffu
    }, {
        0x52258du, 0xffu
    }, {
        0x52258eu, 0xffu
    }, {
        0x52258fu, 0xffu
    }, {
        0x5225bcu, 0xffu
    }, {
        0x5225bdu, 0xffu
    }, {
        0x5225beu, 0xffu
    }, {
        0x5225bfu, 0xffu
    }, {
        0x5225ecu, 0xffu
    }, {
        0x5225edu, 0xffu
    }, {
        0x5225eeu, 0xffu
    }, {
        0x5225efu, 0xffu
    }, {
        0x52261cu, 0xffu
    }, {
        0x52261du, 0xffu
    }, {
        0x52261eu, 0xffu
    }, {
        0x52261fu, 0xffu
    }, {
        0x52264cu, 0xffu
    }, {
        0x52264du, 0xffu
    }, {
        0x52264eu, 0xffu
    }, {
        0x52264fu, 0xffu
    }, {
        0x52267cu, 0xffu
    }, {
        0x52267du, 0xffu
    }, {
        0x52267eu, 0xffu
    }, {
        0x52267fu, 0xffu
    }, {
        0x5226acu, 0xffu
    }, {
        0x5226adu, 0xffu
    }, {
        0x5226aeu, 0xffu
    }, {
        0x5226afu, 0xffu
    }, {
        0x5226dcu, 0xffu
    }, {
        0x5226ddu, 0xffu
    }, {
        0x5226deu, 0xffu
    }, {
        0x5226dfu, 0xffu
    }, {
        0x52270cu, 0xffu
    }, {
        0x52270du, 0xffu
    }, {
        0x52270eu, 0xffu
    }, {
        0x52270fu, 0xffu
    }, {
        0x52273cu, 0xffu
    }, {
        0x52273du, 0xffu
    }, {
        0x52273eu, 0xffu
    }, {
        0x52273fu, 0xffu
    }, {
        0x52276cu, 0xffu
    }, {
        0x52276du, 0xffu
    }, {
        0x52276eu, 0xffu
    }, {
        0x52276fu, 0xffu
    }, {
        0x52279cu, 0xffu
    }, {
        0x52279du, 0xffu
    }, {
        0x52279eu, 0xffu
    }, {
        0x52279fu, 0xffu
    }, {
        0x5227ccu, 0xffu
    }, {
        0x5227cdu, 0xffu
    }, {
        0x5227ceu, 0xffu
    }, {
        0x5227cfu, 0xffu
    }, {
        0x5227fcu, 0xffu
    }, {
        0x5227fdu, 0xffu
    }, {
        0x5227feu, 0xffu
    }, {
        0x5227ffu, 0xffu
    }, {
        0x52282cu, 0xffu
    }, {
        0x52282du, 0xffu
    }, {
        0x52282eu, 0xffu
    }, {
        0x52282fu, 0xffu
    }, {
        0x52285cu, 0xffu
    }, {
        0x52285du, 0xffu
    }, {
        0x52285eu, 0xffu
    }, {
        0x52285fu, 0xffu
    }, {
        0x52288cu, 0xffu
    }, {
        0x52288du, 0xffu
    }, {
        0x52288eu, 0xffu
    }, {
        0x52288fu, 0xffu
    }, {
        0x5228bcu, 0xffu
    }, {
        0x5228bdu, 0xffu
    }, {
        0x5228beu, 0xffu
    }, {
        0x5228bfu, 0xffu
    }, {
        0x5228ecu, 0xffu
    }, {
        0x5228edu, 0xffu
    }, {
        0x5228eeu, 0xffu
    }, {
        0x5228efu, 0xffu
    }, {
        0x52291cu, 0xffu
    }, {
        0x52291du, 0xffu
    }, {
        0x52291eu, 0xffu
    }, {
        0x52291fu, 0xffu
    }, {
        0x52294cu, 0xffu
    }, {
        0x52294du, 0xffu
    }, {
        0x52294eu, 0xffu
    }, {
        0x52294fu, 0xffu
    }, {
        0x52297cu, 0xffu
    }, {
        0x52297du, 0xffu
    }, {
        0x52297eu, 0xffu
    }, {
        0x52297fu, 0xffu
    }, {
        0x5229acu, 0xffu
    }, {
        0x5229adu, 0xffu
    }, {
        0x5229aeu, 0xffu
    }, {
        0x5229afu, 0xffu
    }, {
        0x5229dcu, 0xffu
    }, {
        0x5229ddu, 0xffu
    }, {
        0x5229deu, 0xffu
    }, {
        0x5229dfu, 0xffu
    }, {
        0x522a0cu, 0xffu
    }, {
        0x522a0du, 0xffu
    }, {
        0x522a0eu, 0xffu
    }, {
        0x522a0fu, 0xffu
    }, {
        0x522a3cu, 0xffu
    }, {
        0x522a3du, 0xffu
    }, {
        0x522a3eu, 0xffu
    }, {
        0x522a3fu, 0xffu
    }, {
        0x522a6cu, 0xffu
    }, {
        0x522a6du, 0xffu
    }, {
        0x522a6eu, 0xffu
    }, {
        0x522a6fu, 0xffu
    }, {
        0x522a9cu, 0xffu
    }, {
        0x522a9du, 0xffu
    }, {
        0x522a9eu, 0xffu
    }, {
        0x522a9fu, 0xffu
    }, {
        0x522accu, 0xffu
    }, {
        0x522acdu, 0xffu
    }, {
        0x522aceu, 0xffu
    }, {
        0x522acfu, 0xffu
    }, {
        0x522afcu, 0xffu
    }, {
        0x522afdu, 0xffu
    }, {
        0x522afeu, 0xffu
    }, {
        0x522affu, 0xffu
    }, {
        0x522b2cu, 0xffu
    }, {
        0x522b2du, 0xffu
    }, {
        0x522b2eu, 0xffu
    }, {
        0x522b2fu, 0xffu
    }, {
        0x522b5cu, 0xffu
    }, {
        0x522b5du, 0xffu
    }, {
        0x522b5eu, 0xffu
    }, {
        0x522b5fu, 0xffu
    }, {
        0x522b8cu, 0xffu
    }, {
        0x522b8du, 0xffu
    }, {
        0x522b8eu, 0xffu
    }, {
        0x522b8fu, 0xffu
    }, {
        0x522bbcu, 0xffu
    }, {
        0x522bbdu, 0xffu
    }, {
        0x522bbeu, 0xffu
    }, {
        0x522bbfu, 0xffu
    }, {
        0x522becu, 0xffu
    }, {
        0x522bedu, 0xffu
    }, {
        0x522beeu, 0xffu
    }, {
        0x522befu, 0xffu
    }, {
        0x522c1cu, 0xffu
    }, {
        0x522c1du, 0xffu
    }, {
        0x522c1eu, 0xffu
    }, {
        0x522c1fu, 0xffu
    }, {
        0x522c4cu, 0xffu
    }, {
        0x522c4du, 0xffu
    }, {
        0x522c4eu, 0xffu
    }, {
        0x522c4fu, 0xffu
    }, {
        0x522c7cu, 0xffu
    }, {
        0x522c7du, 0xffu
    }, {
        0x522c7eu, 0xffu
    }, {
        0x522c7fu, 0xffu
    }, {
        0x522cacu, 0xffu
    }, {
        0x522cadu, 0xffu
    }, {
        0x522caeu, 0xffu
    }, {
        0x522cafu, 0xffu
    }, {
        0x522cdcu, 0xffu
    }, {
        0x522cddu, 0xffu
    }, {
        0x522cdeu, 0xffu
    }, {
        0x522cdfu, 0xffu
    }, {
        0x522d0cu, 0xffu
    }, {
        0x522d0du, 0xffu
    }, {
        0x522d0eu, 0xffu
    }, {
        0x522d0fu, 0xffu
    }, {
        0x522d3cu, 0xffu
    }, {
        0x522d3du, 0xffu
    }, {
        0x522d3eu, 0xffu
    }, {
        0x522d3fu, 0xffu
    }, {
        0x522d6cu, 0xffu
    }, {
        0x522d6du, 0xffu
    }, {
        0x522d6eu, 0xffu
    }, {
        0x522d6fu, 0xffu
    }, {
        0x522d9cu, 0xffu
    }, {
        0x522d9du, 0xffu
    }, {
        0x522d9eu, 0xffu
    }, {
        0x522d9fu, 0xffu
    }, {
        0x522dccu, 0xffu
    }, {
        0x522dcdu, 0xffu
    }, {
        0x522dceu, 0xffu
    }, {
        0x522dcfu, 0xffu
    }, {
        0x522dfcu, 0xffu
    }, {
        0x522dfdu, 0xffu
    }, {
        0x522dfeu, 0xffu
    }, {
        0x522dffu, 0xffu
    }, {
        0x522e2cu, 0xffu
    }, {
        0x522e2du, 0xffu
    }, {
        0x522e2eu, 0xffu
    }, {
        0x522e2fu, 0xffu
    }, {
        0x522e5cu, 0xffu
    }, {
        0x522e5du, 0xffu
    }, {
        0x522e5eu, 0xffu
    }, {
        0x522e5fu, 0xffu
    }, {
        0x522e8cu, 0xffu
    }, {
        0x522e8du, 0xffu
    }, {
        0x522e8eu, 0xffu
    }, {
        0x522e8fu, 0xffu
    }, {
        0x522ebcu, 0xffu
    }, {
        0x522ebdu, 0xffu
    }, {
        0x522ebeu, 0xffu
    }, {
        0x522ebfu, 0xffu
    }, {
        0x522eecu, 0xffu
    }, {
        0x522eedu, 0xffu
    }, {
        0x522eeeu, 0xffu
    }, {
        0x522eefu, 0xffu
    }, {
        0x522f1cu, 0xffu
    }, {
        0x522f1du, 0xffu
    }, {
        0x522f1eu, 0xffu
    }, {
        0x522f1fu, 0xffu
    }, {
        0x522f4cu, 0xffu
    }, {
        0x522f4du, 0xffu
    }, {
        0x522f4eu, 0xffu
    }, {
        0x522f4fu, 0xffu
    }, {
        0x522f7cu, 0xffu
    }, {
        0x522f7du, 0xffu
    }, {
        0x522f7eu, 0xffu
    }, {
        0x522f7fu, 0xffu
    }, {
        0x522facu, 0xffu
    }, {
        0x522fadu, 0xffu
    }, {
        0x522faeu, 0xffu
    }, {
        0x522fafu, 0xffu
    }, {
        0x522fdcu, 0xffu
    }, {
        0x522fddu, 0xffu
    }, {
        0x522fdeu, 0xffu
    }, {
        0x522fdfu, 0xffu
    }, {
        0x52300cu, 0xffu
    }, {
        0x52300du, 0xffu
    }, {
        0x52300eu, 0xffu
    }, {
        0x52300fu, 0xffu
    }, {
        0x52303cu, 0xffu
    }, {
        0x52303du, 0xffu
    }, {
        0x52303eu, 0xffu
    }, {
        0x52303fu, 0xffu
    }, {
        0x52306cu, 0xffu
    }, {
        0x52306du, 0xffu
    }, {
        0x52306eu, 0xffu
    }, {
        0x52306fu, 0xffu
    }, {
        0x52309cu, 0xffu
    }, {
        0x52309du, 0xffu
    }, {
        0x52309eu, 0xffu
    }, {
        0x52309fu, 0xffu
    }, {
        0x5230ccu, 0xffu
    }, {
        0x5230cdu, 0xffu
    }, {
        0x5230ceu, 0xffu
    }, {
        0x5230cfu, 0xffu
    }, {
        0x5230fcu, 0xffu
    }, {
        0x5230fdu, 0xffu
    }, {
        0x5230feu, 0xffu
    }, {
        0x5230ffu, 0xffu
    }, {
        0x52312cu, 0xffu
    }, {
        0x52312du, 0xffu
    }, {
        0x52312eu, 0xffu
    }, {
        0x52312fu, 0xffu
    }, {
        0x52315cu, 0xffu
    }, {
        0x52315du, 0xffu
    }, {
        0x52315eu, 0xffu
    }, {
        0x52315fu, 0xffu
    }, {
        0x52318cu, 0xffu
    }, {
        0x52318du, 0xffu
    }, {
        0x52318eu, 0xffu
    }, {
        0x52318fu, 0xffu
    }, {
        0x5231bcu, 0xffu
    }, {
        0x5231bdu, 0xffu
    }, {
        0x5231beu, 0xffu
    }, {
        0x5231bfu, 0xffu
    }, {
        0x5231ecu, 0xffu
    }, {
        0x5231edu, 0xffu
    }, {
        0x5231eeu, 0xffu
    }, {
        0x5231efu, 0xffu
    }, {
        0x52321cu, 0xffu
    }, {
        0x52321du, 0xffu
    }, {
        0x52321eu, 0xffu
    }, {
        0x52321fu, 0xffu
    }, {
        0x52324cu, 0xffu
    }, {
        0x52324du, 0xffu
    }, {
        0x52324eu, 0xffu
    }, {
        0x52324fu, 0xffu
    }, {
        0x52327cu, 0xffu
    }, {
        0x52327du, 0xffu
    }, {
        0x52327eu, 0xffu
    }, {
        0x52327fu, 0xffu
    }, {
        0x5232acu, 0xffu
    }, {
        0x5232adu, 0xffu
    }, {
        0x5232aeu, 0xffu
    }, {
        0x5232afu, 0xffu
    }, {
        0x5232dcu, 0xffu
    }, {
        0x5232ddu, 0xffu
    }, {
        0x5232deu, 0xffu
    }, {
        0x5232dfu, 0xffu
    }, {
        0x52330cu, 0xffu
    }, {
        0x52330du, 0xffu
    }, {
        0x52330eu, 0xffu
    }, {
        0x52330fu, 0xffu
    }, {
        0x52333cu, 0xffu
    }, {
        0x52333du, 0xffu
    }, {
        0x52333eu, 0xffu
    }, {
        0x52333fu, 0xffu
    }, {
        0x52336cu, 0xffu
    }, {
        0x52336du, 0xffu
    }, {
        0x52336eu, 0xffu
    }, {
        0x52336fu, 0xffu
    }, {
        0x52339cu, 0xffu
    }, {
        0x52339du, 0xffu
    }, {
        0x52339eu, 0xffu
    }, {
        0x52339fu, 0xffu
    }, {
        0x5233ccu, 0xffu
    }, {
        0x5233cdu, 0xffu
    }, {
        0x5233ceu, 0xffu
    }, {
        0x5233cfu, 0xffu
    }, {
        0x5233fcu, 0xffu
    }, {
        0x5233fdu, 0xffu
    }, {
        0x5233feu, 0xffu
    }, {
        0x5233ffu, 0xffu
    }, {
        0x52342cu, 0xffu
    }, {
        0x52342du, 0xffu
    }, {
        0x52342eu, 0xffu
    }, {
        0x52342fu, 0xffu
    }, {
        0x52345cu, 0xffu
    }, {
        0x52345du, 0xffu
    }, {
        0x52345eu, 0xffu
    }, {
        0x52345fu, 0xffu
    }, {
        0x52348cu, 0xffu
    }, {
        0x52348du, 0xffu
    }, {
        0x52348eu, 0xffu
    }, {
        0x52348fu, 0xffu
    }, {
        0x5234bcu, 0xffu
    }, {
        0x5234bdu, 0xffu
    }, {
        0x5234beu, 0xffu
    }, {
        0x5234bfu, 0xffu
    }, {
        0x5234ecu, 0xffu
    }, {
        0x5234edu, 0xffu
    }, {
        0x5234eeu, 0xffu
    }, {
        0x5234efu, 0xffu
    }, {
        0x52351cu, 0xffu
    }, {
        0x52351du, 0xffu
    }, {
        0x52351eu, 0xffu
    }, {
        0x52351fu, 0xffu
    }, {
        0x52354cu, 0xffu
    }, {
        0x52354du, 0xffu
    }, {
        0x52354eu, 0xffu
    }, {
        0x52354fu, 0xffu
    }, {
        0x52357cu, 0xffu
    }, {
        0x52357du, 0xffu
    }, {
        0x52357eu, 0xffu
    }, {
        0x52357fu, 0xffu
    }, {
        0x5235acu, 0xffu
    }, {
        0x5235adu, 0xffu
    }, {
        0x5235aeu, 0xffu
    }, {
        0x5235afu, 0xffu
    }, {
        0x5235dcu, 0xffu
    }, {
        0x5235ddu, 0xffu
    }, {
        0x5235deu, 0xffu
    }, {
        0x5235dfu, 0xffu
    }, {
        0x52360cu, 0xffu
    }, {
        0x52360du, 0xffu
    }, {
        0x52360eu, 0xffu
    }, {
        0x52360fu, 0xffu
    }, {
        0x521a19u, 0x5u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_32[] = {
    {
        0xff4u, 0x88u
    }, {
        0xff5u, 0x77u
    }, {
        0xff6u, 0x66u
    }, {
        0xff7u, 0x55u
    }, {
        0xff8u, 0x77u
    }, {
        0xff9u, 0x66u
    }, {
        0xffau, 0x55u
    }, {
        0xffbu, 0x44u
    }, {
        0xffcu, 0x1u
    }, {
        0xffdu, 0xefu
    }, {
        0xffeu, 0xcdu
    }, {
        0xfffu, 0xabu
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_33[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x7bu
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x0u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x5u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x0u
    }, {
        0x100bu, 0x0u
    }, {
        0x521a1cu, 0x7bu
    }, {
        0x521a1du, 0x0u
    }, {
        0x521a1eu, 0x0u
    }, {
        0x521a1fu, 0x0u
    }, {
        0x521a4cu, 0xffu
    }, {
        0x521a4du, 0xffu
    }, {
        0x521a4eu, 0xffu
    }, {
        0x521a4fu, 0xffu
    }, {
        0x521a7cu, 0xffu
    }, {
        0x521a7du, 0xffu
    }, {
        0x521a7eu, 0xffu
    }, {
        0x521a7fu, 0xffu
    }, {
        0x521aacu, 0xffu
    }, {
        0x521aadu, 0xffu
    }, {
        0x521aaeu, 0xffu
    }, {
        0x521aafu, 0xffu
    }, {
        0x521adcu, 0xffu
    }, {
        0x521addu, 0xffu
    }, {
        0x521adeu, 0xffu
    }, {
        0x521adfu, 0xffu
    }, {
        0x521b0cu, 0xffu
    }, {
        0x521b0du, 0xffu
    }, {
        0x521b0eu, 0xffu
    }, {
        0x521b0fu, 0xffu
    }, {
        0x521b3cu, 0xffu
    }, {
        0x521b3du, 0xffu
    }, {
        0x521b3eu, 0xffu
    }, {
        0x521b3fu, 0xffu
    }, {
        0x521b6cu, 0xffu
    }, {
        0x521b6du, 0xffu
    }, {
        0x521b6eu, 0xffu
    }, {
        0x521b6fu, 0xffu
    }, {
        0x521b9cu, 0xffu
    }, {
        0x521b9du, 0xffu
    }, {
        0x521b9eu, 0xffu
    }, {
        0x521b9fu, 0xffu
    }, {
        0x521bccu, 0xffu
    }, {
        0x521bcdu, 0xffu
    }, {
        0x521bceu, 0xffu
    }, {
        0x521bcfu, 0xffu
    }, {
        0x521bfcu, 0xffu
    }, {
        0x521bfdu, 0xffu
    }, {
        0x521bfeu, 0xffu
    }, {
        0x521bffu, 0xffu
    }, {
        0x521c2cu, 0xffu
    }, {
        0x521c2du, 0xffu
    }, {
        0x521c2eu, 0xffu
    }, {
        0x521c2fu, 0xffu
    }, {
        0x521c5cu, 0xffu
    }, {
        0x521c5du, 0xffu
    }, {
        0x521c5eu, 0xffu
    }, {
        0x521c5fu, 0xffu
    }, {
        0x521c8cu, 0xffu
    }, {
        0x521c8du, 0xffu
    }, {
        0x521c8eu, 0xffu
    }, {
        0x521c8fu, 0xffu
    }, {
        0x521cbcu, 0xffu
    }, {
        0x521cbdu, 0xffu
    }, {
        0x521cbeu, 0xffu
    }, {
        0x521cbfu, 0xffu
    }, {
        0x521cecu, 0xffu
    }, {
        0x521cedu, 0xffu
    }, {
        0x521ceeu, 0xffu
    }, {
        0x521cefu, 0xffu
    }, {
        0x521d1cu, 0xffu
    }, {
        0x521d1du, 0xffu
    }, {
        0x521d1eu, 0xffu
    }, {
        0x521d1fu, 0xffu
    }, {
        0x521d4cu, 0xffu
    }, {
        0x521d4du, 0xffu
    }, {
        0x521d4eu, 0xffu
    }, {
        0x521d4fu, 0xffu
    }, {
        0x521d7cu, 0xffu
    }, {
        0x521d7du, 0xffu
    }, {
        0x521d7eu, 0xffu
    }, {
        0x521d7fu, 0xffu
    }, {
        0x521dacu, 0xffu
    }, {
        0x521dadu, 0xffu
    }, {
        0x521daeu, 0xffu
    }, {
        0x521dafu, 0xffu
    }, {
        0x521ddcu, 0xffu
    }, {
        0x521dddu, 0xffu
    }, {
        0x521ddeu, 0xffu
    }, {
        0x521ddfu, 0xffu
    }, {
        0x521e0cu, 0xffu
    }, {
        0x521e0du, 0xffu
    }, {
        0x521e0eu, 0xffu
    }, {
        0x521e0fu, 0xffu
    }, {
        0x521e3cu, 0xffu
    }, {
        0x521e3du, 0xffu
    }, {
        0x521e3eu, 0xffu
    }, {
        0x521e3fu, 0xffu
    }, {
        0x521e6cu, 0xffu
    }, {
        0x521e6du, 0xffu
    }, {
        0x521e6eu, 0xffu
    }, {
        0x521e6fu, 0xffu
    }, {
        0x521e9cu, 0xffu
    }, {
        0x521e9du, 0xffu
    }, {
        0x521e9eu, 0xffu
    }, {
        0x521e9fu, 0xffu
    }, {
        0x521eccu, 0xffu
    }, {
        0x521ecdu, 0xffu
    }, {
        0x521eceu, 0xffu
    }, {
        0x521ecfu, 0xffu
    }, {
        0x521efcu, 0xffu
    }, {
        0x521efdu, 0xffu
    }, {
        0x521efeu, 0xffu
    }, {
        0x521effu, 0xffu
    }, {
        0x521f2cu, 0xffu
    }, {
        0x521f2du, 0xffu
    }, {
        0x521f2eu, 0xffu
    }, {
        0x521f2fu, 0xffu
    }, {
        0x521f5cu, 0xffu
    }, {
        0x521f5du, 0xffu
    }, {
        0x521f5eu, 0xffu
    }, {
        0x521f5fu, 0xffu
    }, {
        0x521f8cu, 0xffu
    }, {
        0x521f8du, 0xffu
    }, {
        0x521f8eu, 0xffu
    }, {
        0x521f8fu, 0xffu
    }, {
        0x521fbcu, 0xffu
    }, {
        0x521fbdu, 0xffu
    }, {
        0x521fbeu, 0xffu
    }, {
        0x521fbfu, 0xffu
    }, {
        0x521fecu, 0xffu
    }, {
        0x521fedu, 0xffu
    }, {
        0x521feeu, 0xffu
    }, {
        0x521fefu, 0xffu
    }, {
        0x52201cu, 0xffu
    }, {
        0x52201du, 0xffu
    }, {
        0x52201eu, 0xffu
    }, {
        0x52201fu, 0xffu
    }, {
        0x52204cu, 0xffu
    }, {
        0x52204du, 0xffu
    }, {
        0x52204eu, 0xffu
    }, {
        0x52204fu, 0xffu
    }, {
        0x52207cu, 0xffu
    }, {
        0x52207du, 0xffu
    }, {
        0x52207eu, 0xffu
    }, {
        0x52207fu, 0xffu
    }, {
        0x5220acu, 0xffu
    }, {
        0x5220adu, 0xffu
    }, {
        0x5220aeu, 0xffu
    }, {
        0x5220afu, 0xffu
    }, {
        0x5220dcu, 0xffu
    }, {
        0x5220ddu, 0xffu
    }, {
        0x5220deu, 0xffu
    }, {
        0x5220dfu, 0xffu
    }, {
        0x52210cu, 0xffu
    }, {
        0x52210du, 0xffu
    }, {
        0x52210eu, 0xffu
    }, {
        0x52210fu, 0xffu
    }, {
        0x52213cu, 0xffu
    }, {
        0x52213du, 0xffu
    }, {
        0x52213eu, 0xffu
    }, {
        0x52213fu, 0xffu
    }, {
        0x52216cu, 0xffu
    }, {
        0x52216du, 0xffu
    }, {
        0x52216eu, 0xffu
    }, {
        0x52216fu, 0xffu
    }, {
        0x52219cu, 0xffu
    }, {
        0x52219du, 0xffu
    }, {
        0x52219eu, 0xffu
    }, {
        0x52219fu, 0xffu
    }, {
        0x5221ccu, 0xffu
    }, {
        0x5221cdu, 0xffu
    }, {
        0x5221ceu, 0xffu
    }, {
        0x5221cfu, 0xffu
    }, {
        0x5221fcu, 0xffu
    }, {
        0x5221fdu, 0xffu
    }, {
        0x5221feu, 0xffu
    }, {
        0x5221ffu, 0xffu
    }, {
        0x52222cu, 0xffu
    }, {
        0x52222du, 0xffu
    }, {
        0x52222eu, 0xffu
    }, {
        0x52222fu, 0xffu
    }, {
        0x52225cu, 0xffu
    }, {
        0x52225du, 0xffu
    }, {
        0x52225eu, 0xffu
    }, {
        0x52225fu, 0xffu
    }, {
        0x52228cu, 0xffu
    }, {
        0x52228du, 0xffu
    }, {
        0x52228eu, 0xffu
    }, {
        0x52228fu, 0xffu
    }, {
        0x5222bcu, 0xffu
    }, {
        0x5222bdu, 0xffu
    }, {
        0x5222beu, 0xffu
    }, {
        0x5222bfu, 0xffu
    }, {
        0x5222ecu, 0xffu
    }, {
        0x5222edu, 0xffu
    }, {
        0x5222eeu, 0xffu
    }, {
        0x5222efu, 0xffu
    }, {
        0x52231cu, 0xffu
    }, {
        0x52231du, 0xffu
    }, {
        0x52231eu, 0xffu
    }, {
        0x52231fu, 0xffu
    }, {
        0x52234cu, 0xffu
    }, {
        0x52234du, 0xffu
    }, {
        0x52234eu, 0xffu
    }, {
        0x52234fu, 0xffu
    }, {
        0x52237cu, 0xffu
    }, {
        0x52237du, 0xffu
    }, {
        0x52237eu, 0xffu
    }, {
        0x52237fu, 0xffu
    }, {
        0x5223acu, 0xffu
    }, {
        0x5223adu, 0xffu
    }, {
        0x5223aeu, 0xffu
    }, {
        0x5223afu, 0xffu
    }, {
        0x5223dcu, 0xffu
    }, {
        0x5223ddu, 0xffu
    }, {
        0x5223deu, 0xffu
    }, {
        0x5223dfu, 0xffu
    }, {
        0x52240cu, 0xffu
    }, {
        0x52240du, 0xffu
    }, {
        0x52240eu, 0xffu
    }, {
        0x52240fu, 0xffu
    }, {
        0x52243cu, 0xffu
    }, {
        0x52243du, 0xffu
    }, {
        0x52243eu, 0xffu
    }, {
        0x52243fu, 0xffu
    }, {
        0x52246cu, 0xffu
    }, {
        0x52246du, 0xffu
    }, {
        0x52246eu, 0xffu
    }, {
        0x52246fu, 0xffu
    }, {
        0x52249cu, 0xffu
    }, {
        0x52249du, 0xffu
    }, {
        0x52249eu, 0xffu
    }, {
        0x52249fu, 0xffu
    }, {
        0x5224ccu, 0xffu
    }, {
        0x5224cdu, 0xffu
    }, {
        0x5224ceu, 0xffu
    }, {
        0x5224cfu, 0xffu
    }, {
        0x5224fcu, 0xffu
    }, {
        0x5224fdu, 0xffu
    }, {
        0x5224feu, 0xffu
    }, {
        0x5224ffu, 0xffu
    }, {
        0x52252cu, 0xffu
    }, {
        0x52252du, 0xffu
    }, {
        0x52252eu, 0xffu
    }, {
        0x52252fu, 0xffu
    }, {
        0x52255cu, 0xffu
    }, {
        0x52255du, 0xffu
    }, {
        0x52255eu, 0xffu
    }, {
        0x52255fu, 0xffu
    }, {
        0x52258cu, 0xffu
    }, {
        0x52258du, 0xffu
    }, {
        0x52258eu, 0xffu
    }, {
        0x52258fu, 0xffu
    }, {
        0x5225bcu, 0xffu
    }, {
        0x5225bdu, 0xffu
    }, {
        0x5225beu, 0xffu
    }, {
        0x5225bfu, 0xffu
    }, {
        0x5225ecu, 0xffu
    }, {
        0x5225edu, 0xffu
    }, {
        0x5225eeu, 0xffu
    }, {
        0x5225efu, 0xffu
    }, {
        0x52261cu, 0xffu
    }, {
        0x52261du, 0xffu
    }, {
        0x52261eu, 0xffu
    }, {
        0x52261fu, 0xffu
    }, {
        0x52264cu, 0xffu
    }, {
        0x52264du, 0xffu
    }, {
        0x52264eu, 0xffu
    }, {
        0x52264fu, 0xffu
    }, {
        0x52267cu, 0xffu
    }, {
        0x52267du, 0xffu
    }, {
        0x52267eu, 0xffu
    }, {
        0x52267fu, 0xffu
    }, {
        0x5226acu, 0xffu
    }, {
        0x5226adu, 0xffu
    }, {
        0x5226aeu, 0xffu
    }, {
        0x5226afu, 0xffu
    }, {
        0x5226dcu, 0xffu
    }, {
        0x5226ddu, 0xffu
    }, {
        0x5226deu, 0xffu
    }, {
        0x5226dfu, 0xffu
    }, {
        0x52270cu, 0xffu
    }, {
        0x52270du, 0xffu
    }, {
        0x52270eu, 0xffu
    }, {
        0x52270fu, 0xffu
    }, {
        0x52273cu, 0xffu
    }, {
        0x52273du, 0xffu
    }, {
        0x52273eu, 0xffu
    }, {
        0x52273fu, 0xffu
    }, {
        0x52276cu, 0xffu
    }, {
        0x52276du, 0xffu
    }, {
        0x52276eu, 0xffu
    }, {
        0x52276fu, 0xffu
    }, {
        0x52279cu, 0xffu
    }, {
        0x52279du, 0xffu
    }, {
        0x52279eu, 0xffu
    }, {
        0x52279fu, 0xffu
    }, {
        0x5227ccu, 0xffu
    }, {
        0x5227cdu, 0xffu
    }, {
        0x5227ceu, 0xffu
    }, {
        0x5227cfu, 0xffu
    }, {
        0x5227fcu, 0xffu
    }, {
        0x5227fdu, 0xffu
    }, {
        0x5227feu, 0xffu
    }, {
        0x5227ffu, 0xffu
    }, {
        0x52282cu, 0xffu
    }, {
        0x52282du, 0xffu
    }, {
        0x52282eu, 0xffu
    }, {
        0x52282fu, 0xffu
    }, {
        0x52285cu, 0xffu
    }, {
        0x52285du, 0xffu
    }, {
        0x52285eu, 0xffu
    }, {
        0x52285fu, 0xffu
    }, {
        0x52288cu, 0xffu
    }, {
        0x52288du, 0xffu
    }, {
        0x52288eu, 0xffu
    }, {
        0x52288fu, 0xffu
    }, {
        0x5228bcu, 0xffu
    }, {
        0x5228bdu, 0xffu
    }, {
        0x5228beu, 0xffu
    }, {
        0x5228bfu, 0xffu
    }, {
        0x5228ecu, 0xffu
    }, {
        0x5228edu, 0xffu
    }, {
        0x5228eeu, 0xffu
    }, {
        0x5228efu, 0xffu
    }, {
        0x52291cu, 0xffu
    }, {
        0x52291du, 0xffu
    }, {
        0x52291eu, 0xffu
    }, {
        0x52291fu, 0xffu
    }, {
        0x52294cu, 0xffu
    }, {
        0x52294du, 0xffu
    }, {
        0x52294eu, 0xffu
    }, {
        0x52294fu, 0xffu
    }, {
        0x52297cu, 0xffu
    }, {
        0x52297du, 0xffu
    }, {
        0x52297eu, 0xffu
    }, {
        0x52297fu, 0xffu
    }, {
        0x5229acu, 0xffu
    }, {
        0x5229adu, 0xffu
    }, {
        0x5229aeu, 0xffu
    }, {
        0x5229afu, 0xffu
    }, {
        0x5229dcu, 0xffu
    }, {
        0x5229ddu, 0xffu
    }, {
        0x5229deu, 0xffu
    }, {
        0x5229dfu, 0xffu
    }, {
        0x522a0cu, 0xffu
    }, {
        0x522a0du, 0xffu
    }, {
        0x522a0eu, 0xffu
    }, {
        0x522a0fu, 0xffu
    }, {
        0x522a3cu, 0xffu
    }, {
        0x522a3du, 0xffu
    }, {
        0x522a3eu, 0xffu
    }, {
        0x522a3fu, 0xffu
    }, {
        0x522a6cu, 0xffu
    }, {
        0x522a6du, 0xffu
    }, {
        0x522a6eu, 0xffu
    }, {
        0x522a6fu, 0xffu
    }, {
        0x522a9cu, 0xffu
    }, {
        0x522a9du, 0xffu
    }, {
        0x522a9eu, 0xffu
    }, {
        0x522a9fu, 0xffu
    }, {
        0x522accu, 0xffu
    }, {
        0x522acdu, 0xffu
    }, {
        0x522aceu, 0xffu
    }, {
        0x522acfu, 0xffu
    }, {
        0x522afcu, 0xffu
    }, {
        0x522afdu, 0xffu
    }, {
        0x522afeu, 0xffu
    }, {
        0x522affu, 0xffu
    }, {
        0x522b2cu, 0xffu
    }, {
        0x522b2du, 0xffu
    }, {
        0x522b2eu, 0xffu
    }, {
        0x522b2fu, 0xffu
    }, {
        0x522b5cu, 0xffu
    }, {
        0x522b5du, 0xffu
    }, {
        0x522b5eu, 0xffu
    }, {
        0x522b5fu, 0xffu
    }, {
        0x522b8cu, 0xffu
    }, {
        0x522b8du, 0xffu
    }, {
        0x522b8eu, 0xffu
    }, {
        0x522b8fu, 0xffu
    }, {
        0x522bbcu, 0xffu
    }, {
        0x522bbdu, 0xffu
    }, {
        0x522bbeu, 0xffu
    }, {
        0x522bbfu, 0xffu
    }, {
        0x522becu, 0xffu
    }, {
        0x522bedu, 0xffu
    }, {
        0x522beeu, 0xffu
    }, {
        0x522befu, 0xffu
    }, {
        0x522c1cu, 0xffu
    }, {
        0x522c1du, 0xffu
    }, {
        0x522c1eu, 0xffu
    }, {
        0x522c1fu, 0xffu
    }, {
        0x522c4cu, 0xffu
    }, {
        0x522c4du, 0xffu
    }, {
        0x522c4eu, 0xffu
    }, {
        0x522c4fu, 0xffu
    }, {
        0x522c7cu, 0xffu
    }, {
        0x522c7du, 0xffu
    }, {
        0x522c7eu, 0xffu
    }, {
        0x522c7fu, 0xffu
    }, {
        0x522cacu, 0xffu
    }, {
        0x522cadu, 0xffu
    }, {
        0x522caeu, 0xffu
    }, {
        0x522cafu, 0xffu
    }, {
        0x522cdcu, 0xffu
    }, {
        0x522cddu, 0xffu
    }, {
        0x522cdeu, 0xffu
    }, {
        0x522cdfu, 0xffu
    }, {
        0x522d0cu, 0xffu
    }, {
        0x522d0du, 0xffu
    }, {
        0x522d0eu, 0xffu
    }, {
        0x522d0fu, 0xffu
    }, {
        0x522d3cu, 0xffu
    }, {
        0x522d3du, 0xffu
    }, {
        0x522d3eu, 0xffu
    }, {
        0x522d3fu, 0xffu
    }, {
        0x522d6cu, 0xffu
    }, {
        0x522d6du, 0xffu
    }, {
        0x522d6eu, 0xffu
    }, {
        0x522d6fu, 0xffu
    }, {
        0x522d9cu, 0xffu
    }, {
        0x522d9du, 0xffu
    }, {
        0x522d9eu, 0xffu
    }, {
        0x522d9fu, 0xffu
    }, {
        0x522dccu, 0xffu
    }, {
        0x522dcdu, 0xffu
    }, {
        0x522dceu, 0xffu
    }, {
        0x522dcfu, 0xffu
    }, {
        0x522dfcu, 0xffu
    }, {
        0x522dfdu, 0xffu
    }, {
        0x522dfeu, 0xffu
    }, {
        0x522dffu, 0xffu
    }, {
        0x522e2cu, 0xffu
    }, {
        0x522e2du, 0xffu
    }, {
        0x522e2eu, 0xffu
    }, {
        0x522e2fu, 0xffu
    }, {
        0x522e5cu, 0xffu
    }, {
        0x522e5du, 0xffu
    }, {
        0x522e5eu, 0xffu
    }, {
        0x522e5fu, 0xffu
    }, {
        0x522e8cu, 0xffu
    }, {
        0x522e8du, 0xffu
    }, {
        0x522e8eu, 0xffu
    }, {
        0x522e8fu, 0xffu
    }, {
        0x522ebcu, 0xffu
    }, {
        0x522ebdu, 0xffu
    }, {
        0x522ebeu, 0xffu
    }, {
        0x522ebfu, 0xffu
    }, {
        0x522eecu, 0xffu
    }, {
        0x522eedu, 0xffu
    }, {
        0x522eeeu, 0xffu
    }, {
        0x522eefu, 0xffu
    }, {
        0x522f1cu, 0xffu
    }, {
        0x522f1du, 0xffu
    }, {
        0x522f1eu, 0xffu
    }, {
        0x522f1fu, 0xffu
    }, {
        0x522f4cu, 0xffu
    }, {
        0x522f4du, 0xffu
    }, {
        0x522f4eu, 0xffu
    }, {
        0x522f4fu, 0xffu
    }, {
        0x522f7cu, 0xffu
    }, {
        0x522f7du, 0xffu
    }, {
        0x522f7eu, 0xffu
    }, {
        0x522f7fu, 0xffu
    }, {
        0x522facu, 0xffu
    }, {
        0x522fadu, 0xffu
    }, {
        0x522faeu, 0xffu
    }, {
        0x522fafu, 0xffu
    }, {
        0x522fdcu, 0xffu
    }, {
        0x522fddu, 0xffu
    }, {
        0x522fdeu, 0xffu
    }, {
        0x522fdfu, 0xffu
    }, {
        0x52300cu, 0xffu
    }, {
        0x52300du, 0xffu
    }, {
        0x52300eu, 0xffu
    }, {
        0x52300fu, 0xffu
    }, {
        0x52303cu, 0xffu
    }, {
        0x52303du, 0xffu
    }, {
        0x52303eu, 0xffu
    }, {
        0x52303fu, 0xffu
    }, {
        0x52306cu, 0xffu
    }, {
        0x52306du, 0xffu
    }, {
        0x52306eu, 0xffu
    }, {
        0x52306fu, 0xffu
    }, {
        0x52309cu, 0xffu
    }, {
        0x52309du, 0xffu
    }, {
        0x52309eu, 0xffu
    }, {
        0x52309fu, 0xffu
    }, {
        0x5230ccu, 0xffu
    }, {
        0x5230cdu, 0xffu
    }, {
        0x5230ceu, 0xffu
    }, {
        0x5230cfu, 0xffu
    }, {
        0x5230fcu, 0xffu
    }, {
        0x5230fdu, 0xffu
    }, {
        0x5230feu, 0xffu
    }, {
        0x5230ffu, 0xffu
    }, {
        0x52312cu, 0xffu
    }, {
        0x52312du, 0xffu
    }, {
        0x52312eu, 0xffu
    }, {
        0x52312fu, 0xffu
    }, {
        0x52315cu, 0xffu
    }, {
        0x52315du, 0xffu
    }, {
        0x52315eu, 0xffu
    }, {
        0x52315fu, 0xffu
    }, {
        0x52318cu, 0xffu
    }, {
        0x52318du, 0xffu
    }, {
        0x52318eu, 0xffu
    }, {
        0x52318fu, 0xffu
    }, {
        0x5231bcu, 0xffu
    }, {
        0x5231bdu, 0xffu
    }, {
        0x5231beu, 0xffu
    }, {
        0x5231bfu, 0xffu
    }, {
        0x5231ecu, 0xffu
    }, {
        0x5231edu, 0xffu
    }, {
        0x5231eeu, 0xffu
    }, {
        0x5231efu, 0xffu
    }, {
        0x52321cu, 0xffu
    }, {
        0x52321du, 0xffu
    }, {
        0x52321eu, 0xffu
    }, {
        0x52321fu, 0xffu
    }, {
        0x52324cu, 0xffu
    }, {
        0x52324du, 0xffu
    }, {
        0x52324eu, 0xffu
    }, {
        0x52324fu, 0xffu
    }, {
        0x52327cu, 0xffu
    }, {
        0x52327du, 0xffu
    }, {
        0x52327eu, 0xffu
    }, {
        0x52327fu, 0xffu
    }, {
        0x5232acu, 0xffu
    }, {
        0x5232adu, 0xffu
    }, {
        0x5232aeu, 0xffu
    }, {
        0x5232afu, 0xffu
    }, {
        0x5232dcu, 0xffu
    }, {
        0x5232ddu, 0xffu
    }, {
        0x5232deu, 0xffu
    }, {
        0x5232dfu, 0xffu
    }, {
        0x52330cu, 0xffu
    }, {
        0x52330du, 0xffu
    }, {
        0x52330eu, 0xffu
    }, {
        0x52330fu, 0xffu
    }, {
        0x52333cu, 0xffu
    }, {
        0x52333du, 0xffu
    }, {
        0x52333eu, 0xffu
    }, {
        0x52333fu, 0xffu
    }, {
        0x52336cu, 0xffu
    }, {
        0x52336du, 0xffu
    }, {
        0x52336eu, 0xffu
    }, {
        0x52336fu, 0xffu
    }, {
        0x52339cu, 0xffu
    }, {
        0x52339du, 0xffu
    }, {
        0x52339eu, 0xffu
    }, {
        0x52339fu, 0xffu
    }, {
        0x5233ccu, 0xffu
    }, {
        0x5233cdu, 0xffu
    }, {
        0x5233ceu, 0xffu
    }, {
        0x5233cfu, 0xffu
    }, {
        0x5233fcu, 0xffu
    }, {
        0x5233fdu, 0xffu
    }, {
        0x5233feu, 0xffu
    }, {
        0x5233ffu, 0xffu
    }, {
        0x52342cu, 0xffu
    }, {
        0x52342du, 0xffu
    }, {
        0x52342eu, 0xffu
    }, {
        0x52342fu, 0xffu
    }, {
        0x52345cu, 0xffu
    }, {
        0x52345du, 0xffu
    }, {
        0x52345eu, 0xffu
    }, {
        0x52345fu, 0xffu
    }, {
        0x52348cu, 0xffu
    }, {
        0x52348du, 0xffu
    }, {
        0x52348eu, 0xffu
    }, {
        0x52348fu, 0xffu
    }, {
        0x5234bcu, 0xffu
    }, {
        0x5234bdu, 0xffu
    }, {
        0x5234beu, 0xffu
    }, {
        0x5234bfu, 0xffu
    }, {
        0x5234ecu, 0xffu
    }, {
        0x5234edu, 0xffu
    }, {
        0x5234eeu, 0xffu
    }, {
        0x5234efu, 0xffu
    }, {
        0x52351cu, 0xffu
    }, {
        0x52351du, 0xffu
    }, {
        0x52351eu, 0xffu
    }, {
        0x52351fu, 0xffu
    }, {
        0x52354cu, 0xffu
    }, {
        0x52354du, 0xffu
    }, {
        0x52354eu, 0xffu
    }, {
        0x52354fu, 0xffu
    }, {
        0x52357cu, 0xffu
    }, {
        0x52357du, 0xffu
    }, {
        0x52357eu, 0xffu
    }, {
        0x52357fu, 0xffu
    }, {
        0x5235acu, 0xffu
    }, {
        0x5235adu, 0xffu
    }, {
        0x5235aeu, 0xffu
    }, {
        0x5235afu, 0xffu
    }, {
        0x5235dcu, 0xffu
    }, {
        0x5235ddu, 0xffu
    }, {
        0x5235deu, 0xffu
    }, {
        0x5235dfu, 0xffu
    }, {
        0x52360cu, 0xffu
    }, {
        0x52360du, 0xffu
    }, {
        0x52360eu, 0xffu
    }, {
        0x52360fu, 0xffu
    }, {
        0x521a19u, 0x5u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_33[] = {
    {
        0xff4u, 0x88u
    }, {
        0xff5u, 0x77u
    }, {
        0xff6u, 0x66u
    }, {
        0xff7u, 0x55u
    }, {
        0xff8u, 0x77u
    }, {
        0xff9u, 0x66u
    }, {
        0xffau, 0x55u
    }, {
        0xffbu, 0x44u
    }, {
        0xffcu, 0x1u
    }, {
        0xffdu, 0xefu
    }, {
        0xffeu, 0xcdu
    }, {
        0xfffu, 0xabu
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_34[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x7bu
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x0u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x5u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x0u
    }, {
        0x100bu, 0x0u
    }, {
        0x521a1cu, 0xffu
    }, {
        0x521a1du, 0xffu
    }, {
        0x521a1eu, 0xffu
    }, {
        0x521a1fu, 0xffu
    }, {
        0x521a4cu, 0xffu
    }, {
        0x521a4du, 0xffu
    }, {
        0x521a4eu, 0xffu
    }, {
        0x521a4fu, 0xffu
    }, {
        0x521a7cu, 0xffu
    }, {
        0x521a7du, 0xffu
    }, {
        0x521a7eu, 0xffu
    }, {
        0x521a7fu, 0xffu
    }, {
        0x521aacu, 0xffu
    }, {
        0x521aadu, 0xffu
    }, {
        0x521aaeu, 0xffu
    }, {
        0x521aafu, 0xffu
    }, {
        0x521adcu, 0xffu
    }, {
        0x521addu, 0xffu
    }, {
        0x521adeu, 0xffu
    }, {
        0x521adfu, 0xffu
    }, {
        0x521b0cu, 0xffu
    }, {
        0x521b0du, 0xffu
    }, {
        0x521b0eu, 0xffu
    }, {
        0x521b0fu, 0xffu
    }, {
        0x521b3cu, 0xffu
    }, {
        0x521b3du, 0xffu
    }, {
        0x521b3eu, 0xffu
    }, {
        0x521b3fu, 0xffu
    }, {
        0x521b6cu, 0xffu
    }, {
        0x521b6du, 0xffu
    }, {
        0x521b6eu, 0xffu
    }, {
        0x521b6fu, 0xffu
    }, {
        0x521b9cu, 0xffu
    }, {
        0x521b9du, 0xffu
    }, {
        0x521b9eu, 0xffu
    }, {
        0x521b9fu, 0xffu
    }, {
        0x521bccu, 0xffu
    }, {
        0x521bcdu, 0xffu
    }, {
        0x521bceu, 0xffu
    }, {
        0x521bcfu, 0xffu
    }, {
        0x521bfcu, 0xffu
    }, {
        0x521bfdu, 0xffu
    }, {
        0x521bfeu, 0xffu
    }, {
        0x521bffu, 0xffu
    }, {
        0x521c2cu, 0xffu
    }, {
        0x521c2du, 0xffu
    }, {
        0x521c2eu, 0xffu
    }, {
        0x521c2fu, 0xffu
    }, {
        0x521c5cu, 0xffu
    }, {
        0x521c5du, 0xffu
    }, {
        0x521c5eu, 0xffu
    }, {
        0x521c5fu, 0xffu
    }, {
        0x521c8cu, 0xffu
    }, {
        0x521c8du, 0xffu
    }, {
        0x521c8eu, 0xffu
    }, {
        0x521c8fu, 0xffu
    }, {
        0x521cbcu, 0xffu
    }, {
        0x521cbdu, 0xffu
    }, {
        0x521cbeu, 0xffu
    }, {
        0x521cbfu, 0xffu
    }, {
        0x521cecu, 0xffu
    }, {
        0x521cedu, 0xffu
    }, {
        0x521ceeu, 0xffu
    }, {
        0x521cefu, 0xffu
    }, {
        0x521d1cu, 0xffu
    }, {
        0x521d1du, 0xffu
    }, {
        0x521d1eu, 0xffu
    }, {
        0x521d1fu, 0xffu
    }, {
        0x521d4cu, 0xffu
    }, {
        0x521d4du, 0xffu
    }, {
        0x521d4eu, 0xffu
    }, {
        0x521d4fu, 0xffu
    }, {
        0x521d7cu, 0xffu
    }, {
        0x521d7du, 0xffu
    }, {
        0x521d7eu, 0xffu
    }, {
        0x521d7fu, 0xffu
    }, {
        0x521dacu, 0xffu
    }, {
        0x521dadu, 0xffu
    }, {
        0x521daeu, 0xffu
    }, {
        0x521dafu, 0xffu
    }, {
        0x521ddcu, 0xffu
    }, {
        0x521dddu, 0xffu
    }, {
        0x521ddeu, 0xffu
    }, {
        0x521ddfu, 0xffu
    }, {
        0x521e0cu, 0xffu
    }, {
        0x521e0du, 0xffu
    }, {
        0x521e0eu, 0xffu
    }, {
        0x521e0fu, 0xffu
    }, {
        0x521e3cu, 0xffu
    }, {
        0x521e3du, 0xffu
    }, {
        0x521e3eu, 0xffu
    }, {
        0x521e3fu, 0xffu
    }, {
        0x521e6cu, 0xffu
    }, {
        0x521e6du, 0xffu
    }, {
        0x521e6eu, 0xffu
    }, {
        0x521e6fu, 0xffu
    }, {
        0x521e9cu, 0xffu
    }, {
        0x521e9du, 0xffu
    }, {
        0x521e9eu, 0xffu
    }, {
        0x521e9fu, 0xffu
    }, {
        0x521eccu, 0xffu
    }, {
        0x521ecdu, 0xffu
    }, {
        0x521eceu, 0xffu
    }, {
        0x521ecfu, 0xffu
    }, {
        0x521efcu, 0xffu
    }, {
        0x521efdu, 0xffu
    }, {
        0x521efeu, 0xffu
    }, {
        0x521effu, 0xffu
    }, {
        0x521f2cu, 0xffu
    }, {
        0x521f2du, 0xffu
    }, {
        0x521f2eu, 0xffu
    }, {
        0x521f2fu, 0xffu
    }, {
        0x521f5cu, 0xffu
    }, {
        0x521f5du, 0xffu
    }, {
        0x521f5eu, 0xffu
    }, {
        0x521f5fu, 0xffu
    }, {
        0x521f8cu, 0xffu
    }, {
        0x521f8du, 0xffu
    }, {
        0x521f8eu, 0xffu
    }, {
        0x521f8fu, 0xffu
    }, {
        0x521fbcu, 0xffu
    }, {
        0x521fbdu, 0xffu
    }, {
        0x521fbeu, 0xffu
    }, {
        0x521fbfu, 0xffu
    }, {
        0x521fecu, 0xffu
    }, {
        0x521fedu, 0xffu
    }, {
        0x521feeu, 0xffu
    }, {
        0x521fefu, 0xffu
    }, {
        0x52201cu, 0xffu
    }, {
        0x52201du, 0xffu
    }, {
        0x52201eu, 0xffu
    }, {
        0x52201fu, 0xffu
    }, {
        0x52204cu, 0xffu
    }, {
        0x52204du, 0xffu
    }, {
        0x52204eu, 0xffu
    }, {
        0x52204fu, 0xffu
    }, {
        0x52207cu, 0xffu
    }, {
        0x52207du, 0xffu
    }, {
        0x52207eu, 0xffu
    }, {
        0x52207fu, 0xffu
    }, {
        0x5220acu, 0xffu
    }, {
        0x5220adu, 0xffu
    }, {
        0x5220aeu, 0xffu
    }, {
        0x5220afu, 0xffu
    }, {
        0x5220dcu, 0xffu
    }, {
        0x5220ddu, 0xffu
    }, {
        0x5220deu, 0xffu
    }, {
        0x5220dfu, 0xffu
    }, {
        0x52210cu, 0xffu
    }, {
        0x52210du, 0xffu
    }, {
        0x52210eu, 0xffu
    }, {
        0x52210fu, 0xffu
    }, {
        0x52213cu, 0xffu
    }, {
        0x52213du, 0xffu
    }, {
        0x52213eu, 0xffu
    }, {
        0x52213fu, 0xffu
    }, {
        0x52216cu, 0xffu
    }, {
        0x52216du, 0xffu
    }, {
        0x52216eu, 0xffu
    }, {
        0x52216fu, 0xffu
    }, {
        0x52219cu, 0xffu
    }, {
        0x52219du, 0xffu
    }, {
        0x52219eu, 0xffu
    }, {
        0x52219fu, 0xffu
    }, {
        0x5221ccu, 0xffu
    }, {
        0x5221cdu, 0xffu
    }, {
        0x5221ceu, 0xffu
    }, {
        0x5221cfu, 0xffu
    }, {
        0x5221fcu, 0xffu
    }, {
        0x5221fdu, 0xffu
    }, {
        0x5221feu, 0xffu
    }, {
        0x5221ffu, 0xffu
    }, {
        0x52222cu, 0xffu
    }, {
        0x52222du, 0xffu
    }, {
        0x52222eu, 0xffu
    }, {
        0x52222fu, 0xffu
    }, {
        0x52225cu, 0xffu
    }, {
        0x52225du, 0xffu
    }, {
        0x52225eu, 0xffu
    }, {
        0x52225fu, 0xffu
    }, {
        0x52228cu, 0xffu
    }, {
        0x52228du, 0xffu
    }, {
        0x52228eu, 0xffu
    }, {
        0x52228fu, 0xffu
    }, {
        0x5222bcu, 0xffu
    }, {
        0x5222bdu, 0xffu
    }, {
        0x5222beu, 0xffu
    }, {
        0x5222bfu, 0xffu
    }, {
        0x5222ecu, 0xffu
    }, {
        0x5222edu, 0xffu
    }, {
        0x5222eeu, 0xffu
    }, {
        0x5222efu, 0xffu
    }, {
        0x52231cu, 0xffu
    }, {
        0x52231du, 0xffu
    }, {
        0x52231eu, 0xffu
    }, {
        0x52231fu, 0xffu
    }, {
        0x52234cu, 0xffu
    }, {
        0x52234du, 0xffu
    }, {
        0x52234eu, 0xffu
    }, {
        0x52234fu, 0xffu
    }, {
        0x52237cu, 0xffu
    }, {
        0x52237du, 0xffu
    }, {
        0x52237eu, 0xffu
    }, {
        0x52237fu, 0xffu
    }, {
        0x5223acu, 0xffu
    }, {
        0x5223adu, 0xffu
    }, {
        0x5223aeu, 0xffu
    }, {
        0x5223afu, 0xffu
    }, {
        0x5223dcu, 0xffu
    }, {
        0x5223ddu, 0xffu
    }, {
        0x5223deu, 0xffu
    }, {
        0x5223dfu, 0xffu
    }, {
        0x52240cu, 0xffu
    }, {
        0x52240du, 0xffu
    }, {
        0x52240eu, 0xffu
    }, {
        0x52240fu, 0xffu
    }, {
        0x52243cu, 0xffu
    }, {
        0x52243du, 0xffu
    }, {
        0x52243eu, 0xffu
    }, {
        0x52243fu, 0xffu
    }, {
        0x52246cu, 0xffu
    }, {
        0x52246du, 0xffu
    }, {
        0x52246eu, 0xffu
    }, {
        0x52246fu, 0xffu
    }, {
        0x52249cu, 0xffu
    }, {
        0x52249du, 0xffu
    }, {
        0x52249eu, 0xffu
    }, {
        0x52249fu, 0xffu
    }, {
        0x5224ccu, 0xffu
    }, {
        0x5224cdu, 0xffu
    }, {
        0x5224ceu, 0xffu
    }, {
        0x5224cfu, 0xffu
    }, {
        0x5224fcu, 0xffu
    }, {
        0x5224fdu, 0xffu
    }, {
        0x5224feu, 0xffu
    }, {
        0x5224ffu, 0xffu
    }, {
        0x52252cu, 0xffu
    }, {
        0x52252du, 0xffu
    }, {
        0x52252eu, 0xffu
    }, {
        0x52252fu, 0xffu
    }, {
        0x52255cu, 0xffu
    }, {
        0x52255du, 0xffu
    }, {
        0x52255eu, 0xffu
    }, {
        0x52255fu, 0xffu
    }, {
        0x52258cu, 0xffu
    }, {
        0x52258du, 0xffu
    }, {
        0x52258eu, 0xffu
    }, {
        0x52258fu, 0xffu
    }, {
        0x5225bcu, 0xffu
    }, {
        0x5225bdu, 0xffu
    }, {
        0x5225beu, 0xffu
    }, {
        0x5225bfu, 0xffu
    }, {
        0x5225ecu, 0xffu
    }, {
        0x5225edu, 0xffu
    }, {
        0x5225eeu, 0xffu
    }, {
        0x5225efu, 0xffu
    }, {
        0x52261cu, 0xffu
    }, {
        0x52261du, 0xffu
    }, {
        0x52261eu, 0xffu
    }, {
        0x52261fu, 0xffu
    }, {
        0x52264cu, 0xffu
    }, {
        0x52264du, 0xffu
    }, {
        0x52264eu, 0xffu
    }, {
        0x52264fu, 0xffu
    }, {
        0x52267cu, 0xffu
    }, {
        0x52267du, 0xffu
    }, {
        0x52267eu, 0xffu
    }, {
        0x52267fu, 0xffu
    }, {
        0x5226acu, 0xffu
    }, {
        0x5226adu, 0xffu
    }, {
        0x5226aeu, 0xffu
    }, {
        0x5226afu, 0xffu
    }, {
        0x5226dcu, 0xffu
    }, {
        0x5226ddu, 0xffu
    }, {
        0x5226deu, 0xffu
    }, {
        0x5226dfu, 0xffu
    }, {
        0x52270cu, 0xffu
    }, {
        0x52270du, 0xffu
    }, {
        0x52270eu, 0xffu
    }, {
        0x52270fu, 0xffu
    }, {
        0x52273cu, 0xffu
    }, {
        0x52273du, 0xffu
    }, {
        0x52273eu, 0xffu
    }, {
        0x52273fu, 0xffu
    }, {
        0x52276cu, 0xffu
    }, {
        0x52276du, 0xffu
    }, {
        0x52276eu, 0xffu
    }, {
        0x52276fu, 0xffu
    }, {
        0x52279cu, 0xffu
    }, {
        0x52279du, 0xffu
    }, {
        0x52279eu, 0xffu
    }, {
        0x52279fu, 0xffu
    }, {
        0x5227ccu, 0xffu
    }, {
        0x5227cdu, 0xffu
    }, {
        0x5227ceu, 0xffu
    }, {
        0x5227cfu, 0xffu
    }, {
        0x5227fcu, 0xffu
    }, {
        0x5227fdu, 0xffu
    }, {
        0x5227feu, 0xffu
    }, {
        0x5227ffu, 0xffu
    }, {
        0x52282cu, 0xffu
    }, {
        0x52282du, 0xffu
    }, {
        0x52282eu, 0xffu
    }, {
        0x52282fu, 0xffu
    }, {
        0x52285cu, 0xffu
    }, {
        0x52285du, 0xffu
    }, {
        0x52285eu, 0xffu
    }, {
        0x52285fu, 0xffu
    }, {
        0x52288cu, 0xffu
    }, {
        0x52288du, 0xffu
    }, {
        0x52288eu, 0xffu
    }, {
        0x52288fu, 0xffu
    }, {
        0x5228bcu, 0xffu
    }, {
        0x5228bdu, 0xffu
    }, {
        0x5228beu, 0xffu
    }, {
        0x5228bfu, 0xffu
    }, {
        0x5228ecu, 0xffu
    }, {
        0x5228edu, 0xffu
    }, {
        0x5228eeu, 0xffu
    }, {
        0x5228efu, 0xffu
    }, {
        0x52291cu, 0xffu
    }, {
        0x52291du, 0xffu
    }, {
        0x52291eu, 0xffu
    }, {
        0x52291fu, 0xffu
    }, {
        0x52294cu, 0xffu
    }, {
        0x52294du, 0xffu
    }, {
        0x52294eu, 0xffu
    }, {
        0x52294fu, 0xffu
    }, {
        0x52297cu, 0xffu
    }, {
        0x52297du, 0xffu
    }, {
        0x52297eu, 0xffu
    }, {
        0x52297fu, 0xffu
    }, {
        0x5229acu, 0xffu
    }, {
        0x5229adu, 0xffu
    }, {
        0x5229aeu, 0xffu
    }, {
        0x5229afu, 0xffu
    }, {
        0x5229dcu, 0xffu
    }, {
        0x5229ddu, 0xffu
    }, {
        0x5229deu, 0xffu
    }, {
        0x5229dfu, 0xffu
    }, {
        0x522a0cu, 0xffu
    }, {
        0x522a0du, 0xffu
    }, {
        0x522a0eu, 0xffu
    }, {
        0x522a0fu, 0xffu
    }, {
        0x522a3cu, 0xffu
    }, {
        0x522a3du, 0xffu
    }, {
        0x522a3eu, 0xffu
    }, {
        0x522a3fu, 0xffu
    }, {
        0x522a6cu, 0xffu
    }, {
        0x522a6du, 0xffu
    }, {
        0x522a6eu, 0xffu
    }, {
        0x522a6fu, 0xffu
    }, {
        0x522a9cu, 0xffu
    }, {
        0x522a9du, 0xffu
    }, {
        0x522a9eu, 0xffu
    }, {
        0x522a9fu, 0xffu
    }, {
        0x522accu, 0xffu
    }, {
        0x522acdu, 0xffu
    }, {
        0x522aceu, 0xffu
    }, {
        0x522acfu, 0xffu
    }, {
        0x522afcu, 0xffu
    }, {
        0x522afdu, 0xffu
    }, {
        0x522afeu, 0xffu
    }, {
        0x522affu, 0xffu
    }, {
        0x522b2cu, 0xffu
    }, {
        0x522b2du, 0xffu
    }, {
        0x522b2eu, 0xffu
    }, {
        0x522b2fu, 0xffu
    }, {
        0x522b5cu, 0xffu
    }, {
        0x522b5du, 0xffu
    }, {
        0x522b5eu, 0xffu
    }, {
        0x522b5fu, 0xffu
    }, {
        0x522b8cu, 0xffu
    }, {
        0x522b8du, 0xffu
    }, {
        0x522b8eu, 0xffu
    }, {
        0x522b8fu, 0xffu
    }, {
        0x522bbcu, 0xffu
    }, {
        0x522bbdu, 0xffu
    }, {
        0x522bbeu, 0xffu
    }, {
        0x522bbfu, 0xffu
    }, {
        0x522becu, 0xffu
    }, {
        0x522bedu, 0xffu
    }, {
        0x522beeu, 0xffu
    }, {
        0x522befu, 0xffu
    }, {
        0x522c1cu, 0xffu
    }, {
        0x522c1du, 0xffu
    }, {
        0x522c1eu, 0xffu
    }, {
        0x522c1fu, 0xffu
    }, {
        0x522c4cu, 0xffu
    }, {
        0x522c4du, 0xffu
    }, {
        0x522c4eu, 0xffu
    }, {
        0x522c4fu, 0xffu
    }, {
        0x522c7cu, 0xffu
    }, {
        0x522c7du, 0xffu
    }, {
        0x522c7eu, 0xffu
    }, {
        0x522c7fu, 0xffu
    }, {
        0x522cacu, 0xffu
    }, {
        0x522cadu, 0xffu
    }, {
        0x522caeu, 0xffu
    }, {
        0x522cafu, 0xffu
    }, {
        0x522cdcu, 0xffu
    }, {
        0x522cddu, 0xffu
    }, {
        0x522cdeu, 0xffu
    }, {
        0x522cdfu, 0xffu
    }, {
        0x522d0cu, 0xffu
    }, {
        0x522d0du, 0xffu
    }, {
        0x522d0eu, 0xffu
    }, {
        0x522d0fu, 0xffu
    }, {
        0x522d3cu, 0xffu
    }, {
        0x522d3du, 0xffu
    }, {
        0x522d3eu, 0xffu
    }, {
        0x522d3fu, 0xffu
    }, {
        0x522d6cu, 0xffu
    }, {
        0x522d6du, 0xffu
    }, {
        0x522d6eu, 0xffu
    }, {
        0x522d6fu, 0xffu
    }, {
        0x522d9cu, 0xffu
    }, {
        0x522d9du, 0xffu
    }, {
        0x522d9eu, 0xffu
    }, {
        0x522d9fu, 0xffu
    }, {
        0x522dccu, 0xffu
    }, {
        0x522dcdu, 0xffu
    }, {
        0x522dceu, 0xffu
    }, {
        0x522dcfu, 0xffu
    }, {
        0x522dfcu, 0xffu
    }, {
        0x522dfdu, 0xffu
    }, {
        0x522dfeu, 0xffu
    }, {
        0x522dffu, 0xffu
    }, {
        0x522e2cu, 0xffu
    }, {
        0x522e2du, 0xffu
    }, {
        0x522e2eu, 0xffu
    }, {
        0x522e2fu, 0xffu
    }, {
        0x522e5cu, 0xffu
    }, {
        0x522e5du, 0xffu
    }, {
        0x522e5eu, 0xffu
    }, {
        0x522e5fu, 0xffu
    }, {
        0x522e8cu, 0xffu
    }, {
        0x522e8du, 0xffu
    }, {
        0x522e8eu, 0xffu
    }, {
        0x522e8fu, 0xffu
    }, {
        0x522ebcu, 0xffu
    }, {
        0x522ebdu, 0xffu
    }, {
        0x522ebeu, 0xffu
    }, {
        0x522ebfu, 0xffu
    }, {
        0x522eecu, 0xffu
    }, {
        0x522eedu, 0xffu
    }, {
        0x522eeeu, 0xffu
    }, {
        0x522eefu, 0xffu
    }, {
        0x522f1cu, 0xffu
    }, {
        0x522f1du, 0xffu
    }, {
        0x522f1eu, 0xffu
    }, {
        0x522f1fu, 0xffu
    }, {
        0x522f4cu, 0xffu
    }, {
        0x522f4du, 0xffu
    }, {
        0x522f4eu, 0xffu
    }, {
        0x522f4fu, 0xffu
    }, {
        0x522f7cu, 0xffu
    }, {
        0x522f7du, 0xffu
    }, {
        0x522f7eu, 0xffu
    }, {
        0x522f7fu, 0xffu
    }, {
        0x522facu, 0xffu
    }, {
        0x522fadu, 0xffu
    }, {
        0x522faeu, 0xffu
    }, {
        0x522fafu, 0xffu
    }, {
        0x522fdcu, 0xffu
    }, {
        0x522fddu, 0xffu
    }, {
        0x522fdeu, 0xffu
    }, {
        0x522fdfu, 0xffu
    }, {
        0x52300cu, 0xffu
    }, {
        0x52300du, 0xffu
    }, {
        0x52300eu, 0xffu
    }, {
        0x52300fu, 0xffu
    }, {
        0x52303cu, 0xffu
    }, {
        0x52303du, 0xffu
    }, {
        0x52303eu, 0xffu
    }, {
        0x52303fu, 0xffu
    }, {
        0x52306cu, 0xffu
    }, {
        0x52306du, 0xffu
    }, {
        0x52306eu, 0xffu
    }, {
        0x52306fu, 0xffu
    }, {
        0x52309cu, 0xffu
    }, {
        0x52309du, 0xffu
    }, {
        0x52309eu, 0xffu
    }, {
        0x52309fu, 0xffu
    }, {
        0x5230ccu, 0xffu
    }, {
        0x5230cdu, 0xffu
    }, {
        0x5230ceu, 0xffu
    }, {
        0x5230cfu, 0xffu
    }, {
        0x5230fcu, 0xffu
    }, {
        0x5230fdu, 0xffu
    }, {
        0x5230feu, 0xffu
    }, {
        0x5230ffu, 0xffu
    }, {
        0x52312cu, 0xffu
    }, {
        0x52312du, 0xffu
    }, {
        0x52312eu, 0xffu
    }, {
        0x52312fu, 0xffu
    }, {
        0x52315cu, 0xffu
    }, {
        0x52315du, 0xffu
    }, {
        0x52315eu, 0xffu
    }, {
        0x52315fu, 0xffu
    }, {
        0x52318cu, 0xffu
    }, {
        0x52318du, 0xffu
    }, {
        0x52318eu, 0xffu
    }, {
        0x52318fu, 0xffu
    }, {
        0x5231bcu, 0xffu
    }, {
        0x5231bdu, 0xffu
    }, {
        0x5231beu, 0xffu
    }, {
        0x5231bfu, 0xffu
    }, {
        0x5231ecu, 0xffu
    }, {
        0x5231edu, 0xffu
    }, {
        0x5231eeu, 0xffu
    }, {
        0x5231efu, 0xffu
    }, {
        0x52321cu, 0xffu
    }, {
        0x52321du, 0xffu
    }, {
        0x52321eu, 0xffu
    }, {
        0x52321fu, 0xffu
    }, {
        0x52324cu, 0xffu
    }, {
        0x52324du, 0xffu
    }, {
        0x52324eu, 0xffu
    }, {
        0x52324fu, 0xffu
    }, {
        0x52327cu, 0xffu
    }, {
        0x52327du, 0xffu
    }, {
        0x52327eu, 0xffu
    }, {
        0x52327fu, 0xffu
    }, {
        0x5232acu, 0xffu
    }, {
        0x5232adu, 0xffu
    }, {
        0x5232aeu, 0xffu
    }, {
        0x5232afu, 0xffu
    }, {
        0x5232dcu, 0xffu
    }, {
        0x5232ddu, 0xffu
    }, {
        0x5232deu, 0xffu
    }, {
        0x5232dfu, 0xffu
    }, {
        0x52330cu, 0xffu
    }, {
        0x52330du, 0xffu
    }, {
        0x52330eu, 0xffu
    }, {
        0x52330fu, 0xffu
    }, {
        0x52333cu, 0xffu
    }, {
        0x52333du, 0xffu
    }, {
        0x52333eu, 0xffu
    }, {
        0x52333fu, 0xffu
    }, {
        0x52336cu, 0xffu
    }, {
        0x52336du, 0xffu
    }, {
        0x52336eu, 0xffu
    }, {
        0x52336fu, 0xffu
    }, {
        0x52339cu, 0xffu
    }, {
        0x52339du, 0xffu
    }, {
        0x52339eu, 0xffu
    }, {
        0x52339fu, 0xffu
    }, {
        0x5233ccu, 0xffu
    }, {
        0x5233cdu, 0xffu
    }, {
        0x5233ceu, 0xffu
    }, {
        0x5233cfu, 0xffu
    }, {
        0x5233fcu, 0xffu
    }, {
        0x5233fdu, 0xffu
    }, {
        0x5233feu, 0xffu
    }, {
        0x5233ffu, 0xffu
    }, {
        0x52342cu, 0xffu
    }, {
        0x52342du, 0xffu
    }, {
        0x52342eu, 0xffu
    }, {
        0x52342fu, 0xffu
    }, {
        0x52345cu, 0xffu
    }, {
        0x52345du, 0xffu
    }, {
        0x52345eu, 0xffu
    }, {
        0x52345fu, 0xffu
    }, {
        0x52348cu, 0xffu
    }, {
        0x52348du, 0xffu
    }, {
        0x52348eu, 0xffu
    }, {
        0x52348fu, 0xffu
    }, {
        0x5234bcu, 0xffu
    }, {
        0x5234bdu, 0xffu
    }, {
        0x5234beu, 0xffu
    }, {
        0x5234bfu, 0xffu
    }, {
        0x5234ecu, 0xffu
    }, {
        0x5234edu, 0xffu
    }, {
        0x5234eeu, 0xffu
    }, {
        0x5234efu, 0xffu
    }, {
        0x52351cu, 0xffu
    }, {
        0x52351du, 0xffu
    }, {
        0x52351eu, 0xffu
    }, {
        0x52351fu, 0xffu
    }, {
        0x52354cu, 0xffu
    }, {
        0x52354du, 0xffu
    }, {
        0x52354eu, 0xffu
    }, {
        0x52354fu, 0xffu
    }, {
        0x52357cu, 0xffu
    }, {
        0x52357du, 0xffu
    }, {
        0x52357eu, 0xffu
    }, {
        0x52357fu, 0xffu
    }, {
        0x5235acu, 0xffu
    }, {
        0x5235adu, 0xffu
    }, {
        0x5235aeu, 0xffu
    }, {
        0x5235afu, 0xffu
    }, {
        0x5235dcu, 0xffu
    }, {
        0x5235ddu, 0xffu
    }, {
        0x5235deu, 0xffu
    }, {
        0x5235dfu, 0xffu
    }, {
        0x52360cu, 0x7bu
    }, {
        0x52360du, 0x0u
    }, {
        0x52360eu, 0x0u
    }, {
        0x52360fu, 0x0u
    }, {
        0x523609u, 0x5u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_34[] = {
    {
        0xff4u, 0x88u
    }, {
        0xff5u, 0x77u
    }, {
        0xff6u, 0x66u
    }, {
        0xff7u, 0x55u
    }, {
        0xff8u, 0x77u
    }, {
        0xff9u, 0x66u
    }, {
        0xffau, 0x55u
    }, {
        0xffbu, 0x44u
    }, {
        0xffcu, 0x1u
    }, {
        0xffdu, 0xefu
    }, {
        0xffeu, 0xcdu
    }, {
        0xfffu, 0xabu
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_35[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x7bu
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x0u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x5u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x0u
    }, {
        0x100bu, 0x0u
    }, {
        0x521a1cu, 0xffu
    }, {
        0x521a1du, 0xffu
    }, {
        0x521a1eu, 0xffu
    }, {
        0x521a1fu, 0xffu
    }, {
        0x521a4cu, 0xffu
    }, {
        0x521a4du, 0xffu
    }, {
        0x521a4eu, 0xffu
    }, {
        0x521a4fu, 0xffu
    }, {
        0x521a7cu, 0xffu
    }, {
        0x521a7du, 0xffu
    }, {
        0x521a7eu, 0xffu
    }, {
        0x521a7fu, 0xffu
    }, {
        0x521aacu, 0xffu
    }, {
        0x521aadu, 0xffu
    }, {
        0x521aaeu, 0xffu
    }, {
        0x521aafu, 0xffu
    }, {
        0x521adcu, 0xffu
    }, {
        0x521addu, 0xffu
    }, {
        0x521adeu, 0xffu
    }, {
        0x521adfu, 0xffu
    }, {
        0x521b0cu, 0xffu
    }, {
        0x521b0du, 0xffu
    }, {
        0x521b0eu, 0xffu
    }, {
        0x521b0fu, 0xffu
    }, {
        0x521b3cu, 0xffu
    }, {
        0x521b3du, 0xffu
    }, {
        0x521b3eu, 0xffu
    }, {
        0x521b3fu, 0xffu
    }, {
        0x521b6cu, 0xffu
    }, {
        0x521b6du, 0xffu
    }, {
        0x521b6eu, 0xffu
    }, {
        0x521b6fu, 0xffu
    }, {
        0x521b9cu, 0xffu
    }, {
        0x521b9du, 0xffu
    }, {
        0x521b9eu, 0xffu
    }, {
        0x521b9fu, 0xffu
    }, {
        0x521bccu, 0xffu
    }, {
        0x521bcdu, 0xffu
    }, {
        0x521bceu, 0xffu
    }, {
        0x521bcfu, 0xffu
    }, {
        0x521bfcu, 0xffu
    }, {
        0x521bfdu, 0xffu
    }, {
        0x521bfeu, 0xffu
    }, {
        0x521bffu, 0xffu
    }, {
        0x521c2cu, 0xffu
    }, {
        0x521c2du, 0xffu
    }, {
        0x521c2eu, 0xffu
    }, {
        0x521c2fu, 0xffu
    }, {
        0x521c5cu, 0xffu
    }, {
        0x521c5du, 0xffu
    }, {
        0x521c5eu, 0xffu
    }, {
        0x521c5fu, 0xffu
    }, {
        0x521c8cu, 0xffu
    }, {
        0x521c8du, 0xffu
    }, {
        0x521c8eu, 0xffu
    }, {
        0x521c8fu, 0xffu
    }, {
        0x521cbcu, 0xffu
    }, {
        0x521cbdu, 0xffu
    }, {
        0x521cbeu, 0xffu
    }, {
        0x521cbfu, 0xffu
    }, {
        0x521cecu, 0xffu
    }, {
        0x521cedu, 0xffu
    }, {
        0x521ceeu, 0xffu
    }, {
        0x521cefu, 0xffu
    }, {
        0x521d1cu, 0xffu
    }, {
        0x521d1du, 0xffu
    }, {
        0x521d1eu, 0xffu
    }, {
        0x521d1fu, 0xffu
    }, {
        0x521d4cu, 0xffu
    }, {
        0x521d4du, 0xffu
    }, {
        0x521d4eu, 0xffu
    }, {
        0x521d4fu, 0xffu
    }, {
        0x521d7cu, 0xffu
    }, {
        0x521d7du, 0xffu
    }, {
        0x521d7eu, 0xffu
    }, {
        0x521d7fu, 0xffu
    }, {
        0x521dacu, 0xffu
    }, {
        0x521dadu, 0xffu
    }, {
        0x521daeu, 0xffu
    }, {
        0x521dafu, 0xffu
    }, {
        0x521ddcu, 0xffu
    }, {
        0x521dddu, 0xffu
    }, {
        0x521ddeu, 0xffu
    }, {
        0x521ddfu, 0xffu
    }, {
        0x521e0cu, 0xffu
    }, {
        0x521e0du, 0xffu
    }, {
        0x521e0eu, 0xffu
    }, {
        0x521e0fu, 0xffu
    }, {
        0x521e3cu, 0xffu
    }, {
        0x521e3du, 0xffu
    }, {
        0x521e3eu, 0xffu
    }, {
        0x521e3fu, 0xffu
    }, {
        0x521e6cu, 0xffu
    }, {
        0x521e6du, 0xffu
    }, {
        0x521e6eu, 0xffu
    }, {
        0x521e6fu, 0xffu
    }, {
        0x521e9cu, 0xffu
    }, {
        0x521e9du, 0xffu
    }, {
        0x521e9eu, 0xffu
    }, {
        0x521e9fu, 0xffu
    }, {
        0x521eccu, 0xffu
    }, {
        0x521ecdu, 0xffu
    }, {
        0x521eceu, 0xffu
    }, {
        0x521ecfu, 0xffu
    }, {
        0x521efcu, 0xffu
    }, {
        0x521efdu, 0xffu
    }, {
        0x521efeu, 0xffu
    }, {
        0x521effu, 0xffu
    }, {
        0x521f2cu, 0xffu
    }, {
        0x521f2du, 0xffu
    }, {
        0x521f2eu, 0xffu
    }, {
        0x521f2fu, 0xffu
    }, {
        0x521f5cu, 0xffu
    }, {
        0x521f5du, 0xffu
    }, {
        0x521f5eu, 0xffu
    }, {
        0x521f5fu, 0xffu
    }, {
        0x521f8cu, 0xffu
    }, {
        0x521f8du, 0xffu
    }, {
        0x521f8eu, 0xffu
    }, {
        0x521f8fu, 0xffu
    }, {
        0x521fbcu, 0xffu
    }, {
        0x521fbdu, 0xffu
    }, {
        0x521fbeu, 0xffu
    }, {
        0x521fbfu, 0xffu
    }, {
        0x521fecu, 0xffu
    }, {
        0x521fedu, 0xffu
    }, {
        0x521feeu, 0xffu
    }, {
        0x521fefu, 0xffu
    }, {
        0x52201cu, 0xffu
    }, {
        0x52201du, 0xffu
    }, {
        0x52201eu, 0xffu
    }, {
        0x52201fu, 0xffu
    }, {
        0x52204cu, 0xffu
    }, {
        0x52204du, 0xffu
    }, {
        0x52204eu, 0xffu
    }, {
        0x52204fu, 0xffu
    }, {
        0x52207cu, 0xffu
    }, {
        0x52207du, 0xffu
    }, {
        0x52207eu, 0xffu
    }, {
        0x52207fu, 0xffu
    }, {
        0x5220acu, 0xffu
    }, {
        0x5220adu, 0xffu
    }, {
        0x5220aeu, 0xffu
    }, {
        0x5220afu, 0xffu
    }, {
        0x5220dcu, 0xffu
    }, {
        0x5220ddu, 0xffu
    }, {
        0x5220deu, 0xffu
    }, {
        0x5220dfu, 0xffu
    }, {
        0x52210cu, 0xffu
    }, {
        0x52210du, 0xffu
    }, {
        0x52210eu, 0xffu
    }, {
        0x52210fu, 0xffu
    }, {
        0x52213cu, 0xffu
    }, {
        0x52213du, 0xffu
    }, {
        0x52213eu, 0xffu
    }, {
        0x52213fu, 0xffu
    }, {
        0x52216cu, 0xffu
    }, {
        0x52216du, 0xffu
    }, {
        0x52216eu, 0xffu
    }, {
        0x52216fu, 0xffu
    }, {
        0x52219cu, 0xffu
    }, {
        0x52219du, 0xffu
    }, {
        0x52219eu, 0xffu
    }, {
        0x52219fu, 0xffu
    }, {
        0x5221ccu, 0xffu
    }, {
        0x5221cdu, 0xffu
    }, {
        0x5221ceu, 0xffu
    }, {
        0x5221cfu, 0xffu
    }, {
        0x5221fcu, 0xffu
    }, {
        0x5221fdu, 0xffu
    }, {
        0x5221feu, 0xffu
    }, {
        0x5221ffu, 0xffu
    }, {
        0x52222cu, 0xffu
    }, {
        0x52222du, 0xffu
    }, {
        0x52222eu, 0xffu
    }, {
        0x52222fu, 0xffu
    }, {
        0x52225cu, 0xffu
    }, {
        0x52225du, 0xffu
    }, {
        0x52225eu, 0xffu
    }, {
        0x52225fu, 0xffu
    }, {
        0x52228cu, 0xffu
    }, {
        0x52228du, 0xffu
    }, {
        0x52228eu, 0xffu
    }, {
        0x52228fu, 0xffu
    }, {
        0x5222bcu, 0xffu
    }, {
        0x5222bdu, 0xffu
    }, {
        0x5222beu, 0xffu
    }, {
        0x5222bfu, 0xffu
    }, {
        0x5222ecu, 0xffu
    }, {
        0x5222edu, 0xffu
    }, {
        0x5222eeu, 0xffu
    }, {
        0x5222efu, 0xffu
    }, {
        0x52231cu, 0xffu
    }, {
        0x52231du, 0xffu
    }, {
        0x52231eu, 0xffu
    }, {
        0x52231fu, 0xffu
    }, {
        0x52234cu, 0xffu
    }, {
        0x52234du, 0xffu
    }, {
        0x52234eu, 0xffu
    }, {
        0x52234fu, 0xffu
    }, {
        0x52237cu, 0xffu
    }, {
        0x52237du, 0xffu
    }, {
        0x52237eu, 0xffu
    }, {
        0x52237fu, 0xffu
    }, {
        0x5223acu, 0xffu
    }, {
        0x5223adu, 0xffu
    }, {
        0x5223aeu, 0xffu
    }, {
        0x5223afu, 0xffu
    }, {
        0x5223dcu, 0xffu
    }, {
        0x5223ddu, 0xffu
    }, {
        0x5223deu, 0xffu
    }, {
        0x5223dfu, 0xffu
    }, {
        0x52240cu, 0xffu
    }, {
        0x52240du, 0xffu
    }, {
        0x52240eu, 0xffu
    }, {
        0x52240fu, 0xffu
    }, {
        0x52243cu, 0xffu
    }, {
        0x52243du, 0xffu
    }, {
        0x52243eu, 0xffu
    }, {
        0x52243fu, 0xffu
    }, {
        0x52246cu, 0xffu
    }, {
        0x52246du, 0xffu
    }, {
        0x52246eu, 0xffu
    }, {
        0x52246fu, 0xffu
    }, {
        0x52249cu, 0xffu
    }, {
        0x52249du, 0xffu
    }, {
        0x52249eu, 0xffu
    }, {
        0x52249fu, 0xffu
    }, {
        0x5224ccu, 0xffu
    }, {
        0x5224cdu, 0xffu
    }, {
        0x5224ceu, 0xffu
    }, {
        0x5224cfu, 0xffu
    }, {
        0x5224fcu, 0xffu
    }, {
        0x5224fdu, 0xffu
    }, {
        0x5224feu, 0xffu
    }, {
        0x5224ffu, 0xffu
    }, {
        0x52252cu, 0xffu
    }, {
        0x52252du, 0xffu
    }, {
        0x52252eu, 0xffu
    }, {
        0x52252fu, 0xffu
    }, {
        0x52255cu, 0xffu
    }, {
        0x52255du, 0xffu
    }, {
        0x52255eu, 0xffu
    }, {
        0x52255fu, 0xffu
    }, {
        0x52258cu, 0xffu
    }, {
        0x52258du, 0xffu
    }, {
        0x52258eu, 0xffu
    }, {
        0x52258fu, 0xffu
    }, {
        0x5225bcu, 0xffu
    }, {
        0x5225bdu, 0xffu
    }, {
        0x5225beu, 0xffu
    }, {
        0x5225bfu, 0xffu
    }, {
        0x5225ecu, 0xffu
    }, {
        0x5225edu, 0xffu
    }, {
        0x5225eeu, 0xffu
    }, {
        0x5225efu, 0xffu
    }, {
        0x52261cu, 0xffu
    }, {
        0x52261du, 0xffu
    }, {
        0x52261eu, 0xffu
    }, {
        0x52261fu, 0xffu
    }, {
        0x52264cu, 0xffu
    }, {
        0x52264du, 0xffu
    }, {
        0x52264eu, 0xffu
    }, {
        0x52264fu, 0xffu
    }, {
        0x52267cu, 0xffu
    }, {
        0x52267du, 0xffu
    }, {
        0x52267eu, 0xffu
    }, {
        0x52267fu, 0xffu
    }, {
        0x5226acu, 0xffu
    }, {
        0x5226adu, 0xffu
    }, {
        0x5226aeu, 0xffu
    }, {
        0x5226afu, 0xffu
    }, {
        0x5226dcu, 0xffu
    }, {
        0x5226ddu, 0xffu
    }, {
        0x5226deu, 0xffu
    }, {
        0x5226dfu, 0xffu
    }, {
        0x52270cu, 0xffu
    }, {
        0x52270du, 0xffu
    }, {
        0x52270eu, 0xffu
    }, {
        0x52270fu, 0xffu
    }, {
        0x52273cu, 0xffu
    }, {
        0x52273du, 0xffu
    }, {
        0x52273eu, 0xffu
    }, {
        0x52273fu, 0xffu
    }, {
        0x52276cu, 0xffu
    }, {
        0x52276du, 0xffu
    }, {
        0x52276eu, 0xffu
    }, {
        0x52276fu, 0xffu
    }, {
        0x52279cu, 0xffu
    }, {
        0x52279du, 0xffu
    }, {
        0x52279eu, 0xffu
    }, {
        0x52279fu, 0xffu
    }, {
        0x5227ccu, 0xffu
    }, {
        0x5227cdu, 0xffu
    }, {
        0x5227ceu, 0xffu
    }, {
        0x5227cfu, 0xffu
    }, {
        0x5227fcu, 0xffu
    }, {
        0x5227fdu, 0xffu
    }, {
        0x5227feu, 0xffu
    }, {
        0x5227ffu, 0xffu
    }, {
        0x52282cu, 0xffu
    }, {
        0x52282du, 0xffu
    }, {
        0x52282eu, 0xffu
    }, {
        0x52282fu, 0xffu
    }, {
        0x52285cu, 0xffu
    }, {
        0x52285du, 0xffu
    }, {
        0x52285eu, 0xffu
    }, {
        0x52285fu, 0xffu
    }, {
        0x52288cu, 0xffu
    }, {
        0x52288du, 0xffu
    }, {
        0x52288eu, 0xffu
    }, {
        0x52288fu, 0xffu
    }, {
        0x5228bcu, 0xffu
    }, {
        0x5228bdu, 0xffu
    }, {
        0x5228beu, 0xffu
    }, {
        0x5228bfu, 0xffu
    }, {
        0x5228ecu, 0xffu
    }, {
        0x5228edu, 0xffu
    }, {
        0x5228eeu, 0xffu
    }, {
        0x5228efu, 0xffu
    }, {
        0x52291cu, 0xffu
    }, {
        0x52291du, 0xffu
    }, {
        0x52291eu, 0xffu
    }, {
        0x52291fu, 0xffu
    }, {
        0x52294cu, 0xffu
    }, {
        0x52294du, 0xffu
    }, {
        0x52294eu, 0xffu
    }, {
        0x52294fu, 0xffu
    }, {
        0x52297cu, 0xffu
    }, {
        0x52297du, 0xffu
    }, {
        0x52297eu, 0xffu
    }, {
        0x52297fu, 0xffu
    }, {
        0x5229acu, 0xffu
    }, {
        0x5229adu, 0xffu
    }, {
        0x5229aeu, 0xffu
    }, {
        0x5229afu, 0xffu
    }, {
        0x5229dcu, 0xffu
    }, {
        0x5229ddu, 0xffu
    }, {
        0x5229deu, 0xffu
    }, {
        0x5229dfu, 0xffu
    }, {
        0x522a0cu, 0xffu
    }, {
        0x522a0du, 0xffu
    }, {
        0x522a0eu, 0xffu
    }, {
        0x522a0fu, 0xffu
    }, {
        0x522a3cu, 0xffu
    }, {
        0x522a3du, 0xffu
    }, {
        0x522a3eu, 0xffu
    }, {
        0x522a3fu, 0xffu
    }, {
        0x522a6cu, 0xffu
    }, {
        0x522a6du, 0xffu
    }, {
        0x522a6eu, 0xffu
    }, {
        0x522a6fu, 0xffu
    }, {
        0x522a9cu, 0xffu
    }, {
        0x522a9du, 0xffu
    }, {
        0x522a9eu, 0xffu
    }, {
        0x522a9fu, 0xffu
    }, {
        0x522accu, 0xffu
    }, {
        0x522acdu, 0xffu
    }, {
        0x522aceu, 0xffu
    }, {
        0x522acfu, 0xffu
    }, {
        0x522afcu, 0xffu
    }, {
        0x522afdu, 0xffu
    }, {
        0x522afeu, 0xffu
    }, {
        0x522affu, 0xffu
    }, {
        0x522b2cu, 0xffu
    }, {
        0x522b2du, 0xffu
    }, {
        0x522b2eu, 0xffu
    }, {
        0x522b2fu, 0xffu
    }, {
        0x522b5cu, 0xffu
    }, {
        0x522b5du, 0xffu
    }, {
        0x522b5eu, 0xffu
    }, {
        0x522b5fu, 0xffu
    }, {
        0x522b8cu, 0xffu
    }, {
        0x522b8du, 0xffu
    }, {
        0x522b8eu, 0xffu
    }, {
        0x522b8fu, 0xffu
    }, {
        0x522bbcu, 0xffu
    }, {
        0x522bbdu, 0xffu
    }, {
        0x522bbeu, 0xffu
    }, {
        0x522bbfu, 0xffu
    }, {
        0x522becu, 0xffu
    }, {
        0x522bedu, 0xffu
    }, {
        0x522beeu, 0xffu
    }, {
        0x522befu, 0xffu
    }, {
        0x522c1cu, 0xffu
    }, {
        0x522c1du, 0xffu
    }, {
        0x522c1eu, 0xffu
    }, {
        0x522c1fu, 0xffu
    }, {
        0x522c4cu, 0xffu
    }, {
        0x522c4du, 0xffu
    }, {
        0x522c4eu, 0xffu
    }, {
        0x522c4fu, 0xffu
    }, {
        0x522c7cu, 0xffu
    }, {
        0x522c7du, 0xffu
    }, {
        0x522c7eu, 0xffu
    }, {
        0x522c7fu, 0xffu
    }, {
        0x522cacu, 0xffu
    }, {
        0x522cadu, 0xffu
    }, {
        0x522caeu, 0xffu
    }, {
        0x522cafu, 0xffu
    }, {
        0x522cdcu, 0xffu
    }, {
        0x522cddu, 0xffu
    }, {
        0x522cdeu, 0xffu
    }, {
        0x522cdfu, 0xffu
    }, {
        0x522d0cu, 0xffu
    }, {
        0x522d0du, 0xffu
    }, {
        0x522d0eu, 0xffu
    }, {
        0x522d0fu, 0xffu
    }, {
        0x522d3cu, 0xffu
    }, {
        0x522d3du, 0xffu
    }, {
        0x522d3eu, 0xffu
    }, {
        0x522d3fu, 0xffu
    }, {
        0x522d6cu, 0xffu
    }, {
        0x522d6du, 0xffu
    }, {
        0x522d6eu, 0xffu
    }, {
        0x522d6fu, 0xffu
    }, {
        0x522d9cu, 0xffu
    }, {
        0x522d9du, 0xffu
    }, {
        0x522d9eu, 0xffu
    }, {
        0x522d9fu, 0xffu
    }, {
        0x522dccu, 0xffu
    }, {
        0x522dcdu, 0xffu
    }, {
        0x522dceu, 0xffu
    }, {
        0x522dcfu, 0xffu
    }, {
        0x522dfcu, 0xffu
    }, {
        0x522dfdu, 0xffu
    }, {
        0x522dfeu, 0xffu
    }, {
        0x522dffu, 0xffu
    }, {
        0x522e2cu, 0xffu
    }, {
        0x522e2du, 0xffu
    }, {
        0x522e2eu, 0xffu
    }, {
        0x522e2fu, 0xffu
    }, {
        0x522e5cu, 0xffu
    }, {
        0x522e5du, 0xffu
    }, {
        0x522e5eu, 0xffu
    }, {
        0x522e5fu, 0xffu
    }, {
        0x522e8cu, 0xffu
    }, {
        0x522e8du, 0xffu
    }, {
        0x522e8eu, 0xffu
    }, {
        0x522e8fu, 0xffu
    }, {
        0x522ebcu, 0xffu
    }, {
        0x522ebdu, 0xffu
    }, {
        0x522ebeu, 0xffu
    }, {
        0x522ebfu, 0xffu
    }, {
        0x522eecu, 0xffu
    }, {
        0x522eedu, 0xffu
    }, {
        0x522eeeu, 0xffu
    }, {
        0x522eefu, 0xffu
    }, {
        0x522f1cu, 0xffu
    }, {
        0x522f1du, 0xffu
    }, {
        0x522f1eu, 0xffu
    }, {
        0x522f1fu, 0xffu
    }, {
        0x522f4cu, 0xffu
    }, {
        0x522f4du, 0xffu
    }, {
        0x522f4eu, 0xffu
    }, {
        0x522f4fu, 0xffu
    }, {
        0x522f7cu, 0xffu
    }, {
        0x522f7du, 0xffu
    }, {
        0x522f7eu, 0xffu
    }, {
        0x522f7fu, 0xffu
    }, {
        0x522facu, 0xffu
    }, {
        0x522fadu, 0xffu
    }, {
        0x522faeu, 0xffu
    }, {
        0x522fafu, 0xffu
    }, {
        0x522fdcu, 0xffu
    }, {
        0x522fddu, 0xffu
    }, {
        0x522fdeu, 0xffu
    }, {
        0x522fdfu, 0xffu
    }, {
        0x52300cu, 0xffu
    }, {
        0x52300du, 0xffu
    }, {
        0x52300eu, 0xffu
    }, {
        0x52300fu, 0xffu
    }, {
        0x52303cu, 0xffu
    }, {
        0x52303du, 0xffu
    }, {
        0x52303eu, 0xffu
    }, {
        0x52303fu, 0xffu
    }, {
        0x52306cu, 0xffu
    }, {
        0x52306du, 0xffu
    }, {
        0x52306eu, 0xffu
    }, {
        0x52306fu, 0xffu
    }, {
        0x52309cu, 0xffu
    }, {
        0x52309du, 0xffu
    }, {
        0x52309eu, 0xffu
    }, {
        0x52309fu, 0xffu
    }, {
        0x5230ccu, 0xffu
    }, {
        0x5230cdu, 0xffu
    }, {
        0x5230ceu, 0xffu
    }, {
        0x5230cfu, 0xffu
    }, {
        0x5230fcu, 0xffu
    }, {
        0x5230fdu, 0xffu
    }, {
        0x5230feu, 0xffu
    }, {
        0x5230ffu, 0xffu
    }, {
        0x52312cu, 0xffu
    }, {
        0x52312du, 0xffu
    }, {
        0x52312eu, 0xffu
    }, {
        0x52312fu, 0xffu
    }, {
        0x52315cu, 0xffu
    }, {
        0x52315du, 0xffu
    }, {
        0x52315eu, 0xffu
    }, {
        0x52315fu, 0xffu
    }, {
        0x52318cu, 0xffu
    }, {
        0x52318du, 0xffu
    }, {
        0x52318eu, 0xffu
    }, {
        0x52318fu, 0xffu
    }, {
        0x5231bcu, 0xffu
    }, {
        0x5231bdu, 0xffu
    }, {
        0x5231beu, 0xffu
    }, {
        0x5231bfu, 0xffu
    }, {
        0x5231ecu, 0xffu
    }, {
        0x5231edu, 0xffu
    }, {
        0x5231eeu, 0xffu
    }, {
        0x5231efu, 0xffu
    }, {
        0x52321cu, 0xffu
    }, {
        0x52321du, 0xffu
    }, {
        0x52321eu, 0xffu
    }, {
        0x52321fu, 0xffu
    }, {
        0x52324cu, 0xffu
    }, {
        0x52324du, 0xffu
    }, {
        0x52324eu, 0xffu
    }, {
        0x52324fu, 0xffu
    }, {
        0x52327cu, 0xffu
    }, {
        0x52327du, 0xffu
    }, {
        0x52327eu, 0xffu
    }, {
        0x52327fu, 0xffu
    }, {
        0x5232acu, 0xffu
    }, {
        0x5232adu, 0xffu
    }, {
        0x5232aeu, 0xffu
    }, {
        0x5232afu, 0xffu
    }, {
        0x5232dcu, 0xffu
    }, {
        0x5232ddu, 0xffu
    }, {
        0x5232deu, 0xffu
    }, {
        0x5232dfu, 0xffu
    }, {
        0x52330cu, 0xffu
    }, {
        0x52330du, 0xffu
    }, {
        0x52330eu, 0xffu
    }, {
        0x52330fu, 0xffu
    }, {
        0x52333cu, 0xffu
    }, {
        0x52333du, 0xffu
    }, {
        0x52333eu, 0xffu
    }, {
        0x52333fu, 0xffu
    }, {
        0x52336cu, 0xffu
    }, {
        0x52336du, 0xffu
    }, {
        0x52336eu, 0xffu
    }, {
        0x52336fu, 0xffu
    }, {
        0x52339cu, 0xffu
    }, {
        0x52339du, 0xffu
    }, {
        0x52339eu, 0xffu
    }, {
        0x52339fu, 0xffu
    }, {
        0x5233ccu, 0xffu
    }, {
        0x5233cdu, 0xffu
    }, {
        0x5233ceu, 0xffu
    }, {
        0x5233cfu, 0xffu
    }, {
        0x5233fcu, 0xffu
    }, {
        0x5233fdu, 0xffu
    }, {
        0x5233feu, 0xffu
    }, {
        0x5233ffu, 0xffu
    }, {
        0x52342cu, 0xffu
    }, {
        0x52342du, 0xffu
    }, {
        0x52342eu, 0xffu
    }, {
        0x52342fu, 0xffu
    }, {
        0x52345cu, 0xffu
    }, {
        0x52345du, 0xffu
    }, {
        0x52345eu, 0xffu
    }, {
        0x52345fu, 0xffu
    }, {
        0x52348cu, 0xffu
    }, {
        0x52348du, 0xffu
    }, {
        0x52348eu, 0xffu
    }, {
        0x52348fu, 0xffu
    }, {
        0x5234bcu, 0xffu
    }, {
        0x5234bdu, 0xffu
    }, {
        0x5234beu, 0xffu
    }, {
        0x5234bfu, 0xffu
    }, {
        0x5234ecu, 0xffu
    }, {
        0x5234edu, 0xffu
    }, {
        0x5234eeu, 0xffu
    }, {
        0x5234efu, 0xffu
    }, {
        0x52351cu, 0xffu
    }, {
        0x52351du, 0xffu
    }, {
        0x52351eu, 0xffu
    }, {
        0x52351fu, 0xffu
    }, {
        0x52354cu, 0xffu
    }, {
        0x52354du, 0xffu
    }, {
        0x52354eu, 0xffu
    }, {
        0x52354fu, 0xffu
    }, {
        0x52357cu, 0xffu
    }, {
        0x52357du, 0xffu
    }, {
        0x52357eu, 0xffu
    }, {
        0x52357fu, 0xffu
    }, {
        0x5235acu, 0xffu
    }, {
        0x5235adu, 0xffu
    }, {
        0x5235aeu, 0xffu
    }, {
        0x5235afu, 0xffu
    }, {
        0x5235dcu, 0xffu
    }, {
        0x5235ddu, 0xffu
    }, {
        0x5235deu, 0xffu
    }, {
        0x5235dfu, 0xffu
    }, {
        0x52360cu, 0x7bu
    }, {
        0x52360du, 0x0u
    }, {
        0x52360eu, 0x0u
    }, {
        0x52360fu, 0x0u
    }, {
        0x523609u, 0x5u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_35[] = {
    {
        0xff4u, 0x88u
    }, {
        0xff5u, 0x77u
    }, {
        0xff6u, 0x66u
    }, {
        0xff7u, 0x55u
    }, {
        0xff8u, 0x77u
    }, {
        0xff9u, 0x66u
    }, {
        0xffau, 0x55u
    }, {
        0xffbu, 0x44u
    }, {
        0xffcu, 0x1u
    }, {
        0xffdu, 0xefu
    }, {
        0xffeu, 0xcdu
    }, {
        0xfffu, 0xabu
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_36[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x7bu
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x0u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x5u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x0u
    }, {
        0x100bu, 0x0u
    }, {
        0x521a1cu, 0xffu
    }, {
        0x521a1du, 0xffu
    }, {
        0x521a1eu, 0xffu
    }, {
        0x521a1fu, 0xffu
    }, {
        0x521a4cu, 0xffu
    }, {
        0x521a4du, 0xffu
    }, {
        0x521a4eu, 0xffu
    }, {
        0x521a4fu, 0xffu
    }, {
        0x521a7cu, 0xffu
    }, {
        0x521a7du, 0xffu
    }, {
        0x521a7eu, 0xffu
    }, {
        0x521a7fu, 0xffu
    }, {
        0x521aacu, 0xffu
    }, {
        0x521aadu, 0xffu
    }, {
        0x521aaeu, 0xffu
    }, {
        0x521aafu, 0xffu
    }, {
        0x521adcu, 0xffu
    }, {
        0x521addu, 0xffu
    }, {
        0x521adeu, 0xffu
    }, {
        0x521adfu, 0xffu
    }, {
        0x521b0cu, 0xffu
    }, {
        0x521b0du, 0xffu
    }, {
        0x521b0eu, 0xffu
    }, {
        0x521b0fu, 0xffu
    }, {
        0x521b3cu, 0xffu
    }, {
        0x521b3du, 0xffu
    }, {
        0x521b3eu, 0xffu
    }, {
        0x521b3fu, 0xffu
    }, {
        0x521b6cu, 0xffu
    }, {
        0x521b6du, 0xffu
    }, {
        0x521b6eu, 0xffu
    }, {
        0x521b6fu, 0xffu
    }, {
        0x521b9cu, 0xffu
    }, {
        0x521b9du, 0xffu
    }, {
        0x521b9eu, 0xffu
    }, {
        0x521b9fu, 0xffu
    }, {
        0x521bccu, 0xffu
    }, {
        0x521bcdu, 0xffu
    }, {
        0x521bceu, 0xffu
    }, {
        0x521bcfu, 0xffu
    }, {
        0x521bfcu, 0xffu
    }, {
        0x521bfdu, 0xffu
    }, {
        0x521bfeu, 0xffu
    }, {
        0x521bffu, 0xffu
    }, {
        0x521c2cu, 0xffu
    }, {
        0x521c2du, 0xffu
    }, {
        0x521c2eu, 0xffu
    }, {
        0x521c2fu, 0xffu
    }, {
        0x521c5cu, 0xffu
    }, {
        0x521c5du, 0xffu
    }, {
        0x521c5eu, 0xffu
    }, {
        0x521c5fu, 0xffu
    }, {
        0x521c8cu, 0xffu
    }, {
        0x521c8du, 0xffu
    }, {
        0x521c8eu, 0xffu
    }, {
        0x521c8fu, 0xffu
    }, {
        0x521cbcu, 0xffu
    }, {
        0x521cbdu, 0xffu
    }, {
        0x521cbeu, 0xffu
    }, {
        0x521cbfu, 0xffu
    }, {
        0x521cecu, 0xffu
    }, {
        0x521cedu, 0xffu
    }, {
        0x521ceeu, 0xffu
    }, {
        0x521cefu, 0xffu
    }, {
        0x521d1cu, 0xffu
    }, {
        0x521d1du, 0xffu
    }, {
        0x521d1eu, 0xffu
    }, {
        0x521d1fu, 0xffu
    }, {
        0x521d4cu, 0xffu
    }, {
        0x521d4du, 0xffu
    }, {
        0x521d4eu, 0xffu
    }, {
        0x521d4fu, 0xffu
    }, {
        0x521d7cu, 0xffu
    }, {
        0x521d7du, 0xffu
    }, {
        0x521d7eu, 0xffu
    }, {
        0x521d7fu, 0xffu
    }, {
        0x521dacu, 0xffu
    }, {
        0x521dadu, 0xffu
    }, {
        0x521daeu, 0xffu
    }, {
        0x521dafu, 0xffu
    }, {
        0x521ddcu, 0xffu
    }, {
        0x521dddu, 0xffu
    }, {
        0x521ddeu, 0xffu
    }, {
        0x521ddfu, 0xffu
    }, {
        0x521e0cu, 0xffu
    }, {
        0x521e0du, 0xffu
    }, {
        0x521e0eu, 0xffu
    }, {
        0x521e0fu, 0xffu
    }, {
        0x521e3cu, 0xffu
    }, {
        0x521e3du, 0xffu
    }, {
        0x521e3eu, 0xffu
    }, {
        0x521e3fu, 0xffu
    }, {
        0x521e6cu, 0xffu
    }, {
        0x521e6du, 0xffu
    }, {
        0x521e6eu, 0xffu
    }, {
        0x521e6fu, 0xffu
    }, {
        0x521e9cu, 0xffu
    }, {
        0x521e9du, 0xffu
    }, {
        0x521e9eu, 0xffu
    }, {
        0x521e9fu, 0xffu
    }, {
        0x521eccu, 0xffu
    }, {
        0x521ecdu, 0xffu
    }, {
        0x521eceu, 0xffu
    }, {
        0x521ecfu, 0xffu
    }, {
        0x521efcu, 0xffu
    }, {
        0x521efdu, 0xffu
    }, {
        0x521efeu, 0xffu
    }, {
        0x521effu, 0xffu
    }, {
        0x521f2cu, 0xffu
    }, {
        0x521f2du, 0xffu
    }, {
        0x521f2eu, 0xffu
    }, {
        0x521f2fu, 0xffu
    }, {
        0x521f5cu, 0xffu
    }, {
        0x521f5du, 0xffu
    }, {
        0x521f5eu, 0xffu
    }, {
        0x521f5fu, 0xffu
    }, {
        0x521f8cu, 0xffu
    }, {
        0x521f8du, 0xffu
    }, {
        0x521f8eu, 0xffu
    }, {
        0x521f8fu, 0xffu
    }, {
        0x521fbcu, 0xffu
    }, {
        0x521fbdu, 0xffu
    }, {
        0x521fbeu, 0xffu
    }, {
        0x521fbfu, 0xffu
    }, {
        0x521fecu, 0xffu
    }, {
        0x521fedu, 0xffu
    }, {
        0x521feeu, 0xffu
    }, {
        0x521fefu, 0xffu
    }, {
        0x52201cu, 0xffu
    }, {
        0x52201du, 0xffu
    }, {
        0x52201eu, 0xffu
    }, {
        0x52201fu, 0xffu
    }, {
        0x52204cu, 0xffu
    }, {
        0x52204du, 0xffu
    }, {
        0x52204eu, 0xffu
    }, {
        0x52204fu, 0xffu
    }, {
        0x52207cu, 0xffu
    }, {
        0x52207du, 0xffu
    }, {
        0x52207eu, 0xffu
    }, {
        0x52207fu, 0xffu
    }, {
        0x5220acu, 0xffu
    }, {
        0x5220adu, 0xffu
    }, {
        0x5220aeu, 0xffu
    }, {
        0x5220afu, 0xffu
    }, {
        0x5220dcu, 0xffu
    }, {
        0x5220ddu, 0xffu
    }, {
        0x5220deu, 0xffu
    }, {
        0x5220dfu, 0xffu
    }, {
        0x52210cu, 0xffu
    }, {
        0x52210du, 0xffu
    }, {
        0x52210eu, 0xffu
    }, {
        0x52210fu, 0xffu
    }, {
        0x52213cu, 0xffu
    }, {
        0x52213du, 0xffu
    }, {
        0x52213eu, 0xffu
    }, {
        0x52213fu, 0xffu
    }, {
        0x52216cu, 0xffu
    }, {
        0x52216du, 0xffu
    }, {
        0x52216eu, 0xffu
    }, {
        0x52216fu, 0xffu
    }, {
        0x52219cu, 0xffu
    }, {
        0x52219du, 0xffu
    }, {
        0x52219eu, 0xffu
    }, {
        0x52219fu, 0xffu
    }, {
        0x5221ccu, 0xffu
    }, {
        0x5221cdu, 0xffu
    }, {
        0x5221ceu, 0xffu
    }, {
        0x5221cfu, 0xffu
    }, {
        0x5221fcu, 0xffu
    }, {
        0x5221fdu, 0xffu
    }, {
        0x5221feu, 0xffu
    }, {
        0x5221ffu, 0xffu
    }, {
        0x52222cu, 0xffu
    }, {
        0x52222du, 0xffu
    }, {
        0x52222eu, 0xffu
    }, {
        0x52222fu, 0xffu
    }, {
        0x52225cu, 0xffu
    }, {
        0x52225du, 0xffu
    }, {
        0x52225eu, 0xffu
    }, {
        0x52225fu, 0xffu
    }, {
        0x52228cu, 0xffu
    }, {
        0x52228du, 0xffu
    }, {
        0x52228eu, 0xffu
    }, {
        0x52228fu, 0xffu
    }, {
        0x5222bcu, 0xffu
    }, {
        0x5222bdu, 0xffu
    }, {
        0x5222beu, 0xffu
    }, {
        0x5222bfu, 0xffu
    }, {
        0x5222ecu, 0xffu
    }, {
        0x5222edu, 0xffu
    }, {
        0x5222eeu, 0xffu
    }, {
        0x5222efu, 0xffu
    }, {
        0x52231cu, 0xffu
    }, {
        0x52231du, 0xffu
    }, {
        0x52231eu, 0xffu
    }, {
        0x52231fu, 0xffu
    }, {
        0x52234cu, 0xffu
    }, {
        0x52234du, 0xffu
    }, {
        0x52234eu, 0xffu
    }, {
        0x52234fu, 0xffu
    }, {
        0x52237cu, 0xffu
    }, {
        0x52237du, 0xffu
    }, {
        0x52237eu, 0xffu
    }, {
        0x52237fu, 0xffu
    }, {
        0x5223acu, 0xffu
    }, {
        0x5223adu, 0xffu
    }, {
        0x5223aeu, 0xffu
    }, {
        0x5223afu, 0xffu
    }, {
        0x5223dcu, 0xffu
    }, {
        0x5223ddu, 0xffu
    }, {
        0x5223deu, 0xffu
    }, {
        0x5223dfu, 0xffu
    }, {
        0x52240cu, 0xffu
    }, {
        0x52240du, 0xffu
    }, {
        0x52240eu, 0xffu
    }, {
        0x52240fu, 0xffu
    }, {
        0x52243cu, 0xffu
    }, {
        0x52243du, 0xffu
    }, {
        0x52243eu, 0xffu
    }, {
        0x52243fu, 0xffu
    }, {
        0x52246cu, 0xffu
    }, {
        0x52246du, 0xffu
    }, {
        0x52246eu, 0xffu
    }, {
        0x52246fu, 0xffu
    }, {
        0x52249cu, 0xffu
    }, {
        0x52249du, 0xffu
    }, {
        0x52249eu, 0xffu
    }, {
        0x52249fu, 0xffu
    }, {
        0x5224ccu, 0xffu
    }, {
        0x5224cdu, 0xffu
    }, {
        0x5224ceu, 0xffu
    }, {
        0x5224cfu, 0xffu
    }, {
        0x5224fcu, 0xffu
    }, {
        0x5224fdu, 0xffu
    }, {
        0x5224feu, 0xffu
    }, {
        0x5224ffu, 0xffu
    }, {
        0x52252cu, 0xffu
    }, {
        0x52252du, 0xffu
    }, {
        0x52252eu, 0xffu
    }, {
        0x52252fu, 0xffu
    }, {
        0x52255cu, 0xffu
    }, {
        0x52255du, 0xffu
    }, {
        0x52255eu, 0xffu
    }, {
        0x52255fu, 0xffu
    }, {
        0x52258cu, 0xffu
    }, {
        0x52258du, 0xffu
    }, {
        0x52258eu, 0xffu
    }, {
        0x52258fu, 0xffu
    }, {
        0x5225bcu, 0xffu
    }, {
        0x5225bdu, 0xffu
    }, {
        0x5225beu, 0xffu
    }, {
        0x5225bfu, 0xffu
    }, {
        0x5225ecu, 0xffu
    }, {
        0x5225edu, 0xffu
    }, {
        0x5225eeu, 0xffu
    }, {
        0x5225efu, 0xffu
    }, {
        0x52261cu, 0xffu
    }, {
        0x52261du, 0xffu
    }, {
        0x52261eu, 0xffu
    }, {
        0x52261fu, 0xffu
    }, {
        0x52264cu, 0xffu
    }, {
        0x52264du, 0xffu
    }, {
        0x52264eu, 0xffu
    }, {
        0x52264fu, 0xffu
    }, {
        0x52267cu, 0xffu
    }, {
        0x52267du, 0xffu
    }, {
        0x52267eu, 0xffu
    }, {
        0x52267fu, 0xffu
    }, {
        0x5226acu, 0xffu
    }, {
        0x5226adu, 0xffu
    }, {
        0x5226aeu, 0xffu
    }, {
        0x5226afu, 0xffu
    }, {
        0x5226dcu, 0xffu
    }, {
        0x5226ddu, 0xffu
    }, {
        0x5226deu, 0xffu
    }, {
        0x5226dfu, 0xffu
    }, {
        0x52270cu, 0xffu
    }, {
        0x52270du, 0xffu
    }, {
        0x52270eu, 0xffu
    }, {
        0x52270fu, 0xffu
    }, {
        0x52273cu, 0xffu
    }, {
        0x52273du, 0xffu
    }, {
        0x52273eu, 0xffu
    }, {
        0x52273fu, 0xffu
    }, {
        0x52276cu, 0xffu
    }, {
        0x52276du, 0xffu
    }, {
        0x52276eu, 0xffu
    }, {
        0x52276fu, 0xffu
    }, {
        0x52279cu, 0xffu
    }, {
        0x52279du, 0xffu
    }, {
        0x52279eu, 0xffu
    }, {
        0x52279fu, 0xffu
    }, {
        0x5227ccu, 0xffu
    }, {
        0x5227cdu, 0xffu
    }, {
        0x5227ceu, 0xffu
    }, {
        0x5227cfu, 0xffu
    }, {
        0x5227fcu, 0xffu
    }, {
        0x5227fdu, 0xffu
    }, {
        0x5227feu, 0xffu
    }, {
        0x5227ffu, 0xffu
    }, {
        0x52282cu, 0xffu
    }, {
        0x52282du, 0xffu
    }, {
        0x52282eu, 0xffu
    }, {
        0x52282fu, 0xffu
    }, {
        0x52285cu, 0xffu
    }, {
        0x52285du, 0xffu
    }, {
        0x52285eu, 0xffu
    }, {
        0x52285fu, 0xffu
    }, {
        0x52288cu, 0xffu
    }, {
        0x52288du, 0xffu
    }, {
        0x52288eu, 0xffu
    }, {
        0x52288fu, 0xffu
    }, {
        0x5228bcu, 0xffu
    }, {
        0x5228bdu, 0xffu
    }, {
        0x5228beu, 0xffu
    }, {
        0x5228bfu, 0xffu
    }, {
        0x5228ecu, 0xffu
    }, {
        0x5228edu, 0xffu
    }, {
        0x5228eeu, 0xffu
    }, {
        0x5228efu, 0xffu
    }, {
        0x52291cu, 0xffu
    }, {
        0x52291du, 0xffu
    }, {
        0x52291eu, 0xffu
    }, {
        0x52291fu, 0xffu
    }, {
        0x52294cu, 0xffu
    }, {
        0x52294du, 0xffu
    }, {
        0x52294eu, 0xffu
    }, {
        0x52294fu, 0xffu
    }, {
        0x52297cu, 0xffu
    }, {
        0x52297du, 0xffu
    }, {
        0x52297eu, 0xffu
    }, {
        0x52297fu, 0xffu
    }, {
        0x5229acu, 0xffu
    }, {
        0x5229adu, 0xffu
    }, {
        0x5229aeu, 0xffu
    }, {
        0x5229afu, 0xffu
    }, {
        0x5229dcu, 0xffu
    }, {
        0x5229ddu, 0xffu
    }, {
        0x5229deu, 0xffu
    }, {
        0x5229dfu, 0xffu
    }, {
        0x522a0cu, 0xffu
    }, {
        0x522a0du, 0xffu
    }, {
        0x522a0eu, 0xffu
    }, {
        0x522a0fu, 0xffu
    }, {
        0x522a3cu, 0xffu
    }, {
        0x522a3du, 0xffu
    }, {
        0x522a3eu, 0xffu
    }, {
        0x522a3fu, 0xffu
    }, {
        0x522a6cu, 0xffu
    }, {
        0x522a6du, 0xffu
    }, {
        0x522a6eu, 0xffu
    }, {
        0x522a6fu, 0xffu
    }, {
        0x522a9cu, 0xffu
    }, {
        0x522a9du, 0xffu
    }, {
        0x522a9eu, 0xffu
    }, {
        0x522a9fu, 0xffu
    }, {
        0x522accu, 0xffu
    }, {
        0x522acdu, 0xffu
    }, {
        0x522aceu, 0xffu
    }, {
        0x522acfu, 0xffu
    }, {
        0x522afcu, 0xffu
    }, {
        0x522afdu, 0xffu
    }, {
        0x522afeu, 0xffu
    }, {
        0x522affu, 0xffu
    }, {
        0x522b2cu, 0xffu
    }, {
        0x522b2du, 0xffu
    }, {
        0x522b2eu, 0xffu
    }, {
        0x522b2fu, 0xffu
    }, {
        0x522b5cu, 0xffu
    }, {
        0x522b5du, 0xffu
    }, {
        0x522b5eu, 0xffu
    }, {
        0x522b5fu, 0xffu
    }, {
        0x522b8cu, 0xffu
    }, {
        0x522b8du, 0xffu
    }, {
        0x522b8eu, 0xffu
    }, {
        0x522b8fu, 0xffu
    }, {
        0x522bbcu, 0xffu
    }, {
        0x522bbdu, 0xffu
    }, {
        0x522bbeu, 0xffu
    }, {
        0x522bbfu, 0xffu
    }, {
        0x522becu, 0xffu
    }, {
        0x522bedu, 0xffu
    }, {
        0x522beeu, 0xffu
    }, {
        0x522befu, 0xffu
    }, {
        0x522c1cu, 0xffu
    }, {
        0x522c1du, 0xffu
    }, {
        0x522c1eu, 0xffu
    }, {
        0x522c1fu, 0xffu
    }, {
        0x522c4cu, 0xffu
    }, {
        0x522c4du, 0xffu
    }, {
        0x522c4eu, 0xffu
    }, {
        0x522c4fu, 0xffu
    }, {
        0x522c7cu, 0xffu
    }, {
        0x522c7du, 0xffu
    }, {
        0x522c7eu, 0xffu
    }, {
        0x522c7fu, 0xffu
    }, {
        0x522cacu, 0xffu
    }, {
        0x522cadu, 0xffu
    }, {
        0x522caeu, 0xffu
    }, {
        0x522cafu, 0xffu
    }, {
        0x522cdcu, 0xffu
    }, {
        0x522cddu, 0xffu
    }, {
        0x522cdeu, 0xffu
    }, {
        0x522cdfu, 0xffu
    }, {
        0x522d0cu, 0xffu
    }, {
        0x522d0du, 0xffu
    }, {
        0x522d0eu, 0xffu
    }, {
        0x522d0fu, 0xffu
    }, {
        0x522d3cu, 0xffu
    }, {
        0x522d3du, 0xffu
    }, {
        0x522d3eu, 0xffu
    }, {
        0x522d3fu, 0xffu
    }, {
        0x522d6cu, 0xffu
    }, {
        0x522d6du, 0xffu
    }, {
        0x522d6eu, 0xffu
    }, {
        0x522d6fu, 0xffu
    }, {
        0x522d9cu, 0xffu
    }, {
        0x522d9du, 0xffu
    }, {
        0x522d9eu, 0xffu
    }, {
        0x522d9fu, 0xffu
    }, {
        0x522dccu, 0xffu
    }, {
        0x522dcdu, 0xffu
    }, {
        0x522dceu, 0xffu
    }, {
        0x522dcfu, 0xffu
    }, {
        0x522dfcu, 0xffu
    }, {
        0x522dfdu, 0xffu
    }, {
        0x522dfeu, 0xffu
    }, {
        0x522dffu, 0xffu
    }, {
        0x522e2cu, 0xffu
    }, {
        0x522e2du, 0xffu
    }, {
        0x522e2eu, 0xffu
    }, {
        0x522e2fu, 0xffu
    }, {
        0x522e5cu, 0xffu
    }, {
        0x522e5du, 0xffu
    }, {
        0x522e5eu, 0xffu
    }, {
        0x522e5fu, 0xffu
    }, {
        0x522e8cu, 0xffu
    }, {
        0x522e8du, 0xffu
    }, {
        0x522e8eu, 0xffu
    }, {
        0x522e8fu, 0xffu
    }, {
        0x522ebcu, 0xffu
    }, {
        0x522ebdu, 0xffu
    }, {
        0x522ebeu, 0xffu
    }, {
        0x522ebfu, 0xffu
    }, {
        0x522eecu, 0xffu
    }, {
        0x522eedu, 0xffu
    }, {
        0x522eeeu, 0xffu
    }, {
        0x522eefu, 0xffu
    }, {
        0x522f1cu, 0xffu
    }, {
        0x522f1du, 0xffu
    }, {
        0x522f1eu, 0xffu
    }, {
        0x522f1fu, 0xffu
    }, {
        0x522f4cu, 0xffu
    }, {
        0x522f4du, 0xffu
    }, {
        0x522f4eu, 0xffu
    }, {
        0x522f4fu, 0xffu
    }, {
        0x522f7cu, 0xffu
    }, {
        0x522f7du, 0xffu
    }, {
        0x522f7eu, 0xffu
    }, {
        0x522f7fu, 0xffu
    }, {
        0x522facu, 0xffu
    }, {
        0x522fadu, 0xffu
    }, {
        0x522faeu, 0xffu
    }, {
        0x522fafu, 0xffu
    }, {
        0x522fdcu, 0xffu
    }, {
        0x522fddu, 0xffu
    }, {
        0x522fdeu, 0xffu
    }, {
        0x522fdfu, 0xffu
    }, {
        0x52300cu, 0xffu
    }, {
        0x52300du, 0xffu
    }, {
        0x52300eu, 0xffu
    }, {
        0x52300fu, 0xffu
    }, {
        0x52303cu, 0xffu
    }, {
        0x52303du, 0xffu
    }, {
        0x52303eu, 0xffu
    }, {
        0x52303fu, 0xffu
    }, {
        0x52306cu, 0xffu
    }, {
        0x52306du, 0xffu
    }, {
        0x52306eu, 0xffu
    }, {
        0x52306fu, 0xffu
    }, {
        0x52309cu, 0xffu
    }, {
        0x52309du, 0xffu
    }, {
        0x52309eu, 0xffu
    }, {
        0x52309fu, 0xffu
    }, {
        0x5230ccu, 0xffu
    }, {
        0x5230cdu, 0xffu
    }, {
        0x5230ceu, 0xffu
    }, {
        0x5230cfu, 0xffu
    }, {
        0x5230fcu, 0xffu
    }, {
        0x5230fdu, 0xffu
    }, {
        0x5230feu, 0xffu
    }, {
        0x5230ffu, 0xffu
    }, {
        0x52312cu, 0xffu
    }, {
        0x52312du, 0xffu
    }, {
        0x52312eu, 0xffu
    }, {
        0x52312fu, 0xffu
    }, {
        0x52315cu, 0xffu
    }, {
        0x52315du, 0xffu
    }, {
        0x52315eu, 0xffu
    }, {
        0x52315fu, 0xffu
    }, {
        0x52318cu, 0xffu
    }, {
        0x52318du, 0xffu
    }, {
        0x52318eu, 0xffu
    }, {
        0x52318fu, 0xffu
    }, {
        0x5231bcu, 0xffu
    }, {
        0x5231bdu, 0xffu
    }, {
        0x5231beu, 0xffu
    }, {
        0x5231bfu, 0xffu
    }, {
        0x5231ecu, 0xffu
    }, {
        0x5231edu, 0xffu
    }, {
        0x5231eeu, 0xffu
    }, {
        0x5231efu, 0xffu
    }, {
        0x52321cu, 0xffu
    }, {
        0x52321du, 0xffu
    }, {
        0x52321eu, 0xffu
    }, {
        0x52321fu, 0xffu
    }, {
        0x52324cu, 0xffu
    }, {
        0x52324du, 0xffu
    }, {
        0x52324eu, 0xffu
    }, {
        0x52324fu, 0xffu
    }, {
        0x52327cu, 0xffu
    }, {
        0x52327du, 0xffu
    }, {
        0x52327eu, 0xffu
    }, {
        0x52327fu, 0xffu
    }, {
        0x5232acu, 0xffu
    }, {
        0x5232adu, 0xffu
    }, {
        0x5232aeu, 0xffu
    }, {
        0x5232afu, 0xffu
    }, {
        0x5232dcu, 0xffu
    }, {
        0x5232ddu, 0xffu
    }, {
        0x5232deu, 0xffu
    }, {
        0x5232dfu, 0xffu
    }, {
        0x52330cu, 0xffu
    }, {
        0x52330du, 0xffu
    }, {
        0x52330eu, 0xffu
    }, {
        0x52330fu, 0xffu
    }, {
        0x52333cu, 0xffu
    }, {
        0x52333du, 0xffu
    }, {
        0x52333eu, 0xffu
    }, {
        0x52333fu, 0xffu
    }, {
        0x52336cu, 0xffu
    }, {
        0x52336du, 0xffu
    }, {
        0x52336eu, 0xffu
    }, {
        0x52336fu, 0xffu
    }, {
        0x52339cu, 0xffu
    }, {
        0x52339du, 0xffu
    }, {
        0x52339eu, 0xffu
    }, {
        0x52339fu, 0xffu
    }, {
        0x5233ccu, 0xffu
    }, {
        0x5233cdu, 0xffu
    }, {
        0x5233ceu, 0xffu
    }, {
        0x5233cfu, 0xffu
    }, {
        0x5233fcu, 0xffu
    }, {
        0x5233fdu, 0xffu
    }, {
        0x5233feu, 0xffu
    }, {
        0x5233ffu, 0xffu
    }, {
        0x52342cu, 0xffu
    }, {
        0x52342du, 0xffu
    }, {
        0x52342eu, 0xffu
    }, {
        0x52342fu, 0xffu
    }, {
        0x52345cu, 0xffu
    }, {
        0x52345du, 0xffu
    }, {
        0x52345eu, 0xffu
    }, {
        0x52345fu, 0xffu
    }, {
        0x52348cu, 0xffu
    }, {
        0x52348du, 0xffu
    }, {
        0x52348eu, 0xffu
    }, {
        0x52348fu, 0xffu
    }, {
        0x5234bcu, 0xffu
    }, {
        0x5234bdu, 0xffu
    }, {
        0x5234beu, 0xffu
    }, {
        0x5234bfu, 0xffu
    }, {
        0x5234ecu, 0xffu
    }, {
        0x5234edu, 0xffu
    }, {
        0x5234eeu, 0xffu
    }, {
        0x5234efu, 0xffu
    }, {
        0x52351cu, 0xffu
    }, {
        0x52351du, 0xffu
    }, {
        0x52351eu, 0xffu
    }, {
        0x52351fu, 0xffu
    }, {
        0x52354cu, 0xffu
    }, {
        0x52354du, 0xffu
    }, {
        0x52354eu, 0xffu
    }, {
        0x52354fu, 0xffu
    }, {
        0x52357cu, 0xffu
    }, {
        0x52357du, 0xffu
    }, {
        0x52357eu, 0xffu
    }, {
        0x52357fu, 0xffu
    }, {
        0x5235acu, 0xffu
    }, {
        0x5235adu, 0xffu
    }, {
        0x5235aeu, 0xffu
    }, {
        0x5235afu, 0xffu
    }, {
        0x5235dcu, 0xffu
    }, {
        0x5235ddu, 0xffu
    }, {
        0x5235deu, 0xffu
    }, {
        0x5235dfu, 0xffu
    }, {
        0x52360cu, 0xffu
    }, {
        0x52360du, 0xffu
    }, {
        0x52360eu, 0xffu
    }, {
        0x52360fu, 0xffu
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_36[] = {
    {
        0xff4u, 0x88u
    }, {
        0xff5u, 0x77u
    }, {
        0xff6u, 0x66u
    }, {
        0xff7u, 0x55u
    }, {
        0xff8u, 0x77u
    }, {
        0xff9u, 0x66u
    }, {
        0xffau, 0x55u
    }, {
        0xffbu, 0x44u
    }, {
        0xffcu, 0x1u
    }, {
        0xffdu, 0xefu
    }, {
        0xffeu, 0xcdu
    }, {
        0xfffu, 0xabu
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_37[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x7bu
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x0u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x5u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x0u
    }, {
        0x100bu, 0x0u
    }, {
        0x521a1cu, 0xffu
    }, {
        0x521a1du, 0xffu
    }, {
        0x521a1eu, 0xffu
    }, {
        0x521a1fu, 0xffu
    }, {
        0x521a4cu, 0xffu
    }, {
        0x521a4du, 0xffu
    }, {
        0x521a4eu, 0xffu
    }, {
        0x521a4fu, 0xffu
    }, {
        0x521a7cu, 0xffu
    }, {
        0x521a7du, 0xffu
    }, {
        0x521a7eu, 0xffu
    }, {
        0x521a7fu, 0xffu
    }, {
        0x521aacu, 0xffu
    }, {
        0x521aadu, 0xffu
    }, {
        0x521aaeu, 0xffu
    }, {
        0x521aafu, 0xffu
    }, {
        0x521adcu, 0xffu
    }, {
        0x521addu, 0xffu
    }, {
        0x521adeu, 0xffu
    }, {
        0x521adfu, 0xffu
    }, {
        0x521b0cu, 0xffu
    }, {
        0x521b0du, 0xffu
    }, {
        0x521b0eu, 0xffu
    }, {
        0x521b0fu, 0xffu
    }, {
        0x521b3cu, 0xffu
    }, {
        0x521b3du, 0xffu
    }, {
        0x521b3eu, 0xffu
    }, {
        0x521b3fu, 0xffu
    }, {
        0x521b6cu, 0xffu
    }, {
        0x521b6du, 0xffu
    }, {
        0x521b6eu, 0xffu
    }, {
        0x521b6fu, 0xffu
    }, {
        0x521b9cu, 0xffu
    }, {
        0x521b9du, 0xffu
    }, {
        0x521b9eu, 0xffu
    }, {
        0x521b9fu, 0xffu
    }, {
        0x521bccu, 0xffu
    }, {
        0x521bcdu, 0xffu
    }, {
        0x521bceu, 0xffu
    }, {
        0x521bcfu, 0xffu
    }, {
        0x521bfcu, 0xffu
    }, {
        0x521bfdu, 0xffu
    }, {
        0x521bfeu, 0xffu
    }, {
        0x521bffu, 0xffu
    }, {
        0x521c2cu, 0xffu
    }, {
        0x521c2du, 0xffu
    }, {
        0x521c2eu, 0xffu
    }, {
        0x521c2fu, 0xffu
    }, {
        0x521c5cu, 0xffu
    }, {
        0x521c5du, 0xffu
    }, {
        0x521c5eu, 0xffu
    }, {
        0x521c5fu, 0xffu
    }, {
        0x521c8cu, 0xffu
    }, {
        0x521c8du, 0xffu
    }, {
        0x521c8eu, 0xffu
    }, {
        0x521c8fu, 0xffu
    }, {
        0x521cbcu, 0xffu
    }, {
        0x521cbdu, 0xffu
    }, {
        0x521cbeu, 0xffu
    }, {
        0x521cbfu, 0xffu
    }, {
        0x521cecu, 0xffu
    }, {
        0x521cedu, 0xffu
    }, {
        0x521ceeu, 0xffu
    }, {
        0x521cefu, 0xffu
    }, {
        0x521d1cu, 0xffu
    }, {
        0x521d1du, 0xffu
    }, {
        0x521d1eu, 0xffu
    }, {
        0x521d1fu, 0xffu
    }, {
        0x521d4cu, 0xffu
    }, {
        0x521d4du, 0xffu
    }, {
        0x521d4eu, 0xffu
    }, {
        0x521d4fu, 0xffu
    }, {
        0x521d7cu, 0xffu
    }, {
        0x521d7du, 0xffu
    }, {
        0x521d7eu, 0xffu
    }, {
        0x521d7fu, 0xffu
    }, {
        0x521dacu, 0xffu
    }, {
        0x521dadu, 0xffu
    }, {
        0x521daeu, 0xffu
    }, {
        0x521dafu, 0xffu
    }, {
        0x521ddcu, 0xffu
    }, {
        0x521dddu, 0xffu
    }, {
        0x521ddeu, 0xffu
    }, {
        0x521ddfu, 0xffu
    }, {
        0x521e0cu, 0xffu
    }, {
        0x521e0du, 0xffu
    }, {
        0x521e0eu, 0xffu
    }, {
        0x521e0fu, 0xffu
    }, {
        0x521e3cu, 0xffu
    }, {
        0x521e3du, 0xffu
    }, {
        0x521e3eu, 0xffu
    }, {
        0x521e3fu, 0xffu
    }, {
        0x521e6cu, 0xffu
    }, {
        0x521e6du, 0xffu
    }, {
        0x521e6eu, 0xffu
    }, {
        0x521e6fu, 0xffu
    }, {
        0x521e9cu, 0xffu
    }, {
        0x521e9du, 0xffu
    }, {
        0x521e9eu, 0xffu
    }, {
        0x521e9fu, 0xffu
    }, {
        0x521eccu, 0xffu
    }, {
        0x521ecdu, 0xffu
    }, {
        0x521eceu, 0xffu
    }, {
        0x521ecfu, 0xffu
    }, {
        0x521efcu, 0xffu
    }, {
        0x521efdu, 0xffu
    }, {
        0x521efeu, 0xffu
    }, {
        0x521effu, 0xffu
    }, {
        0x521f2cu, 0xffu
    }, {
        0x521f2du, 0xffu
    }, {
        0x521f2eu, 0xffu
    }, {
        0x521f2fu, 0xffu
    }, {
        0x521f5cu, 0xffu
    }, {
        0x521f5du, 0xffu
    }, {
        0x521f5eu, 0xffu
    }, {
        0x521f5fu, 0xffu
    }, {
        0x521f8cu, 0xffu
    }, {
        0x521f8du, 0xffu
    }, {
        0x521f8eu, 0xffu
    }, {
        0x521f8fu, 0xffu
    }, {
        0x521fbcu, 0xffu
    }, {
        0x521fbdu, 0xffu
    }, {
        0x521fbeu, 0xffu
    }, {
        0x521fbfu, 0xffu
    }, {
        0x521fecu, 0xffu
    }, {
        0x521fedu, 0xffu
    }, {
        0x521feeu, 0xffu
    }, {
        0x521fefu, 0xffu
    }, {
        0x52201cu, 0xffu
    }, {
        0x52201du, 0xffu
    }, {
        0x52201eu, 0xffu
    }, {
        0x52201fu, 0xffu
    }, {
        0x52204cu, 0xffu
    }, {
        0x52204du, 0xffu
    }, {
        0x52204eu, 0xffu
    }, {
        0x52204fu, 0xffu
    }, {
        0x52207cu, 0xffu
    }, {
        0x52207du, 0xffu
    }, {
        0x52207eu, 0xffu
    }, {
        0x52207fu, 0xffu
    }, {
        0x5220acu, 0xffu
    }, {
        0x5220adu, 0xffu
    }, {
        0x5220aeu, 0xffu
    }, {
        0x5220afu, 0xffu
    }, {
        0x5220dcu, 0xffu
    }, {
        0x5220ddu, 0xffu
    }, {
        0x5220deu, 0xffu
    }, {
        0x5220dfu, 0xffu
    }, {
        0x52210cu, 0xffu
    }, {
        0x52210du, 0xffu
    }, {
        0x52210eu, 0xffu
    }, {
        0x52210fu, 0xffu
    }, {
        0x52213cu, 0xffu
    }, {
        0x52213du, 0xffu
    }, {
        0x52213eu, 0xffu
    }, {
        0x52213fu, 0xffu
    }, {
        0x52216cu, 0xffu
    }, {
        0x52216du, 0xffu
    }, {
        0x52216eu, 0xffu
    }, {
        0x52216fu, 0xffu
    }, {
        0x52219cu, 0xffu
    }, {
        0x52219du, 0xffu
    }, {
        0x52219eu, 0xffu
    }, {
        0x52219fu, 0xffu
    }, {
        0x5221ccu, 0xffu
    }, {
        0x5221cdu, 0xffu
    }, {
        0x5221ceu, 0xffu
    }, {
        0x5221cfu, 0xffu
    }, {
        0x5221fcu, 0xffu
    }, {
        0x5221fdu, 0xffu
    }, {
        0x5221feu, 0xffu
    }, {
        0x5221ffu, 0xffu
    }, {
        0x52222cu, 0xffu
    }, {
        0x52222du, 0xffu
    }, {
        0x52222eu, 0xffu
    }, {
        0x52222fu, 0xffu
    }, {
        0x52225cu, 0xffu
    }, {
        0x52225du, 0xffu
    }, {
        0x52225eu, 0xffu
    }, {
        0x52225fu, 0xffu
    }, {
        0x52228cu, 0xffu
    }, {
        0x52228du, 0xffu
    }, {
        0x52228eu, 0xffu
    }, {
        0x52228fu, 0xffu
    }, {
        0x5222bcu, 0xffu
    }, {
        0x5222bdu, 0xffu
    }, {
        0x5222beu, 0xffu
    }, {
        0x5222bfu, 0xffu
    }, {
        0x5222ecu, 0xffu
    }, {
        0x5222edu, 0xffu
    }, {
        0x5222eeu, 0xffu
    }, {
        0x5222efu, 0xffu
    }, {
        0x52231cu, 0xffu
    }, {
        0x52231du, 0xffu
    }, {
        0x52231eu, 0xffu
    }, {
        0x52231fu, 0xffu
    }, {
        0x52234cu, 0xffu
    }, {
        0x52234du, 0xffu
    }, {
        0x52234eu, 0xffu
    }, {
        0x52234fu, 0xffu
    }, {
        0x52237cu, 0xffu
    }, {
        0x52237du, 0xffu
    }, {
        0x52237eu, 0xffu
    }, {
        0x52237fu, 0xffu
    }, {
        0x5223acu, 0xffu
    }, {
        0x5223adu, 0xffu
    }, {
        0x5223aeu, 0xffu
    }, {
        0x5223afu, 0xffu
    }, {
        0x5223dcu, 0xffu
    }, {
        0x5223ddu, 0xffu
    }, {
        0x5223deu, 0xffu
    }, {
        0x5223dfu, 0xffu
    }, {
        0x52240cu, 0xffu
    }, {
        0x52240du, 0xffu
    }, {
        0x52240eu, 0xffu
    }, {
        0x52240fu, 0xffu
    }, {
        0x52243cu, 0xffu
    }, {
        0x52243du, 0xffu
    }, {
        0x52243eu, 0xffu
    }, {
        0x52243fu, 0xffu
    }, {
        0x52246cu, 0xffu
    }, {
        0x52246du, 0xffu
    }, {
        0x52246eu, 0xffu
    }, {
        0x52246fu, 0xffu
    }, {
        0x52249cu, 0xffu
    }, {
        0x52249du, 0xffu
    }, {
        0x52249eu, 0xffu
    }, {
        0x52249fu, 0xffu
    }, {
        0x5224ccu, 0xffu
    }, {
        0x5224cdu, 0xffu
    }, {
        0x5224ceu, 0xffu
    }, {
        0x5224cfu, 0xffu
    }, {
        0x5224fcu, 0xffu
    }, {
        0x5224fdu, 0xffu
    }, {
        0x5224feu, 0xffu
    }, {
        0x5224ffu, 0xffu
    }, {
        0x52252cu, 0xffu
    }, {
        0x52252du, 0xffu
    }, {
        0x52252eu, 0xffu
    }, {
        0x52252fu, 0xffu
    }, {
        0x52255cu, 0xffu
    }, {
        0x52255du, 0xffu
    }, {
        0x52255eu, 0xffu
    }, {
        0x52255fu, 0xffu
    }, {
        0x52258cu, 0xffu
    }, {
        0x52258du, 0xffu
    }, {
        0x52258eu, 0xffu
    }, {
        0x52258fu, 0xffu
    }, {
        0x5225bcu, 0xffu
    }, {
        0x5225bdu, 0xffu
    }, {
        0x5225beu, 0xffu
    }, {
        0x5225bfu, 0xffu
    }, {
        0x5225ecu, 0xffu
    }, {
        0x5225edu, 0xffu
    }, {
        0x5225eeu, 0xffu
    }, {
        0x5225efu, 0xffu
    }, {
        0x52261cu, 0xffu
    }, {
        0x52261du, 0xffu
    }, {
        0x52261eu, 0xffu
    }, {
        0x52261fu, 0xffu
    }, {
        0x52264cu, 0xffu
    }, {
        0x52264du, 0xffu
    }, {
        0x52264eu, 0xffu
    }, {
        0x52264fu, 0xffu
    }, {
        0x52267cu, 0xffu
    }, {
        0x52267du, 0xffu
    }, {
        0x52267eu, 0xffu
    }, {
        0x52267fu, 0xffu
    }, {
        0x5226acu, 0xffu
    }, {
        0x5226adu, 0xffu
    }, {
        0x5226aeu, 0xffu
    }, {
        0x5226afu, 0xffu
    }, {
        0x5226dcu, 0xffu
    }, {
        0x5226ddu, 0xffu
    }, {
        0x5226deu, 0xffu
    }, {
        0x5226dfu, 0xffu
    }, {
        0x52270cu, 0xffu
    }, {
        0x52270du, 0xffu
    }, {
        0x52270eu, 0xffu
    }, {
        0x52270fu, 0xffu
    }, {
        0x52273cu, 0xffu
    }, {
        0x52273du, 0xffu
    }, {
        0x52273eu, 0xffu
    }, {
        0x52273fu, 0xffu
    }, {
        0x52276cu, 0xffu
    }, {
        0x52276du, 0xffu
    }, {
        0x52276eu, 0xffu
    }, {
        0x52276fu, 0xffu
    }, {
        0x52279cu, 0xffu
    }, {
        0x52279du, 0xffu
    }, {
        0x52279eu, 0xffu
    }, {
        0x52279fu, 0xffu
    }, {
        0x5227ccu, 0xffu
    }, {
        0x5227cdu, 0xffu
    }, {
        0x5227ceu, 0xffu
    }, {
        0x5227cfu, 0xffu
    }, {
        0x5227fcu, 0xffu
    }, {
        0x5227fdu, 0xffu
    }, {
        0x5227feu, 0xffu
    }, {
        0x5227ffu, 0xffu
    }, {
        0x52282cu, 0xffu
    }, {
        0x52282du, 0xffu
    }, {
        0x52282eu, 0xffu
    }, {
        0x52282fu, 0xffu
    }, {
        0x52285cu, 0xffu
    }, {
        0x52285du, 0xffu
    }, {
        0x52285eu, 0xffu
    }, {
        0x52285fu, 0xffu
    }, {
        0x52288cu, 0xffu
    }, {
        0x52288du, 0xffu
    }, {
        0x52288eu, 0xffu
    }, {
        0x52288fu, 0xffu
    }, {
        0x5228bcu, 0xffu
    }, {
        0x5228bdu, 0xffu
    }, {
        0x5228beu, 0xffu
    }, {
        0x5228bfu, 0xffu
    }, {
        0x5228ecu, 0xffu
    }, {
        0x5228edu, 0xffu
    }, {
        0x5228eeu, 0xffu
    }, {
        0x5228efu, 0xffu
    }, {
        0x52291cu, 0xffu
    }, {
        0x52291du, 0xffu
    }, {
        0x52291eu, 0xffu
    }, {
        0x52291fu, 0xffu
    }, {
        0x52294cu, 0xffu
    }, {
        0x52294du, 0xffu
    }, {
        0x52294eu, 0xffu
    }, {
        0x52294fu, 0xffu
    }, {
        0x52297cu, 0xffu
    }, {
        0x52297du, 0xffu
    }, {
        0x52297eu, 0xffu
    }, {
        0x52297fu, 0xffu
    }, {
        0x5229acu, 0xffu
    }, {
        0x5229adu, 0xffu
    }, {
        0x5229aeu, 0xffu
    }, {
        0x5229afu, 0xffu
    }, {
        0x5229dcu, 0xffu
    }, {
        0x5229ddu, 0xffu
    }, {
        0x5229deu, 0xffu
    }, {
        0x5229dfu, 0xffu
    }, {
        0x522a0cu, 0xffu
    }, {
        0x522a0du, 0xffu
    }, {
        0x522a0eu, 0xffu
    }, {
        0x522a0fu, 0xffu
    }, {
        0x522a3cu, 0xffu
    }, {
        0x522a3du, 0xffu
    }, {
        0x522a3eu, 0xffu
    }, {
        0x522a3fu, 0xffu
    }, {
        0x522a6cu, 0xffu
    }, {
        0x522a6du, 0xffu
    }, {
        0x522a6eu, 0xffu
    }, {
        0x522a6fu, 0xffu
    }, {
        0x522a9cu, 0xffu
    }, {
        0x522a9du, 0xffu
    }, {
        0x522a9eu, 0xffu
    }, {
        0x522a9fu, 0xffu
    }, {
        0x522accu, 0xffu
    }, {
        0x522acdu, 0xffu
    }, {
        0x522aceu, 0xffu
    }, {
        0x522acfu, 0xffu
    }, {
        0x522afcu, 0xffu
    }, {
        0x522afdu, 0xffu
    }, {
        0x522afeu, 0xffu
    }, {
        0x522affu, 0xffu
    }, {
        0x522b2cu, 0xffu
    }, {
        0x522b2du, 0xffu
    }, {
        0x522b2eu, 0xffu
    }, {
        0x522b2fu, 0xffu
    }, {
        0x522b5cu, 0xffu
    }, {
        0x522b5du, 0xffu
    }, {
        0x522b5eu, 0xffu
    }, {
        0x522b5fu, 0xffu
    }, {
        0x522b8cu, 0xffu
    }, {
        0x522b8du, 0xffu
    }, {
        0x522b8eu, 0xffu
    }, {
        0x522b8fu, 0xffu
    }, {
        0x522bbcu, 0xffu
    }, {
        0x522bbdu, 0xffu
    }, {
        0x522bbeu, 0xffu
    }, {
        0x522bbfu, 0xffu
    }, {
        0x522becu, 0xffu
    }, {
        0x522bedu, 0xffu
    }, {
        0x522beeu, 0xffu
    }, {
        0x522befu, 0xffu
    }, {
        0x522c1cu, 0xffu
    }, {
        0x522c1du, 0xffu
    }, {
        0x522c1eu, 0xffu
    }, {
        0x522c1fu, 0xffu
    }, {
        0x522c4cu, 0xffu
    }, {
        0x522c4du, 0xffu
    }, {
        0x522c4eu, 0xffu
    }, {
        0x522c4fu, 0xffu
    }, {
        0x522c7cu, 0xffu
    }, {
        0x522c7du, 0xffu
    }, {
        0x522c7eu, 0xffu
    }, {
        0x522c7fu, 0xffu
    }, {
        0x522cacu, 0xffu
    }, {
        0x522cadu, 0xffu
    }, {
        0x522caeu, 0xffu
    }, {
        0x522cafu, 0xffu
    }, {
        0x522cdcu, 0xffu
    }, {
        0x522cddu, 0xffu
    }, {
        0x522cdeu, 0xffu
    }, {
        0x522cdfu, 0xffu
    }, {
        0x522d0cu, 0xffu
    }, {
        0x522d0du, 0xffu
    }, {
        0x522d0eu, 0xffu
    }, {
        0x522d0fu, 0xffu
    }, {
        0x522d3cu, 0xffu
    }, {
        0x522d3du, 0xffu
    }, {
        0x522d3eu, 0xffu
    }, {
        0x522d3fu, 0xffu
    }, {
        0x522d6cu, 0xffu
    }, {
        0x522d6du, 0xffu
    }, {
        0x522d6eu, 0xffu
    }, {
        0x522d6fu, 0xffu
    }, {
        0x522d9cu, 0xffu
    }, {
        0x522d9du, 0xffu
    }, {
        0x522d9eu, 0xffu
    }, {
        0x522d9fu, 0xffu
    }, {
        0x522dccu, 0xffu
    }, {
        0x522dcdu, 0xffu
    }, {
        0x522dceu, 0xffu
    }, {
        0x522dcfu, 0xffu
    }, {
        0x522dfcu, 0xffu
    }, {
        0x522dfdu, 0xffu
    }, {
        0x522dfeu, 0xffu
    }, {
        0x522dffu, 0xffu
    }, {
        0x522e2cu, 0xffu
    }, {
        0x522e2du, 0xffu
    }, {
        0x522e2eu, 0xffu
    }, {
        0x522e2fu, 0xffu
    }, {
        0x522e5cu, 0xffu
    }, {
        0x522e5du, 0xffu
    }, {
        0x522e5eu, 0xffu
    }, {
        0x522e5fu, 0xffu
    }, {
        0x522e8cu, 0xffu
    }, {
        0x522e8du, 0xffu
    }, {
        0x522e8eu, 0xffu
    }, {
        0x522e8fu, 0xffu
    }, {
        0x522ebcu, 0xffu
    }, {
        0x522ebdu, 0xffu
    }, {
        0x522ebeu, 0xffu
    }, {
        0x522ebfu, 0xffu
    }, {
        0x522eecu, 0xffu
    }, {
        0x522eedu, 0xffu
    }, {
        0x522eeeu, 0xffu
    }, {
        0x522eefu, 0xffu
    }, {
        0x522f1cu, 0xffu
    }, {
        0x522f1du, 0xffu
    }, {
        0x522f1eu, 0xffu
    }, {
        0x522f1fu, 0xffu
    }, {
        0x522f4cu, 0xffu
    }, {
        0x522f4du, 0xffu
    }, {
        0x522f4eu, 0xffu
    }, {
        0x522f4fu, 0xffu
    }, {
        0x522f7cu, 0xffu
    }, {
        0x522f7du, 0xffu
    }, {
        0x522f7eu, 0xffu
    }, {
        0x522f7fu, 0xffu
    }, {
        0x522facu, 0xffu
    }, {
        0x522fadu, 0xffu
    }, {
        0x522faeu, 0xffu
    }, {
        0x522fafu, 0xffu
    }, {
        0x522fdcu, 0xffu
    }, {
        0x522fddu, 0xffu
    }, {
        0x522fdeu, 0xffu
    }, {
        0x522fdfu, 0xffu
    }, {
        0x52300cu, 0xffu
    }, {
        0x52300du, 0xffu
    }, {
        0x52300eu, 0xffu
    }, {
        0x52300fu, 0xffu
    }, {
        0x52303cu, 0xffu
    }, {
        0x52303du, 0xffu
    }, {
        0x52303eu, 0xffu
    }, {
        0x52303fu, 0xffu
    }, {
        0x52306cu, 0xffu
    }, {
        0x52306du, 0xffu
    }, {
        0x52306eu, 0xffu
    }, {
        0x52306fu, 0xffu
    }, {
        0x52309cu, 0xffu
    }, {
        0x52309du, 0xffu
    }, {
        0x52309eu, 0xffu
    }, {
        0x52309fu, 0xffu
    }, {
        0x5230ccu, 0xffu
    }, {
        0x5230cdu, 0xffu
    }, {
        0x5230ceu, 0xffu
    }, {
        0x5230cfu, 0xffu
    }, {
        0x5230fcu, 0xffu
    }, {
        0x5230fdu, 0xffu
    }, {
        0x5230feu, 0xffu
    }, {
        0x5230ffu, 0xffu
    }, {
        0x52312cu, 0xffu
    }, {
        0x52312du, 0xffu
    }, {
        0x52312eu, 0xffu
    }, {
        0x52312fu, 0xffu
    }, {
        0x52315cu, 0xffu
    }, {
        0x52315du, 0xffu
    }, {
        0x52315eu, 0xffu
    }, {
        0x52315fu, 0xffu
    }, {
        0x52318cu, 0xffu
    }, {
        0x52318du, 0xffu
    }, {
        0x52318eu, 0xffu
    }, {
        0x52318fu, 0xffu
    }, {
        0x5231bcu, 0xffu
    }, {
        0x5231bdu, 0xffu
    }, {
        0x5231beu, 0xffu
    }, {
        0x5231bfu, 0xffu
    }, {
        0x5231ecu, 0xffu
    }, {
        0x5231edu, 0xffu
    }, {
        0x5231eeu, 0xffu
    }, {
        0x5231efu, 0xffu
    }, {
        0x52321cu, 0xffu
    }, {
        0x52321du, 0xffu
    }, {
        0x52321eu, 0xffu
    }, {
        0x52321fu, 0xffu
    }, {
        0x52324cu, 0xffu
    }, {
        0x52324du, 0xffu
    }, {
        0x52324eu, 0xffu
    }, {
        0x52324fu, 0xffu
    }, {
        0x52327cu, 0xffu
    }, {
        0x52327du, 0xffu
    }, {
        0x52327eu, 0xffu
    }, {
        0x52327fu, 0xffu
    }, {
        0x5232acu, 0xffu
    }, {
        0x5232adu, 0xffu
    }, {
        0x5232aeu, 0xffu
    }, {
        0x5232afu, 0xffu
    }, {
        0x5232dcu, 0xffu
    }, {
        0x5232ddu, 0xffu
    }, {
        0x5232deu, 0xffu
    }, {
        0x5232dfu, 0xffu
    }, {
        0x52330cu, 0xffu
    }, {
        0x52330du, 0xffu
    }, {
        0x52330eu, 0xffu
    }, {
        0x52330fu, 0xffu
    }, {
        0x52333cu, 0xffu
    }, {
        0x52333du, 0xffu
    }, {
        0x52333eu, 0xffu
    }, {
        0x52333fu, 0xffu
    }, {
        0x52336cu, 0xffu
    }, {
        0x52336du, 0xffu
    }, {
        0x52336eu, 0xffu
    }, {
        0x52336fu, 0xffu
    }, {
        0x52339cu, 0xffu
    }, {
        0x52339du, 0xffu
    }, {
        0x52339eu, 0xffu
    }, {
        0x52339fu, 0xffu
    }, {
        0x5233ccu, 0xffu
    }, {
        0x5233cdu, 0xffu
    }, {
        0x5233ceu, 0xffu
    }, {
        0x5233cfu, 0xffu
    }, {
        0x5233fcu, 0xffu
    }, {
        0x5233fdu, 0xffu
    }, {
        0x5233feu, 0xffu
    }, {
        0x5233ffu, 0xffu
    }, {
        0x52342cu, 0xffu
    }, {
        0x52342du, 0xffu
    }, {
        0x52342eu, 0xffu
    }, {
        0x52342fu, 0xffu
    }, {
        0x52345cu, 0xffu
    }, {
        0x52345du, 0xffu
    }, {
        0x52345eu, 0xffu
    }, {
        0x52345fu, 0xffu
    }, {
        0x52348cu, 0xffu
    }, {
        0x52348du, 0xffu
    }, {
        0x52348eu, 0xffu
    }, {
        0x52348fu, 0xffu
    }, {
        0x5234bcu, 0xffu
    }, {
        0x5234bdu, 0xffu
    }, {
        0x5234beu, 0xffu
    }, {
        0x5234bfu, 0xffu
    }, {
        0x5234ecu, 0xffu
    }, {
        0x5234edu, 0xffu
    }, {
        0x5234eeu, 0xffu
    }, {
        0x5234efu, 0xffu
    }, {
        0x52351cu, 0xffu
    }, {
        0x52351du, 0xffu
    }, {
        0x52351eu, 0xffu
    }, {
        0x52351fu, 0xffu
    }, {
        0x52354cu, 0xffu
    }, {
        0x52354du, 0xffu
    }, {
        0x52354eu, 0xffu
    }, {
        0x52354fu, 0xffu
    }, {
        0x52357cu, 0xffu
    }, {
        0x52357du, 0xffu
    }, {
        0x52357eu, 0xffu
    }, {
        0x52357fu, 0xffu
    }, {
        0x5235acu, 0xffu
    }, {
        0x5235adu, 0xffu
    }, {
        0x5235aeu, 0xffu
    }, {
        0x5235afu, 0xffu
    }, {
        0x5235dcu, 0xffu
    }, {
        0x5235ddu, 0xffu
    }, {
        0x5235deu, 0xffu
    }, {
        0x5235dfu, 0xffu
    }, {
        0x52360cu, 0xffu
    }, {
        0x52360du, 0xffu
    }, {
        0x52360eu, 0xffu
    }, {
        0x52360fu, 0xffu
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_37[] = {
    {
        0xff4u, 0x88u
    }, {
        0xff5u, 0x77u
    }, {
        0xff6u, 0x66u
    }, {
        0xff7u, 0x55u
    }, {
        0xff8u, 0x77u
    }, {
        0xff9u, 0x66u
    }, {
        0xffau, 0x55u
    }, {
        0xffbu, 0x44u
    }, {
        0xffcu, 0x1u
    }, {
        0xffdu, 0xefu
    }, {
        0xffeu, 0xcdu
    }, {
        0xfffu, 0xabu
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_38[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x78400cu, 0x0u
    }, {
        0x78400du, 0x0u
    }, {
        0x78400eu, 0x0u
    }, {
        0x78400fu, 0x0u
    }, {
        0x20014u, 0x2u
    }, {
        0x20040u, 0x0u
    }, {
        0x20041u, 0x0u
    }, {
        0x20042u, 0x3u
    }, {
        0x20043u, 0x0u
    }, {
        0x30014u, 0x3u
    }, {
        0x30040u, 0x0u
    }, {
        0x30041u, 0x0u
    }, {
        0x30042u, 0x0u
    }, {
        0x30043u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_38[] = {
    {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_39[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x78400cu, 0x0u
    }, {
        0x78400du, 0x0u
    }, {
        0x78400eu, 0x0u
    }, {
        0x78400fu, 0x0u
    }, {
        0x20014u, 0x2u
    }, {
        0x20040u, 0x0u
    }, {
        0x20041u, 0x0u
    }, {
        0x20042u, 0x3u
    }, {
        0x20043u, 0x0u
    }, {
        0x30014u, 0x3u
    }, {
        0x30040u, 0x0u
    }, {
        0x30041u, 0x0u
    }, {
        0x30042u, 0x0u
    }, {
        0x30043u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_39[] = {
    {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_40[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x78400cu, 0x0u
    }, {
        0x78400du, 0x0u
    }, {
        0x78400eu, 0x2u
    }, {
        0x78400fu, 0x0u
    }, {
        0x20014u, 0x2u
    }, {
        0x20040u, 0x0u
    }, {
        0x20041u, 0x0u
    }, {
        0x20042u, 0x3u
    }, {
        0x20043u, 0x0u
    }, {
        0x30014u, 0x3u
    }, {
        0x30040u, 0x0u
    }, {
        0x30041u, 0x0u
    }, {
        0x30042u, 0x0u
    }, {
        0x30043u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_40[] = {
    {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_41[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x78400cu, 0x0u
    }, {
        0x78400du, 0x0u
    }, {
        0x78400eu, 0x2u
    }, {
        0x78400fu, 0x0u
    }, {
        0x20014u, 0x2u
    }, {
        0x20040u, 0x0u
    }, {
        0x20041u, 0x0u
    }, {
        0x20042u, 0x3u
    }, {
        0x20043u, 0x0u
    }, {
        0x30014u, 0x3u
    }, {
        0x30040u, 0x0u
    }, {
        0x30041u, 0x0u
    }, {
        0x30042u, 0x0u
    }, {
        0x30043u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_41[] = {
    {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_42[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x0u
    }, {
        0x100bu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_42[] = {
    {
        0x20004u, 0x0u
    }, {
        0x20005u, 0x0u
    }, {
        0x20006u, 0x0u
    }, {
        0x20007u, 0x0u
    }, {
        0x20008u, 0x0u
    }, {
        0x20009u, 0x0u
    }, {
        0x2000au, 0x0u
    }, {
        0x2000bu, 0x0u
    }, {
        0x2000cu, 0x0u
    }, {
        0x2000du, 0x0u
    }, {
        0x2000eu, 0x0u
    }, {
        0x2000fu, 0x0u
    }, {
        0x20010u, 0x0u
    }, {
        0x20011u, 0x0u
    }, {
        0x20012u, 0x0u
    }, {
        0x20013u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_43[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x0u
    }, {
        0x100bu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_43[] = {
    {
        0x20004u, 0x0u
    }, {
        0x20005u, 0x0u
    }, {
        0x20006u, 0x0u
    }, {
        0x20007u, 0x0u
    }, {
        0x20008u, 0x0u
    }, {
        0x20009u, 0x0u
    }, {
        0x2000au, 0x0u
    }, {
        0x2000bu, 0x0u
    }, {
        0x2000cu, 0x0u
    }, {
        0x2000du, 0x0u
    }, {
        0x2000eu, 0x0u
    }, {
        0x2000fu, 0x0u
    }, {
        0x20010u, 0x0u
    }, {
        0x20011u, 0x0u
    }, {
        0x20012u, 0x0u
    }, {
        0x20013u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_44[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0xffu
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x0u
    }, {
        0x100bu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_44[] = {
    {
        0x20004u, 0x8u
    }, {
        0x20005u, 0x0u
    }, {
        0x20006u, 0x0u
    }, {
        0x20007u, 0x0u
    }, {
        0x20008u, 0x1u
    }, {
        0x20009u, 0x0u
    }, {
        0x2000au, 0x0u
    }, {
        0x2000bu, 0x0u
    }, {
        0x2000cu, 0x1u
    }, {
        0x2000du, 0x0u
    }, {
        0x2000eu, 0x0u
    }, {
        0x2000fu, 0x0u
    }, {
        0x20010u, 0x1u
    }, {
        0x20011u, 0x0u
    }, {
        0x20012u, 0x0u
    }, {
        0x20013u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_45[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0xffu
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x0u
    }, {
        0x100bu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_45[] = {
    {
        0x20004u, 0x8u
    }, {
        0x20005u, 0x0u
    }, {
        0x20006u, 0x0u
    }, {
        0x20007u, 0x0u
    }, {
        0x20008u, 0x1u
    }, {
        0x20009u, 0x0u
    }, {
        0x2000au, 0x0u
    }, {
        0x2000bu, 0x0u
    }, {
        0x2000cu, 0x1u
    }, {
        0x2000du, 0x0u
    }, {
        0x2000eu, 0x0u
    }, {
        0x2000fu, 0x0u
    }, {
        0x20010u, 0x1u
    }, {
        0x20011u, 0x0u
    }, {
        0x20012u, 0x0u
    }, {
        0x20013u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_46[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0xabu
    }, {
        0x30002u, 0x1u
    }, {
        0x30003u, 0xd2u
    }, {
        0x30004u, 0x4u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x202fcu, 0x7bu
    }, {
        0x202fdu, 0x0u
    }, {
        0x202feu, 0x0u
    }, {
        0x202ffu, 0x0u
    }, {
        0x77448cu, 0x2u
    }, {
        0x77448du, 0x0u
    }, {
        0x77448eu, 0x0u
    }, {
        0x77448fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_46[] = {
    {
        0xff8u, 0x1u
    }, {
        0xff9u, 0xefu
    }, {
        0xffau, 0xcdu
    }, {
        0xffbu, 0xabu
    }, {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x1004u, 0xabu
    }, {
        0x774788u, 0xd2u
    }, {
        0x774789u, 0x4u
    }, {
        0x77478au, 0x0u
    }, {
        0x77478bu, 0x0u
    }, {
        0x774a92u, 0xabu
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_47[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x30000u, 0x0u
    }, {
        0x30001u, 0xabu
    }, {
        0x30002u, 0x1u
    }, {
        0x30003u, 0xd2u
    }, {
        0x30004u, 0x4u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x202fcu, 0x7bu
    }, {
        0x202fdu, 0x0u
    }, {
        0x202feu, 0x0u
    }, {
        0x202ffu, 0x0u
    }, {
        0x77448cu, 0x2u
    }, {
        0x77448du, 0x0u
    }, {
        0x77448eu, 0x0u
    }, {
        0x77448fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_47[] = {
    {
        0xff8u, 0x1u
    }, {
        0xff9u, 0xefu
    }, {
        0xffau, 0xcdu
    }, {
        0xffbu, 0xabu
    }, {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x1004u, 0xabu
    }, {
        0x774788u, 0xd2u
    }, {
        0x774789u, 0x4u
    }, {
        0x77478au, 0x0u
    }, {
        0x77478bu, 0x0u
    }, {
        0x774a92u, 0xabu
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_48[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x30000u, 0x2u
    }, {
        0x30001u, 0xabu
    }, {
        0x30002u, 0x1u
    }, {
        0x30003u, 0xd2u
    }, {
        0x30004u, 0x4u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x20304u, 0x7bu
    }, {
        0x20305u, 0x0u
    }, {
        0x20306u, 0x0u
    }, {
        0x20307u, 0x0u
    }, {
        0x77448cu, 0x2u
    }, {
        0x77448du, 0x0u
    }, {
        0x77448eu, 0x0u
    }, {
        0x77448fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_48[] = {
    {
        0xff8u, 0x1u
    }, {
        0xff9u, 0xefu
    }, {
        0xffau, 0xcdu
    }, {
        0xffbu, 0xabu
    }, {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x1004u, 0xabu
    }, {
        0x774790u, 0xd2u
    }, {
        0x774791u, 0x4u
    }, {
        0x774792u, 0x0u
    }, {
        0x774793u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_49[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x30000u, 0x2u
    }, {
        0x30001u, 0xabu
    }, {
        0x30002u, 0x1u
    }, {
        0x30003u, 0xd2u
    }, {
        0x30004u, 0x4u
    }, {
        0x30005u, 0x0u
    }, {
        0x30006u, 0x0u
    }, {
        0x20304u, 0x7bu
    }, {
        0x20305u, 0x0u
    }, {
        0x20306u, 0x0u
    }, {
        0x20307u, 0x0u
    }, {
        0x77448cu, 0x2u
    }, {
        0x77448du, 0x0u
    }, {
        0x77448eu, 0x0u
    }, {
        0x77448fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_49[] = {
    {
        0xff8u, 0x1u
    }, {
        0xff9u, 0xefu
    }, {
        0xffau, 0xcdu
    }, {
        0xffbu, 0xabu
    }, {
        0xffcu, 0x77u
    }, {
        0xffdu, 0x66u
    }, {
        0xffeu, 0x55u
    }, {
        0xfffu, 0x44u
    }, {
        0x1004u, 0xabu
    }, {
        0x774790u, 0xd2u
    }, {
        0x774791u, 0x4u
    }, {
        0x774792u, 0x0u
    }, {
        0x774793u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_50[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x6fu
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x0u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0xdeu
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x0u
    }, {
        0x1013u, 0x0u
    }, {
        0x1014u, 0x4du
    }, {
        0x1015u, 0x1u
    }, {
        0x1016u, 0x0u
    }, {
        0x1017u, 0x0u
    }, {
        0x1018u, 0xbcu
    }, {
        0x1019u, 0x1u
    }, {
        0x101au, 0x0u
    }, {
        0x101bu, 0x0u
    }, {
        0x101cu, 0x2bu
    }, {
        0x101du, 0x2u
    }, {
        0x101eu, 0x0u
    }, {
        0x101fu, 0x0u
    }, {
        0x1020u, 0x9au
    }, {
        0x1021u, 0x2u
    }, {
        0x1022u, 0x0u
    }, {
        0x1023u, 0x0u
    }, {
        0x30000u, 0x11u
    }, {
        0x30001u, 0x11u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x22u
    }, {
        0x30005u, 0x22u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x30008u, 0x33u
    }, {
        0x30009u, 0x33u
    }, {
        0x3000au, 0x0u
    }, {
        0x3000bu, 0x0u
    }, {
        0x3000cu, 0x44u
    }, {
        0x3000du, 0x44u
    }, {
        0x3000eu, 0x0u
    }, {
        0x3000fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_50[] = {
    {
        0x20000u, 0x11u
    }, {
        0x20001u, 0x11u
    }, {
        0x20002u, 0x0u
    }, {
        0x20003u, 0x0u
    }, {
        0x20004u, 0x22u
    }, {
        0x20005u, 0x22u
    }, {
        0x20006u, 0x0u
    }, {
        0x20007u, 0x0u
    }, {
        0x20008u, 0x33u
    }, {
        0x20009u, 0x33u
    }, {
        0x2000au, 0x0u
    }, {
        0x2000bu, 0x0u
    }, {
        0x2000cu, 0x44u
    }, {
        0x2000du, 0x44u
    }, {
        0x2000eu, 0x0u
    }, {
        0x2000fu, 0x0u
    }, {
        0x20020u, 0x11u
    }, {
        0x20021u, 0x11u
    }, {
        0x20022u, 0x0u
    }, {
        0x20023u, 0x0u
    }, {
        0x20024u, 0x22u
    }, {
        0x20025u, 0x22u
    }, {
        0x20026u, 0x0u
    }, {
        0x20027u, 0x0u
    }, {
        0x20028u, 0x33u
    }, {
        0x20029u, 0x33u
    }, {
        0x2002au, 0x0u
    }, {
        0x2002bu, 0x0u
    }, {
        0x2002cu, 0x44u
    }, {
        0x2002du, 0x44u
    }, {
        0x2002eu, 0x0u
    }, {
        0x2002fu, 0x0u
    }, {
        0x20040u, 0x6fu
    }, {
        0x20041u, 0x0u
    }, {
        0x20042u, 0x0u
    }, {
        0x20043u, 0x0u
    }, {
        0x20044u, 0xdeu
    }, {
        0x20045u, 0x0u
    }, {
        0x20046u, 0x0u
    }, {
        0x20047u, 0x0u
    }, {
        0x20048u, 0x2bu
    }, {
        0x20049u, 0x2u
    }, {
        0x2004au, 0x0u
    }, {
        0x2004bu, 0x0u
    }, {
        0x2004cu, 0x9au
    }, {
        0x2004du, 0x2u
    }, {
        0x2004eu, 0x0u
    }, {
        0x2004fu, 0x0u
    }, {
        0x20050u, 0x4du
    }, {
        0x20051u, 0x1u
    }, {
        0x20052u, 0x0u
    }, {
        0x20053u, 0x0u
    }, {
        0x20054u, 0xbcu
    }, {
        0x20055u, 0x1u
    }, {
        0x20056u, 0x0u
    }, {
        0x20057u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_51[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x0u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x2u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x6fu
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x0u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0xdeu
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x0u
    }, {
        0x1013u, 0x0u
    }, {
        0x1014u, 0x4du
    }, {
        0x1015u, 0x1u
    }, {
        0x1016u, 0x0u
    }, {
        0x1017u, 0x0u
    }, {
        0x1018u, 0xbcu
    }, {
        0x1019u, 0x1u
    }, {
        0x101au, 0x0u
    }, {
        0x101bu, 0x0u
    }, {
        0x101cu, 0x2bu
    }, {
        0x101du, 0x2u
    }, {
        0x101eu, 0x0u
    }, {
        0x101fu, 0x0u
    }, {
        0x1020u, 0x9au
    }, {
        0x1021u, 0x2u
    }, {
        0x1022u, 0x0u
    }, {
        0x1023u, 0x0u
    }, {
        0x30000u, 0x11u
    }, {
        0x30001u, 0x11u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x22u
    }, {
        0x30005u, 0x22u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x30008u, 0x33u
    }, {
        0x30009u, 0x33u
    }, {
        0x3000au, 0x0u
    }, {
        0x3000bu, 0x0u
    }, {
        0x3000cu, 0x44u
    }, {
        0x3000du, 0x44u
    }, {
        0x3000eu, 0x0u
    }, {
        0x3000fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_51[] = {
    {
        0x20000u, 0x11u
    }, {
        0x20001u, 0x11u
    }, {
        0x20002u, 0x0u
    }, {
        0x20003u, 0x0u
    }, {
        0x20004u, 0x22u
    }, {
        0x20005u, 0x22u
    }, {
        0x20006u, 0x0u
    }, {
        0x20007u, 0x0u
    }, {
        0x20008u, 0x33u
    }, {
        0x20009u, 0x33u
    }, {
        0x2000au, 0x0u
    }, {
        0x2000bu, 0x0u
    }, {
        0x2000cu, 0x44u
    }, {
        0x2000du, 0x44u
    }, {
        0x2000eu, 0x0u
    }, {
        0x2000fu, 0x0u
    }, {
        0x20020u, 0x11u
    }, {
        0x20021u, 0x11u
    }, {
        0x20022u, 0x0u
    }, {
        0x20023u, 0x0u
    }, {
        0x20024u, 0x22u
    }, {
        0x20025u, 0x22u
    }, {
        0x20026u, 0x0u
    }, {
        0x20027u, 0x0u
    }, {
        0x20028u, 0x33u
    }, {
        0x20029u, 0x33u
    }, {
        0x2002au, 0x0u
    }, {
        0x2002bu, 0x0u
    }, {
        0x2002cu, 0x44u
    }, {
        0x2002du, 0x44u
    }, {
        0x2002eu, 0x0u
    }, {
        0x2002fu, 0x0u
    }, {
        0x20040u, 0x6fu
    }, {
        0x20041u, 0x0u
    }, {
        0x20042u, 0x0u
    }, {
        0x20043u, 0x0u
    }, {
        0x20044u, 0xdeu
    }, {
        0x20045u, 0x0u
    }, {
        0x20046u, 0x0u
    }, {
        0x20047u, 0x0u
    }, {
        0x20048u, 0x2bu
    }, {
        0x20049u, 0x2u
    }, {
        0x2004au, 0x0u
    }, {
        0x2004bu, 0x0u
    }, {
        0x2004cu, 0x9au
    }, {
        0x2004du, 0x2u
    }, {
        0x2004eu, 0x0u
    }, {
        0x2004fu, 0x0u
    }, {
        0x20050u, 0x4du
    }, {
        0x20051u, 0x1u
    }, {
        0x20052u, 0x0u
    }, {
        0x20053u, 0x0u
    }, {
        0x20054u, 0xbcu
    }, {
        0x20055u, 0x1u
    }, {
        0x20056u, 0x0u
    }, {
        0x20057u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_52[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x4u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x3u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x6fu
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x0u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0xdeu
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x0u
    }, {
        0x1013u, 0x0u
    }, {
        0x1014u, 0x4du
    }, {
        0x1015u, 0x1u
    }, {
        0x1016u, 0x0u
    }, {
        0x1017u, 0x0u
    }, {
        0x1018u, 0xbcu
    }, {
        0x1019u, 0x1u
    }, {
        0x101au, 0x0u
    }, {
        0x101bu, 0x0u
    }, {
        0x101cu, 0x2bu
    }, {
        0x101du, 0x2u
    }, {
        0x101eu, 0x0u
    }, {
        0x101fu, 0x0u
    }, {
        0x1020u, 0x9au
    }, {
        0x1021u, 0x2u
    }, {
        0x1022u, 0x0u
    }, {
        0x1023u, 0x0u
    }, {
        0x30000u, 0x11u
    }, {
        0x30001u, 0x11u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x22u
    }, {
        0x30005u, 0x22u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x30008u, 0x33u
    }, {
        0x30009u, 0x33u
    }, {
        0x3000au, 0x0u
    }, {
        0x3000bu, 0x0u
    }, {
        0x3000cu, 0x44u
    }, {
        0x3000du, 0x44u
    }, {
        0x3000eu, 0x0u
    }, {
        0x3000fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_52[] = {
    {
        0x30004u, 0x11u
    }, {
        0x30005u, 0x11u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x30008u, 0x11u
    }, {
        0x30009u, 0x11u
    }, {
        0x3000au, 0x0u
    }, {
        0x3000bu, 0x0u
    }, {
        0x3000cu, 0x33u
    }, {
        0x3000du, 0x33u
    }, {
        0x3000eu, 0x0u
    }, {
        0x3000fu, 0x0u
    }, {
        0x30010u, 0x33u
    }, {
        0x30011u, 0x33u
    }, {
        0x30012u, 0x0u
    }, {
        0x30013u, 0x0u
    }, {
        0x30024u, 0x11u
    }, {
        0x30025u, 0x11u
    }, {
        0x30026u, 0x0u
    }, {
        0x30027u, 0x0u
    }, {
        0x30028u, 0x11u
    }, {
        0x30029u, 0x11u
    }, {
        0x3002au, 0x0u
    }, {
        0x3002bu, 0x0u
    }, {
        0x3002cu, 0x33u
    }, {
        0x3002du, 0x33u
    }, {
        0x3002eu, 0x0u
    }, {
        0x3002fu, 0x0u
    }, {
        0x30030u, 0x33u
    }, {
        0x30031u, 0x33u
    }, {
        0x30032u, 0x0u
    }, {
        0x30033u, 0x0u
    }, {
        0x30044u, 0x6fu
    }, {
        0x30045u, 0x0u
    }, {
        0x30046u, 0x0u
    }, {
        0x30047u, 0x0u
    }, {
        0x30048u, 0xdeu
    }, {
        0x30049u, 0x0u
    }, {
        0x3004au, 0x0u
    }, {
        0x3004bu, 0x0u
    }, {
        0x3004cu, 0x2bu
    }, {
        0x3004du, 0x2u
    }, {
        0x3004eu, 0x0u
    }, {
        0x3004fu, 0x0u
    }, {
        0x30050u, 0x9au
    }, {
        0x30051u, 0x2u
    }, {
        0x30052u, 0x0u
    }, {
        0x30053u, 0x0u
    }, {
        0x30054u, 0x4du
    }, {
        0x30055u, 0x1u
    }, {
        0x30056u, 0x0u
    }, {
        0x30057u, 0x0u
    }, {
        0x30058u, 0xbcu
    }, {
        0x30059u, 0x1u
    }, {
        0x3005au, 0x0u
    }, {
        0x3005bu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_53[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0x4u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x3u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0x0u
    }, {
        0x1009u, 0x0u
    }, {
        0x100au, 0x3u
    }, {
        0x100bu, 0x0u
    }, {
        0x100cu, 0x6fu
    }, {
        0x100du, 0x0u
    }, {
        0x100eu, 0x0u
    }, {
        0x100fu, 0x0u
    }, {
        0x1010u, 0xdeu
    }, {
        0x1011u, 0x0u
    }, {
        0x1012u, 0x0u
    }, {
        0x1013u, 0x0u
    }, {
        0x1014u, 0x4du
    }, {
        0x1015u, 0x1u
    }, {
        0x1016u, 0x0u
    }, {
        0x1017u, 0x0u
    }, {
        0x1018u, 0xbcu
    }, {
        0x1019u, 0x1u
    }, {
        0x101au, 0x0u
    }, {
        0x101bu, 0x0u
    }, {
        0x101cu, 0x2bu
    }, {
        0x101du, 0x2u
    }, {
        0x101eu, 0x0u
    }, {
        0x101fu, 0x0u
    }, {
        0x1020u, 0x9au
    }, {
        0x1021u, 0x2u
    }, {
        0x1022u, 0x0u
    }, {
        0x1023u, 0x0u
    }, {
        0x30000u, 0x11u
    }, {
        0x30001u, 0x11u
    }, {
        0x30002u, 0x0u
    }, {
        0x30003u, 0x0u
    }, {
        0x30004u, 0x22u
    }, {
        0x30005u, 0x22u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x30008u, 0x33u
    }, {
        0x30009u, 0x33u
    }, {
        0x3000au, 0x0u
    }, {
        0x3000bu, 0x0u
    }, {
        0x3000cu, 0x44u
    }, {
        0x3000du, 0x44u
    }, {
        0x3000eu, 0x0u
    }, {
        0x3000fu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_53[] = {
    {
        0x30004u, 0x11u
    }, {
        0x30005u, 0x11u
    }, {
        0x30006u, 0x0u
    }, {
        0x30007u, 0x0u
    }, {
        0x30008u, 0x11u
    }, {
        0x30009u, 0x11u
    }, {
        0x3000au, 0x0u
    }, {
        0x3000bu, 0x0u
    }, {
        0x3000cu, 0x33u
    }, {
        0x3000du, 0x33u
    }, {
        0x3000eu, 0x0u
    }, {
        0x3000fu, 0x0u
    }, {
        0x30010u, 0x33u
    }, {
        0x30011u, 0x33u
    }, {
        0x30012u, 0x0u
    }, {
        0x30013u, 0x0u
    }, {
        0x30024u, 0x11u
    }, {
        0x30025u, 0x11u
    }, {
        0x30026u, 0x0u
    }, {
        0x30027u, 0x0u
    }, {
        0x30028u, 0x11u
    }, {
        0x30029u, 0x11u
    }, {
        0x3002au, 0x0u
    }, {
        0x3002bu, 0x0u
    }, {
        0x3002cu, 0x33u
    }, {
        0x3002du, 0x33u
    }, {
        0x3002eu, 0x0u
    }, {
        0x3002fu, 0x0u
    }, {
        0x30030u, 0x33u
    }, {
        0x30031u, 0x33u
    }, {
        0x30032u, 0x0u
    }, {
        0x30033u, 0x0u
    }, {
        0x30044u, 0x6fu
    }, {
        0x30045u, 0x0u
    }, {
        0x30046u, 0x0u
    }, {
        0x30047u, 0x0u
    }, {
        0x30048u, 0xdeu
    }, {
        0x30049u, 0x0u
    }, {
        0x3004au, 0x0u
    }, {
        0x3004bu, 0x0u
    }, {
        0x3004cu, 0x2bu
    }, {
        0x3004du, 0x2u
    }, {
        0x3004eu, 0x0u
    }, {
        0x3004fu, 0x0u
    }, {
        0x30050u, 0x9au
    }, {
        0x30051u, 0x2u
    }, {
        0x30052u, 0x0u
    }, {
        0x30053u, 0x0u
    }, {
        0x30054u, 0x4du
    }, {
        0x30055u, 0x1u
    }, {
        0x30056u, 0x0u
    }, {
        0x30057u, 0x0u
    }, {
        0x30058u, 0xbcu
    }, {
        0x30059u, 0x1u
    }, {
        0x3005au, 0x0u
    }, {
        0x3005bu, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_54[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0xc8u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x0u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0xf4u
    }, {
        0x1009u, 0x1u
    }, {
        0x100au, 0x0u
    }, {
        0x100bu, 0x0u
    }, {
        0x548f58u, 0x1u
    }, {
        0x548f59u, 0x0u
    }, {
        0x548f5au, 0x0u
    }, {
        0x548f5bu, 0x0u
    }, {
        0x548f5cu, 0x64u
    }, {
        0x548f5du, 0x0u
    }, {
        0x548f5eu, 0x0u
    }, {
        0x548f5fu, 0x0u
    }, {
        0x548f60u, 0x1u
    }, {
        0x548f61u, 0x0u
    }, {
        0x548f62u, 0x0u
    }, {
        0x548f63u, 0x0u
    }, {
        0x548f64u, 0xc8u
    }, {
        0x548f65u, 0x0u
    }, {
        0x548f66u, 0x0u
    }, {
        0x548f67u, 0x0u
    }, {
        0x548f68u, 0x1u
    }, {
        0x548f69u, 0x0u
    }, {
        0x548f6au, 0x0u
    }, {
        0x548f6bu, 0x0u
    }, {
        0x548f6cu, 0x2cu
    }, {
        0x548f6du, 0x1u
    }, {
        0x548f6eu, 0x0u
    }, {
        0x548f6fu, 0x0u
    }, {
        0x548f70u, 0x0u
    }, {
        0x548f71u, 0x0u
    }, {
        0x548f72u, 0x0u
    }, {
        0x548f73u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_54[] = {
    {
        0x548f64u, 0xf4u
    }, {
        0x548f65u, 0x1u
    }, {
        0x548f66u, 0x0u
    }, {
        0x548f67u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_55[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0xc8u
    }, {
        0x1005u, 0x0u
    }, {
        0x1006u, 0x0u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0xf4u
    }, {
        0x1009u, 0x1u
    }, {
        0x100au, 0x0u
    }, {
        0x100bu, 0x0u
    }, {
        0x548f58u, 0x1u
    }, {
        0x548f59u, 0x0u
    }, {
        0x548f5au, 0x0u
    }, {
        0x548f5bu, 0x0u
    }, {
        0x548f5cu, 0x64u
    }, {
        0x548f5du, 0x0u
    }, {
        0x548f5eu, 0x0u
    }, {
        0x548f5fu, 0x0u
    }, {
        0x548f60u, 0x1u
    }, {
        0x548f61u, 0x0u
    }, {
        0x548f62u, 0x0u
    }, {
        0x548f63u, 0x0u
    }, {
        0x548f64u, 0xc8u
    }, {
        0x548f65u, 0x0u
    }, {
        0x548f66u, 0x0u
    }, {
        0x548f67u, 0x0u
    }, {
        0x548f68u, 0x1u
    }, {
        0x548f69u, 0x0u
    }, {
        0x548f6au, 0x0u
    }, {
        0x548f6bu, 0x0u
    }, {
        0x548f6cu, 0x2cu
    }, {
        0x548f6du, 0x1u
    }, {
        0x548f6eu, 0x0u
    }, {
        0x548f6fu, 0x0u
    }, {
        0x548f70u, 0x0u
    }, {
        0x548f71u, 0x0u
    }, {
        0x548f72u, 0x0u
    }, {
        0x548f73u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_55[] = {
    {
        0x548f64u, 0xf4u
    }, {
        0x548f65u, 0x1u
    }, {
        0x548f66u, 0x0u
    }, {
        0x548f67u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_56[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0xe7u
    }, {
        0x1005u, 0x3u
    }, {
        0x1006u, 0x0u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0xf4u
    }, {
        0x1009u, 0x1u
    }, {
        0x100au, 0x0u
    }, {
        0x100bu, 0x0u
    }, {
        0x548f58u, 0x1u
    }, {
        0x548f59u, 0x0u
    }, {
        0x548f5au, 0x0u
    }, {
        0x548f5bu, 0x0u
    }, {
        0x548f5cu, 0x64u
    }, {
        0x548f5du, 0x0u
    }, {
        0x548f5eu, 0x0u
    }, {
        0x548f5fu, 0x0u
    }, {
        0x548f60u, 0x1u
    }, {
        0x548f61u, 0x0u
    }, {
        0x548f62u, 0x0u
    }, {
        0x548f63u, 0x0u
    }, {
        0x548f64u, 0xc8u
    }, {
        0x548f65u, 0x0u
    }, {
        0x548f66u, 0x0u
    }, {
        0x548f67u, 0x0u
    }, {
        0x548f68u, 0x1u
    }, {
        0x548f69u, 0x0u
    }, {
        0x548f6au, 0x0u
    }, {
        0x548f6bu, 0x0u
    }, {
        0x548f6cu, 0x2cu
    }, {
        0x548f6du, 0x1u
    }, {
        0x548f6eu, 0x0u
    }, {
        0x548f6fu, 0x0u
    }, {
        0x548f70u, 0x0u
    }, {
        0x548f71u, 0x0u
    }, {
        0x548f72u, 0x0u
    }, {
        0x548f73u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_56[] = {
    {
        0u, 0u
    }
};
static const struct census003_byte census003_initial_57[] = {
    {
        0x1000u, 0x0u
    }, {
        0x1001u, 0x0u
    }, {
        0x1002u, 0x1u
    }, {
        0x1003u, 0x0u
    }, {
        0x1004u, 0xe7u
    }, {
        0x1005u, 0x3u
    }, {
        0x1006u, 0x0u
    }, {
        0x1007u, 0x0u
    }, {
        0x1008u, 0xf4u
    }, {
        0x1009u, 0x1u
    }, {
        0x100au, 0x0u
    }, {
        0x100bu, 0x0u
    }, {
        0x548f58u, 0x1u
    }, {
        0x548f59u, 0x0u
    }, {
        0x548f5au, 0x0u
    }, {
        0x548f5bu, 0x0u
    }, {
        0x548f5cu, 0x64u
    }, {
        0x548f5du, 0x0u
    }, {
        0x548f5eu, 0x0u
    }, {
        0x548f5fu, 0x0u
    }, {
        0x548f60u, 0x1u
    }, {
        0x548f61u, 0x0u
    }, {
        0x548f62u, 0x0u
    }, {
        0x548f63u, 0x0u
    }, {
        0x548f64u, 0xc8u
    }, {
        0x548f65u, 0x0u
    }, {
        0x548f66u, 0x0u
    }, {
        0x548f67u, 0x0u
    }, {
        0x548f68u, 0x1u
    }, {
        0x548f69u, 0x0u
    }, {
        0x548f6au, 0x0u
    }, {
        0x548f6bu, 0x0u
    }, {
        0x548f6cu, 0x2cu
    }, {
        0x548f6du, 0x1u
    }, {
        0x548f6eu, 0x0u
    }, {
        0x548f6fu, 0x0u
    }, {
        0x548f70u, 0x0u
    }, {
        0x548f71u, 0x0u
    }, {
        0x548f72u, 0x0u
    }, {
        0x548f73u, 0x0u
    }, {
        0u, 0u
    }
};
static const struct census003_byte census003_writes_57[] = {
    {
        0u, 0u
    }
};
static void test_t1479_census003(void) {
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t) memory;
    {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_0 / sizeof census003_initial_0[0]; j++) guest_write8(census003_initial_0[j].address, census003_initial_0[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9B70();
        CHECK(g_eax == 0xffffffffu);
        CHECK(g_ecx == 0x1u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_0 / sizeof census003_writes_0[0]; j++) {
            uint32_t address = census003_writes_0[j].address;
            CHECK(guest_read8(address) == census003_writes_0[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_1 / sizeof census003_initial_1[0]; j++) guest_write8(census003_initial_1[j].address, census003_initial_1[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9B70();
        CHECK(g_eax == 0xffffffffu);
        CHECK(g_ecx == 0x1u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_1 / sizeof census003_writes_1[0]; j++) {
            uint32_t address = census003_writes_1[j].address;
            CHECK(guest_read8(address) == census003_writes_1[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_2 / sizeof census003_initial_2[0]; j++) guest_write8(census003_initial_2[j].address, census003_initial_2[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9B70();
        CHECK(g_eax == 0xffffffffu);
        CHECK(g_ecx == 0x1u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x2222u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0x1111u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_2 / sizeof census003_writes_2[0]; j++) {
            uint32_t address = census003_writes_2[j].address;
            CHECK(guest_read8(address) == census003_writes_2[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_3 / sizeof census003_initial_3[0]; j++) guest_write8(census003_initial_3[j].address, census003_initial_3[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9B70();
        CHECK(g_eax == 0xffffffffu);
        CHECK(g_ecx == 0x1u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x2222u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0x1111u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_3 / sizeof census003_writes_3[0]; j++) {
            uint32_t address = census003_writes_3[j].address;
            CHECK(guest_read8(address) == census003_writes_3[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_4 / sizeof census003_initial_4[0]; j++) guest_write8(census003_initial_4[j].address, census003_initial_4[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9BE0();
        CHECK(g_eax == 0x3u);
        CHECK(g_ecx == 0x50000u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_4 / sizeof census003_writes_4[0]; j++) {
            uint32_t address = census003_writes_4[j].address;
            CHECK(guest_read8(address) == census003_writes_4[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_5 / sizeof census003_initial_5[0]; j++) guest_write8(census003_initial_5[j].address, census003_initial_5[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9BE0();
        CHECK(g_eax == 0x3u);
        CHECK(g_ecx == 0x50000u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_5 / sizeof census003_writes_5[0]; j++) {
            uint32_t address = census003_writes_5[j].address;
            CHECK(guest_read8(address) == census003_writes_5[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_6 / sizeof census003_initial_6[0]; j++) guest_write8(census003_initial_6[j].address, census003_initial_6[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9BE0();
        CHECK(g_eax == 0x3u);
        CHECK(g_ecx == 0x50000u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x2222u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_6 / sizeof census003_writes_6[0]; j++) {
            uint32_t address = census003_writes_6[j].address;
            CHECK(guest_read8(address) == census003_writes_6[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_7 / sizeof census003_initial_7[0]; j++) guest_write8(census003_initial_7[j].address, census003_initial_7[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9BE0();
        CHECK(g_eax == 0x3u);
        CHECK(g_ecx == 0x50000u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x2222u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_7 / sizeof census003_writes_7[0]; j++) {
            uint32_t address = census003_writes_7[j].address;
            CHECK(guest_read8(address) == census003_writes_7[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_8 / sizeof census003_initial_8[0]; j++) guest_write8(census003_initial_8[j].address, census003_initial_8[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9C40();
        CHECK(g_eax == 0x0u);
        CHECK(g_ecx == 0x50000u);
        CHECK(g_edx == 0x1u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_8 / sizeof census003_writes_8[0]; j++) {
            uint32_t address = census003_writes_8[j].address;
            CHECK(guest_read8(address) == census003_writes_8[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_9 / sizeof census003_initial_9[0]; j++) guest_write8(census003_initial_9[j].address, census003_initial_9[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9C40();
        CHECK(g_eax == 0x0u);
        CHECK(g_ecx == 0x50000u);
        CHECK(g_edx == 0x1u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_9 / sizeof census003_writes_9[0]; j++) {
            uint32_t address = census003_writes_9[j].address;
            CHECK(guest_read8(address) == census003_writes_9[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_10 / sizeof census003_initial_10[0]; j++) guest_write8(census003_initial_10[j].address, census003_initial_10[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9C40();
        CHECK(g_eax == 0x0u);
        CHECK(g_ecx == 0x50000u);
        CHECK(g_edx == 0x1u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x2222u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_10 / sizeof census003_writes_10[0]; j++) {
            uint32_t address = census003_writes_10[j].address;
            CHECK(guest_read8(address) == census003_writes_10[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_11 / sizeof census003_initial_11[0]; j++) guest_write8(census003_initial_11[j].address, census003_initial_11[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9C40();
        CHECK(g_eax == 0x0u);
        CHECK(g_ecx == 0x50000u);
        CHECK(g_edx == 0x1u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x2222u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_11 / sizeof census003_writes_11[0]; j++) {
            uint32_t address = census003_writes_11[j].address;
            CHECK(guest_read8(address) == census003_writes_11[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_12 / sizeof census003_initial_12[0]; j++) guest_write8(census003_initial_12[j].address, census003_initial_12[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9CA0();
        CHECK(g_eax == 0x3u);
        CHECK(g_ecx == 0x50000u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_12 / sizeof census003_writes_12[0]; j++) {
            uint32_t address = census003_writes_12[j].address;
            CHECK(guest_read8(address) == census003_writes_12[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_13 / sizeof census003_initial_13[0]; j++) guest_write8(census003_initial_13[j].address, census003_initial_13[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9CA0();
        CHECK(g_eax == 0x3u);
        CHECK(g_ecx == 0x50000u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_13 / sizeof census003_writes_13[0]; j++) {
            uint32_t address = census003_writes_13[j].address;
            CHECK(guest_read8(address) == census003_writes_13[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_14 / sizeof census003_initial_14[0]; j++) guest_write8(census003_initial_14[j].address, census003_initial_14[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9CA0();
        CHECK(g_eax == 0x3u);
        CHECK(g_ecx == 0x50000u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x2222u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_14 / sizeof census003_writes_14[0]; j++) {
            uint32_t address = census003_writes_14[j].address;
            CHECK(guest_read8(address) == census003_writes_14[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_15 / sizeof census003_initial_15[0]; j++) guest_write8(census003_initial_15[j].address, census003_initial_15[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9CA0();
        CHECK(g_eax == 0x3u);
        CHECK(g_ecx == 0x50000u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x2222u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_15 / sizeof census003_writes_15[0]; j++) {
            uint32_t address = census003_writes_15[j].address;
            CHECK(guest_read8(address) == census003_writes_15[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_16 / sizeof census003_initial_16[0]; j++) guest_write8(census003_initial_16[j].address, census003_initial_16[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9B70();
        CHECK(g_eax == 0xffffffffu);
        CHECK(g_ecx == 0x1u);
        CHECK(g_edx == 0x4u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_16 / sizeof census003_writes_16[0]; j++) {
            uint32_t address = census003_writes_16[j].address;
            CHECK(guest_read8(address) == census003_writes_16[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_17 / sizeof census003_initial_17[0]; j++) guest_write8(census003_initial_17[j].address, census003_initial_17[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9B70();
        CHECK(g_eax == 0xffffffffu);
        CHECK(g_ecx == 0x1u);
        CHECK(g_edx == 0x4u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_17 / sizeof census003_writes_17[0]; j++) {
            uint32_t address = census003_writes_17[j].address;
            CHECK(guest_read8(address) == census003_writes_17[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_18 / sizeof census003_initial_18[0]; j++) guest_write8(census003_initial_18[j].address, census003_initial_18[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9BE0();
        CHECK(g_eax == 0xffffffffu);
        CHECK(g_ecx == 0xffffffffu);
        CHECK(g_edx == 0x6u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_18 / sizeof census003_writes_18[0]; j++) {
            uint32_t address = census003_writes_18[j].address;
            CHECK(guest_read8(address) == census003_writes_18[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_19 / sizeof census003_initial_19[0]; j++) guest_write8(census003_initial_19[j].address, census003_initial_19[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9BE0();
        CHECK(g_eax == 0xffffffffu);
        CHECK(g_ecx == 0xffffffffu);
        CHECK(g_edx == 0x6u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_19 / sizeof census003_writes_19[0]; j++) {
            uint32_t address = census003_writes_19[j].address;
            CHECK(guest_read8(address) == census003_writes_19[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_20 / sizeof census003_initial_20[0]; j++) guest_write8(census003_initial_20[j].address, census003_initial_20[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9C40();
        CHECK(g_eax == 0xffffffffu);
        CHECK(g_ecx == 0xffffffffu);
        CHECK(g_edx == 0x1u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_20 / sizeof census003_writes_20[0]; j++) {
            uint32_t address = census003_writes_20[j].address;
            CHECK(guest_read8(address) == census003_writes_20[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_21 / sizeof census003_initial_21[0]; j++) guest_write8(census003_initial_21[j].address, census003_initial_21[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9C40();
        CHECK(g_eax == 0xffffffffu);
        CHECK(g_ecx == 0xffffffffu);
        CHECK(g_edx == 0x1u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_21 / sizeof census003_writes_21[0]; j++) {
            uint32_t address = census003_writes_21[j].address;
            CHECK(guest_read8(address) == census003_writes_21[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_22 / sizeof census003_initial_22[0]; j++) guest_write8(census003_initial_22[j].address, census003_initial_22[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9CA0();
        CHECK(g_eax == 0x1u);
        CHECK(g_ecx == 0x1u);
        CHECK(g_edx == 0x3u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_22 / sizeof census003_writes_22[0]; j++) {
            uint32_t address = census003_writes_22[j].address;
            CHECK(guest_read8(address) == census003_writes_22[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_23 / sizeof census003_initial_23[0]; j++) guest_write8(census003_initial_23[j].address, census003_initial_23[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9CA0();
        CHECK(g_eax == 0x1u);
        CHECK(g_ecx == 0x1u);
        CHECK(g_edx == 0x3u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_23 / sizeof census003_writes_23[0]; j++) {
            uint32_t address = census003_writes_23[j].address;
            CHECK(guest_read8(address) == census003_writes_23[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_24 / sizeof census003_initial_24[0]; j++) guest_write8(census003_initial_24[j].address, census003_initial_24[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_00359F40();
        CHECK(g_eax == 0xaabbccddu);
        CHECK(g_ecx == 0x11223344u);
        CHECK(g_edx == 0x12345678u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_24 / sizeof census003_writes_24[0]; j++) {
            uint32_t address = census003_writes_24[j].address;
            CHECK(guest_read8(address) == census003_writes_24[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_25 / sizeof census003_initial_25[0]; j++) guest_write8(census003_initial_25[j].address, census003_initial_25[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_00359F40();
        CHECK(g_eax == 0xaabbccddu);
        CHECK(g_ecx == 0x11223344u);
        CHECK(g_edx == 0x12345678u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_25 / sizeof census003_writes_25[0]; j++) {
            uint32_t address = census003_writes_25[j].address;
            CHECK(guest_read8(address) == census003_writes_25[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_26 / sizeof census003_initial_26[0]; j++) guest_write8(census003_initial_26[j].address, census003_initial_26[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9E10();
        CHECK(g_eax == 0x1u);
        CHECK(g_ecx == 0x1u);
        CHECK(g_edx == 0x2u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_26 / sizeof census003_writes_26[0]; j++) {
            uint32_t address = census003_writes_26[j].address;
            CHECK(guest_read8(address) == census003_writes_26[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_27 / sizeof census003_initial_27[0]; j++) guest_write8(census003_initial_27[j].address, census003_initial_27[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9E10();
        CHECK(g_eax == 0x1u);
        CHECK(g_ecx == 0x1u);
        CHECK(g_edx == 0x2u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_27 / sizeof census003_writes_27[0]; j++) {
            uint32_t address = census003_writes_27[j].address;
            CHECK(guest_read8(address) == census003_writes_27[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_28 / sizeof census003_initial_28[0]; j++) guest_write8(census003_initial_28[j].address, census003_initial_28[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9E10();
        CHECK(g_eax == 0x2u);
        CHECK(g_ecx == 0x1u);
        CHECK(g_edx == 0x2u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_28 / sizeof census003_writes_28[0]; j++) {
            uint32_t address = census003_writes_28[j].address;
            CHECK(guest_read8(address) == census003_writes_28[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_29 / sizeof census003_initial_29[0]; j++) guest_write8(census003_initial_29[j].address, census003_initial_29[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9E10();
        CHECK(g_eax == 0x2u);
        CHECK(g_ecx == 0x1u);
        CHECK(g_edx == 0x2u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_29 / sizeof census003_writes_29[0]; j++) {
            uint32_t address = census003_writes_29[j].address;
            CHECK(guest_read8(address) == census003_writes_29[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_30 / sizeof census003_initial_30[0]; j++) guest_write8(census003_initial_30[j].address, census003_initial_30[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002A9E10();
        CHECK(g_eax == 0xffffffffu);
        CHECK(g_ecx == 0x1u);
        CHECK(g_edx == 0x4u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_30 / sizeof census003_writes_30[0]; j++) {
            uint32_t address = census003_writes_30[j].address;
            CHECK(guest_read8(address) == census003_writes_30[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_31 / sizeof census003_initial_31[0]; j++) guest_write8(census003_initial_31[j].address, census003_initial_31[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002A9E10();
        CHECK(g_eax == 0xffffffffu);
        CHECK(g_ecx == 0x1u);
        CHECK(g_edx == 0x4u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_31 / sizeof census003_writes_31[0]; j++) {
            uint32_t address = census003_writes_31[j].address;
            CHECK(guest_read8(address) == census003_writes_31[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_32 / sizeof census003_initial_32[0]; j++) guest_write8(census003_initial_32[j].address, census003_initial_32[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002BE600();
        CHECK(g_eax == 0x0u);
        CHECK(g_ecx == 0x0u);
        CHECK(g_edx == 0x12345605u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_32 / sizeof census003_writes_32[0]; j++) {
            uint32_t address = census003_writes_32[j].address;
            CHECK(guest_read8(address) == census003_writes_32[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_33 / sizeof census003_initial_33[0]; j++) guest_write8(census003_initial_33[j].address, census003_initial_33[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002BE600();
        CHECK(g_eax == 0x0u);
        CHECK(g_ecx == 0x0u);
        CHECK(g_edx == 0x12345605u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_33 / sizeof census003_writes_33[0]; j++) {
            uint32_t address = census003_writes_33[j].address;
            CHECK(guest_read8(address) == census003_writes_33[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_34 / sizeof census003_initial_34[0]; j++) guest_write8(census003_initial_34[j].address, census003_initial_34[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002BE600();
        CHECK(g_eax == 0x95u);
        CHECK(g_ecx == 0x1bf0u);
        CHECK(g_edx == 0x12345605u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_34 / sizeof census003_writes_34[0]; j++) {
            uint32_t address = census003_writes_34[j].address;
            CHECK(guest_read8(address) == census003_writes_34[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_35 / sizeof census003_initial_35[0]; j++) guest_write8(census003_initial_35[j].address, census003_initial_35[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002BE600();
        CHECK(g_eax == 0x95u);
        CHECK(g_ecx == 0x1bf0u);
        CHECK(g_edx == 0x12345605u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_35 / sizeof census003_writes_35[0]; j++) {
            uint32_t address = census003_writes_35[j].address;
            CHECK(guest_read8(address) == census003_writes_35[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_36 / sizeof census003_initial_36[0]; j++) guest_write8(census003_initial_36[j].address, census003_initial_36[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002BE600();
        CHECK(g_eax == 0xffffffffu);
        CHECK(g_ecx == 0x1c20u);
        CHECK(g_edx == 0x12345678u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_36 / sizeof census003_writes_36[0]; j++) {
            uint32_t address = census003_writes_36[j].address;
            CHECK(guest_read8(address) == census003_writes_36[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_37 / sizeof census003_initial_37[0]; j++) guest_write8(census003_initial_37[j].address, census003_initial_37[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002BE600();
        CHECK(g_eax == 0xffffffffu);
        CHECK(g_ecx == 0x1c20u);
        CHECK(g_edx == 0x12345678u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_37 / sizeof census003_writes_37[0]; j++) {
            uint32_t address = census003_writes_37[j].address;
            CHECK(guest_read8(address) == census003_writes_37[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_38 / sizeof census003_initial_38[0]; j++) guest_write8(census003_initial_38[j].address, census003_initial_38[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002FBB10();
        CHECK(g_eax == 0x0u);
        CHECK(g_ecx == 0x0u);
        CHECK(g_edx == 0x12345678u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_38 / sizeof census003_writes_38[0]; j++) {
            uint32_t address = census003_writes_38[j].address;
            CHECK(guest_read8(address) == census003_writes_38[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_39 / sizeof census003_initial_39[0]; j++) guest_write8(census003_initial_39[j].address, census003_initial_39[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002FBB10();
        CHECK(g_eax == 0x0u);
        CHECK(g_ecx == 0x0u);
        CHECK(g_edx == 0x12345678u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_39 / sizeof census003_writes_39[0]; j++) {
            uint32_t address = census003_writes_39[j].address;
            CHECK(guest_read8(address) == census003_writes_39[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_40 / sizeof census003_initial_40[0]; j++) guest_write8(census003_initial_40[j].address, census003_initial_40[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_002FBB10();
        CHECK(g_eax == 0x2u);
        CHECK(g_ecx == 0x0u);
        CHECK(g_edx == 0x1u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_40 / sizeof census003_writes_40[0]; j++) {
            uint32_t address = census003_writes_40[j].address;
            CHECK(guest_read8(address) == census003_writes_40[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_41 / sizeof census003_initial_41[0]; j++) guest_write8(census003_initial_41[j].address, census003_initial_41[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_002FBB10();
        CHECK(g_eax == 0x2u);
        CHECK(g_ecx == 0x0u);
        CHECK(g_edx == 0x1u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_41 / sizeof census003_writes_41[0]; j++) {
            uint32_t address = census003_writes_41[j].address;
            CHECK(guest_read8(address) == census003_writes_41[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_42 / sizeof census003_initial_42[0]; j++) guest_write8(census003_initial_42[j].address, census003_initial_42[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_0031DD10();
        CHECK(g_eax == 0x0u);
        CHECK(g_ecx == 0x20000u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_42 / sizeof census003_writes_42[0]; j++) {
            uint32_t address = census003_writes_42[j].address;
            CHECK(guest_read8(address) == census003_writes_42[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_43 / sizeof census003_initial_43[0]; j++) guest_write8(census003_initial_43[j].address, census003_initial_43[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_0031DD10();
        CHECK(g_eax == 0x0u);
        CHECK(g_ecx == 0x20000u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_43 / sizeof census003_writes_43[0]; j++) {
            uint32_t address = census003_writes_43[j].address;
            CHECK(guest_read8(address) == census003_writes_43[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_44 / sizeof census003_initial_44[0]; j++) guest_write8(census003_initial_44[j].address, census003_initial_44[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_0031DD10();
        CHECK(g_eax == 0x8u);
        CHECK(g_ecx == 0x20000u);
        CHECK(g_edx == 0x1u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_44 / sizeof census003_writes_44[0]; j++) {
            uint32_t address = census003_writes_44[j].address;
            CHECK(guest_read8(address) == census003_writes_44[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_45 / sizeof census003_initial_45[0]; j++) guest_write8(census003_initial_45[j].address, census003_initial_45[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_0031DD10();
        CHECK(g_eax == 0x8u);
        CHECK(g_ecx == 0x20000u);
        CHECK(g_edx == 0x1u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_45 / sizeof census003_writes_45[0]; j++) {
            uint32_t address = census003_writes_45[j].address;
            CHECK(guest_read8(address) == census003_writes_45[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_46 / sizeof census003_initial_46[0]; j++) guest_write8(census003_initial_46[j].address, census003_initial_46[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_00359F40();
        CHECK(g_eax == 0x774798u);
        CHECK(g_ecx == 0x0u);
        CHECK(g_edx == 0xabu);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_46 / sizeof census003_writes_46[0]; j++) {
            uint32_t address = census003_writes_46[j].address;
            CHECK(guest_read8(address) == census003_writes_46[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_47 / sizeof census003_initial_47[0]; j++) guest_write8(census003_initial_47[j].address, census003_initial_47[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_00359F40();
        CHECK(g_eax == 0x774798u);
        CHECK(g_ecx == 0x0u);
        CHECK(g_edx == 0xabu);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_47 / sizeof census003_writes_47[0]; j++) {
            uint32_t address = census003_writes_47[j].address;
            CHECK(guest_read8(address) == census003_writes_47[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_48 / sizeof census003_initial_48[0]; j++) guest_write8(census003_initial_48[j].address, census003_initial_48[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_00359F40();
        CHECK(g_eax == 0x774798u);
        CHECK(g_ecx == 0x2u);
        CHECK(g_edx == 0x1u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_48 / sizeof census003_writes_48[0]; j++) {
            uint32_t address = census003_writes_48[j].address;
            CHECK(guest_read8(address) == census003_writes_48[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_49 / sizeof census003_initial_49[0]; j++) guest_write8(census003_initial_49[j].address, census003_initial_49[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_00359F40();
        CHECK(g_eax == 0x774798u);
        CHECK(g_ecx == 0x2u);
        CHECK(g_edx == 0x1u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_49 / sizeof census003_writes_49[0]; j++) {
            uint32_t address = census003_writes_49[j].address;
            CHECK(guest_read8(address) == census003_writes_49[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_50 / sizeof census003_initial_50[0]; j++) guest_write8(census003_initial_50[j].address, census003_initial_50[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_0036E8C0();
        CHECK(g_eax == 0x20000u);
        CHECK(g_ecx == 0x29au);
        CHECK(g_edx == 0x22bu);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_50 / sizeof census003_writes_50[0]; j++) {
            uint32_t address = census003_writes_50[j].address;
            CHECK(guest_read8(address) == census003_writes_50[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_51 / sizeof census003_initial_51[0]; j++) guest_write8(census003_initial_51[j].address, census003_initial_51[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_0036E8C0();
        CHECK(g_eax == 0x20000u);
        CHECK(g_ecx == 0x29au);
        CHECK(g_edx == 0x22bu);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_51 / sizeof census003_writes_51[0]; j++) {
            uint32_t address = census003_writes_51[j].address;
            CHECK(guest_read8(address) == census003_writes_51[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_52 / sizeof census003_initial_52[0]; j++) guest_write8(census003_initial_52[j].address, census003_initial_52[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_0036E8C0();
        CHECK(g_eax == 0x30004u);
        CHECK(g_ecx == 0x29au);
        CHECK(g_edx == 0x22bu);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_52 / sizeof census003_writes_52[0]; j++) {
            uint32_t address = census003_writes_52[j].address;
            CHECK(guest_read8(address) == census003_writes_52[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_53 / sizeof census003_initial_53[0]; j++) guest_write8(census003_initial_53[j].address, census003_initial_53[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_0036E8C0();
        CHECK(g_eax == 0x30004u);
        CHECK(g_ecx == 0x29au);
        CHECK(g_edx == 0x22bu);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_53 / sizeof census003_writes_53[0]; j++) {
            uint32_t address = census003_writes_53[j].address;
            CHECK(guest_read8(address) == census003_writes_53[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_54 / sizeof census003_initial_54[0]; j++) guest_write8(census003_initial_54[j].address, census003_initial_54[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_00377730();
        CHECK(g_eax == 0x1u);
        CHECK(g_ecx == 0x1f4u);
        CHECK(g_edx == 0x1u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_54 / sizeof census003_writes_54[0]; j++) {
            uint32_t address = census003_writes_54[j].address;
            CHECK(guest_read8(address) == census003_writes_54[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_55 / sizeof census003_initial_55[0]; j++) guest_write8(census003_initial_55[j].address, census003_initial_55[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_00377730();
        CHECK(g_eax == 0x1u);
        CHECK(g_ecx == 0x1f4u);
        CHECK(g_edx == 0x1u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_55 / sizeof census003_writes_55[0]; j++) {
            uint32_t address = census003_writes_55[j].address;
            CHECK(guest_read8(address) == census003_writes_55[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_56 / sizeof census003_initial_56[0]; j++) guest_write8(census003_initial_56[j].address, census003_initial_56[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 0;
        sub_00377730();
        CHECK(g_eax == 0x3u);
        CHECK(g_ecx == 0x3e7u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 0);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_56 / sizeof census003_writes_56[0]; j++) {
            uint32_t address = census003_writes_56[j].address;
            CHECK(guest_read8(address) == census003_writes_56[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    } {
        memset(memory, 0, sizeof memory);
        memset(census003_written, 0, sizeof census003_written);
        for (unsigned j = 0; j + 1u < sizeof census003_initial_57 / sizeof census003_initial_57[0]; j++) guest_write8(census003_initial_57[j].address, census003_initial_57[j].value);
        memcpy(census003_before, memory, sizeof memory);
        g_eax = 0xaabbccddu;
        g_ecx = 0x11223344u;
        g_edx = 0x12345678u;
        g_esi = 0x44556677u;
        g_edi = 0x55667788u;
        g_ebx = 0xabcdef01u;
        g_ebp = 0x98765432u;
        g_esp = 0x1000u;
        g_df = 1;
        sub_00377730();
        CHECK(g_eax == 0x3u);
        CHECK(g_ecx == 0x3e7u);
        CHECK(g_edx == 0x0u);
        CHECK(g_esp == 0x1004u);
        CHECK(g_esi == 0x44556677u);
        CHECK(g_edi == 0x55667788u);
        CHECK(g_ebx == 0xabcdef01u);
        CHECK(g_ebp == 0x98765432u);
        CHECK(g_df == 1);
        for (unsigned j = 0; j + 1u < sizeof census003_writes_57 / sizeof census003_writes_57[0]; j++) {
            uint32_t address = census003_writes_57[j].address;
            CHECK(guest_read8(address) == census003_writes_57[j].value);
            census003_written[address] = 1u;
        }
        int unchanged = 1;
        for (unsigned j = 0; j < sizeof memory; j++) if (! census003_written[j] && memory[j] != census003_before[j]) {
            unchanged = 0;
            break;
        }
        CHECK(unchanged);
    }
}
#ifndef T1479_EMBEDDED
int main(void) {
    test_t1479_census003();
    printf("%u checks %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
#endif
