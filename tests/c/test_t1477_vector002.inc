/* SPDX-License-Identifier: GPL-3.0-or-later
 * Eight admitted original-derived ABI, lane, stack/POP alias and NaN-order controls.
 */
static void test_t1477_vector002(void)
{
    const uint32_t object=0x10003u;
    vec_begin();guest_write32(g_esp+4u,object);guest_write32(object+4u,0x41u);
    guest_write32(0x4782C4u,0x80000000u);invoke(0x0011F540u,4u);
    CHECK(g_eax==object && g_ecx==0x1041u && guest_read32(object+4u)==0x1041u);
    CHECK(guest_read32(object+0xACu)==0x80000000u);vec_check_xmm0(0x80000000u);
    vec_begin();guest_write32(g_esp+4u,g_esp);guest_write32(0x4782C4u,0x7FC12345u);
    invoke(0x0011F540u,4u);CHECK(g_eax==0x100u && guest_read32(0x104u)==0x1100u);
    CHECK(guest_read32(0x1ACu)==0x7FC12345u);
    vec_begin();guest_write32(g_esp+4u,object);guest_write32(g_esp+8u,0x80000000u);
    guest_write32(object+4u,0x44u);invoke(0x0011FE80u,4u);
    CHECK(g_eax==object && g_ecx==0x46u && guest_read32(object+4u)==0x46u);
    CHECK(guest_read32(object+0x94u)==0x80000000u && guest_read32(object+0x90u)==0u);
    CHECK(guest_read32(object+0x88u)==1u && guest_read32(object+0x8Cu)==0x3Fu);
    vec_check_xmm0(0u);
    vec_begin();g_esp=0x20000u;guest_write32(g_esp,0x10000u);
    guest_write32(g_esp+4u,g_esp);guest_write32(g_esp+8u,0x7FC12345u);
    invoke(0x0011FE80u,4u);CHECK(g_eax==0x20000u && g_ecx==0x20002u);
    CHECK(guest_read32(0x20004u)==0x20002u && guest_read32(0x20094u)==0x7FC12345u);
    const uint32_t list=0x11003u,child=0x12003u;
    for(unsigned mode=0;mode<2u;mode++) {
        vec_begin();guest_write32(g_esp+4u,object);guest_write32(object+0x1Cu,1u);
        guest_write32(object+0x1F8u,list);guest_write32(list,child);
        guest_write32(child+0x28u,mode?0x40Cu:0u);
        guest_write8(child+0xEu,0u);guest_write8(child+0xFu,0xFFu);
        guest_write32(0x7ABFA4u,0xDEADBEEFu);guest_write32(0x47D9D4u,0x3F800000u);
        for(unsigned slot=0;slot<4u;slot++)guest_write32(0x742360u+slot*0x6Cu,0u);
        guest_write32(0x742360u,1u);guest_write32(0x742364u,child);
        invoke(0x00129EC0u,4u);
        CHECK(g_eax==1u && g_edx==child && g_ecx==0xFFu);
        CHECK(guest_read32(child)==(mode?0u:0x3F800000u));
        CHECK(guest_read8(child+0xEu)==0xFFu && guest_read8(child+0xFu)==0xFFu);
        CHECK(guest_read32(0x7ABFA4u)==0u && guest_read32(0x742360u)==0u);
        CHECK(g_ebx==0xB0B0B0B0u && g_ebp==0xBEBEBE01u && g_esi==0x51515151u && g_edi==0xD1D1D1D1u);
    }
    /* Child's float store overwrites saved EBX; no RET or other saved-slot write. */
    vec_begin();g_esp=0x20000u;guest_write32(g_esp,0x10000u);
    guest_write32(g_esp+4u,object);guest_write32(object+0x1Cu,1u);guest_write32(object+0x1F8u,list);
    guest_write32(list,g_esp-4u);guest_write32(0x20024u,0u);
    guest_write8(0x2000Au,0xFFu);guest_write8(0x2000Bu,0xFFu);guest_write32(0x47D9D4u,0x7FC12345u);
    for(unsigned slot=0;slot<4u;slot++)guest_write32(0x742360u+slot*0x6Cu,0u);
    invoke(0x00129EC0u,4u);
    CHECK(g_ebx==0x7FC12345u && g_ebp==0xBEBEBE01u && g_esi==0x51515151u && g_edi==0xD1D1D1D1u);
    CHECK(guest_read32(0x1FFFCu)==0x7FC12345u && guest_read32(0x20000u)==0x10000u);
    for(unsigned mode=0;mode<2u;mode++) {
        vec_begin();guest_write32(g_esp+4u,object);guest_write32(object+4u,0u);
        guest_write32(0x7A78B0u,mode);guest_write32(0x7A78BCu,0xABCDu);guest_write32(0x7A78C0u,0x1234u);
        invoke(0x001427D0u,4u);
        CHECK(g_eax==0x7A6160u && g_ecx==mode && guest_read32(0x7A78B0u)==1u);
        CHECK(guest_read32(0x7A78B4u)==0u);
        CHECK(guest_read32(0x7A78BCu)==(mode?0xABCDu:0u));
        CHECK(guest_read32(0x7A78C0u)==(mode?0x1234u:0u));vec_check_xmm0(0u);
    }
    vec_begin();guest_write32(g_esp+4u,object);guest_write32(object+4u,0xFFFFFFFFu);
    invoke(0x001427D0u,4u);vec_check_registers(0xFFFFFFFFu);
    CHECK(g_xmm0.u[1]==0xA5A5A501u);
    vec_begin();g_esp=0x20000u;guest_write32(g_esp,0x10000u);guest_write32(g_esp+4u,object);
    guest_write32(object+4u,0x368BFCEAu); /* solved wrapped record places state at stack+8 */
    guest_write32(0x20008u,0u);guest_write32(0x2000Cu,0xDEADBEEFu);guest_write32(0x20014u,1u);guest_write32(0x20018u,1u);
    invoke(0x001427D0u,4u);
    CHECK(g_eax==0x1E8B8u && guest_read32(0x20008u)==1u && guest_read32(0x2000Cu)==0u);
    CHECK(guest_read32(0x20014u)==0u && guest_read32(0x20018u)==0u && guest_read32(0x20000u)==0x10000u);
    vec_begin();g_esp=0x749848u;guest_write32(g_esp,0x10000u);
    for(unsigned n=0;n<15u;n++)guest_write32(0x74984Cu+4u*n,0xDEADBEEFu);
    guest_write32(0x7A388Cu,1u);guest_write32(0x7A3890u,1u);invoke(0x00155040u,4u);
    vec_check_registers(0u);vec_check_xmm0(0u);
    for(unsigned n=0;n<15u;n++)CHECK(guest_read32(0x74984Cu+4u*n)==0u);
    CHECK(guest_read32(0x7A388Cu)==0u && guest_read32(0x7A3890u)==0u && guest_read32(0x749848u)==0x10000u);
    vec_begin();g_esp=0x74A98Cu;guest_write32(g_esp,0x10000u);guest_write32(g_esp+4u,0x7FC12345u);
    invoke(0x00155410u,4u);vec_check_registers(0x7FC12345u);vec_check_xmm0(0u);
    CHECK(guest_read32(0x4E7968u)==0x7FC12345u && guest_read32(0x74A990u)==0u && guest_read32(0x74A98Cu)==0x10000u);
    guest_write32(0x475C78u,0x3F800000u);
    const uint32_t input[]={0x3F000000u,0x40000000u,0xBF800000u,0x7FC12345u};
    const uint32_t result[]={0x3F000000u,0x3F800000u,0x3E800000u,0x7FC12345u};
    for(unsigned row=0;row<4u;row++) {
        fp_begin();guest_write32(g_esp+4u,input[row]);guest_write32(0x74A990u,0x3E800000u);
        invoke(0x001553B0u,4u);CHECK(guest_read32(0x74A990u)==result[row]);
        CHECK(g_xmm0.u[0]==(row==1u?0x3F800000u:input[row]));
        for(unsigned lane=1;lane<4u;lane++)CHECK(g_xmm0.u[lane]==0u && g_xmm1.u[lane]==0u);
        vec_check_registers(0x11111111u);
    }
    fp_begin();g_esp=0x74A98Cu;guest_write32(g_esp,0x10000u);guest_write32(g_esp+4u,0x40000000u);
    invoke(0x001553B0u,4u);CHECK(guest_read32(0x74A990u)==0x40000000u && g_xmm0.u[0]==0x3F800000u && g_xmm1.u[0]==0x40000000u);
    /* Projection then cross product, aliasing output to point or argument slots. */
    for(unsigned row=0;row<4u;row++) {
        fp_begin();const uint32_t record=0x13200u,point=0x11003u,direction=0x12003u;
        const uint32_t output=row==1u?point:row==3u?g_esp+4u:0x14003u;
        guest_write32(0x74C458u,0x13000u);
        guest_write32(g_esp+4u,0xABCD0001u);guest_write32(g_esp+8u,point);
        guest_write32(g_esp+12u,direction);guest_write32(g_esp+16u,output);
        for(unsigned axis=0;axis<3u;axis++)guest_write32(record+0x140u+axis*4u,0x3F800000u);
        guest_write32(record+0x14Cu,0x3F800000u);guest_write32(record+0x150u,0x40000000u);guest_write32(record+0x154u,0x40400000u);
        guest_write32(point,0x40800000u);guest_write32(point+4u,0x40A00000u);guest_write32(point+8u,row==2u?0xFFC54321u:0x40C00000u);
        guest_write32(direction,0x3F800000u);guest_write32(direction+4u,0u);guest_write32(direction+8u,row==2u?0x7FC12345u:0u);
        invoke(0x00166140u,4u);
        CHECK(g_eax==record && g_ecx==output && g_edx==direction);
        CHECK(guest_read32(output)==(row==2u?0x7FC12345u:0x40000000u));
        CHECK(guest_read32(output+4u)==(row==2u?0x7FC12345u:0x40A00000u));
        CHECK(guest_read32(output+8u)==(row==2u?0x7FC12345u:0xC0800000u));
    }
}
