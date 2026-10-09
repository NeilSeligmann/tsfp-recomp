/* SPDX-License-Identifier: GPL-3.0-or-later
 * Original-derived directed contracts. Admission list is frozen in the receipt.
 */
static void test_t1477_vector001(void)
{
    const uint32_t object = 0x10003u;
    const uint32_t scalars[] = {0u, 0x80000000u, 0x3F800000u, 0x7FC12345u, 0xFFA12345u};
    const uint32_t setters[][3] = {
        {0x0010EA50u, 0x60u, 0x400u}, {0x0010EA90u, 0x5Cu, 0x400u},
        {0x0010EB90u, 0x74u, 0u},
    };
    for (unsigned row = 0; row < 3u; row++) {
        for (unsigned index = 0; index < 5u; index++) {
            vec_begin();
            guest_write32(g_esp + 4u, object);
            guest_write32(g_esp + 8u, scalars[index]);
            guest_write32(object + 4u, 0x80000021u);
            invoke(setters[row][0], 4u);
            CHECK(g_eax == object);
            CHECK(g_ecx == (setters[row][2] ? 0x80000021u | setters[row][2] : 0x22222222u));
            CHECK(g_edx == 0x33333333u);
            CHECK(guest_read32(object + setters[row][1]) == scalars[index]);
            CHECK(guest_read32(object + 4u) == (0x80000021u | setters[row][2]));
            vec_check_xmm0(scalars[index]);
            CHECK(g_ebx == 0xB0B0B0B0u && g_ebp == 0xBEBEBE01u &&
                  g_esi == 0x51515151u && g_edi == 0xD1D1D1D1u);
        }
        vec_begin();
        guest_write32(g_esp + 4u, 0u);
        invoke(setters[row][0], 4u);
        vec_check_registers(0u);
        for (unsigned lane = 0; lane < 4u; lane++) CHECK(g_xmm0.u[lane] == 0xA5A5A500u + lane);
    }
    for (unsigned index = 0; index < 5u; index++) {
        vec_begin();
        guest_write32(g_esp + 4u, object);
        guest_write32(g_esp + 8u, scalars[index]);
        guest_write32(g_esp + 12u, scalars[4u-index]);
        invoke(0x0010EB10u, 4u);
        CHECK(guest_read32(object + 0x68u) == scalars[index]);
        CHECK(guest_read32(object + 0x6Cu) == scalars[4u-index]);
        vec_check_registers(object);
        vec_check_xmm0(scalars[4u-index]);
    }
    vec_begin(); guest_write32(g_esp + 4u, 0u); invoke(0x0010EB10u, 4u);
    vec_check_registers(0u);
    for (unsigned lane = 0; lane < 4u; lane++) CHECK(g_xmm0.u[lane] == 0xA5A5A500u + lane);
    const uint32_t defaults[][3] = {{0x0011F510u,0xA8u,0x400u},{0x0011F570u,0xB0u,0x800u}};
    for (unsigned row=0; row<2u; row++) {
        vec_begin(); guest_write32(g_esp+4u,object);
        guest_write32(0x47CFF4u,0x80000000u); guest_write32(object+4u,0x22u);
        invoke(defaults[row][0],4u);
        CHECK(g_eax==object && g_ecx==(0x22u|defaults[row][2]));
        CHECK(guest_read32(object+4u)==(0x22u|defaults[row][2]));
        CHECK(guest_read32(object+defaults[row][1])==0x80000000u);
        vec_check_xmm0(0x80000000u);
        /* Field +4 is the next scalar stack argument: flag update must precede float read? */
        vec_begin(); guest_write32(g_esp+4u,g_esp);
        guest_write32(0x47CFF4u,0x7FC12345u);
        invoke(defaults[row][0],4u);
        CHECK(guest_read32(0x100u+defaults[row][1])==0x7FC12345u);
        CHECK(guest_read32(0x104u)==(0x100u|defaults[row][2]));
    }
    const uint32_t slots[] = {0u,2u,239u};
    for(unsigned row=0;row<3u;row++) {
        vec_begin(); guest_write32(0x73ED10u,slots[row]); guest_write32(0x742198u,0x3F800000u);
        for(unsigned arg=0;arg<12u;arg++) guest_write32(g_esp+4u+4u*arg,0xABC00000u+arg);
        guest_write32(g_esp+48u,0x12345678u);
        invoke(0x00126AC0u,4u);
        uint32_t dst=0x73ED18u+slots[row]*0x38u;
        CHECK(guest_read32(0x73ED10u)==(slots[row]==239u?0u:slots[row]+1u));
        CHECK(g_eax==dst && g_ecx==0x1234u && g_edx==0x78563412u);
        CHECK(guest_read32(dst)==0xABC00001u && guest_read32(dst+4u)==0xABC00000u);
        CHECK(guest_read32(dst+8u)==0x3F800000u && guest_read32(dst+12u)==0xABC0000Au);
        for(unsigned field=0;field<9u;field++) {
            const uint32_t offsets[]={0x10u,0x14u,0x18u,0x20u,0x24u,0x28u,0x2Cu,0x30u,0x34u};
            CHECK(guest_read32(dst+offsets[field])==(field==8u?0x78563412u:0xABC00002u+field));
        }
        vec_check_xmm0(0xABC00009u);
        CHECK(g_esi==0x51515151u && guest_read32(0xFCu)==0x51515151u);
    }
}

static void test_t1477_vector001_fp_alias(void)
{
    const uint32_t object = 0x10003u;
    guest_write32(0x475C78u,0x3F800000u);
    const uint32_t amplitude[] = {0u, 0x3F000000u, 0x3F800000u, 0x40000000u, 0x7FC12345u};
    for(unsigned row=0;row<5u;row++) {
        fp_begin(); guest_write32(g_esp+4u,object); guest_write32(g_esp+8u,amplitude[row]);
        guest_write32(g_esp+12u,0x34567890u); guest_write32(object+4u,0xFFFFFFFFu);
        invoke(0x0011FDC0u,4u);
        const int active = row==1u || row==2u;
        CHECK(g_eax==object+4u);
        CHECK(guest_read32(object+4u)==(active?0xFFFFFFFFu:0xFFFDFFFDu));
        CHECK(guest_read32(object+0x98u)==(active?amplitude[row]:0u));
        CHECK(guest_read32(object+0x8Cu)==(active?0x34567890u:0u));
        CHECK(g_ecx==(active?0xFFFFFFFFu:0xFFFDFFFDu));
        CHECK(g_edx==(active?0x34567890u:0xFFFDFFFDu));
        vec_check_xmm0(amplitude[row]);
        for(unsigned lane=0;lane<4u;lane++)CHECK(g_xmm1.u[lane]==0u);
    }
    /* The field store aliases argument3: it must be loaded before the following store. */
    fp_begin(); guest_write32(g_esp+4u,0x104u-0x88u);
    guest_write32(g_esp+8u,0x3F000000u); guest_write32(g_esp+12u,0x34567890u);
    guest_write32(0x104u-0x88u+4u,0u);
    invoke(0x0011FDC0u,4u);
    CHECK(guest_read32(0x104u)==0u && guest_read32(0x108u)==0x34567890u);
    /* Ring colour store aliases saved ESI. POP must reload the overwritten guest word. */
    vec_begin(); g_esp=0x20000u; guest_write32(g_esp,0x10000u);
    guest_write32(0x73ED10u,0x0922897Au); /* solved 56*i+73ED18 == 1FFC8 mod2^32 */
    guest_write32(0x742198u,0x3F800000u);
    for(unsigned arg=0;arg<12u;arg++) guest_write32(g_esp+4u+4u*arg,0xABC00000u+arg);
    guest_write32(g_esp+48u,0x12345678u);
    invoke(0x00126AC0u,4u);
    CHECK(g_eax==0x1FFC8u && g_esi==0x78563412u);
    CHECK(guest_read32(0x1FFFCu)==0x78563412u && guest_read32(0x20000u)==0x10000u);
    /* List compaction: DF0 copies a following record; DF1 copies backwards as the guest asks. */
    for(int direction=0;direction<2;direction++) {
        fp_begin(); g_df=direction;
        for(uint32_t off=0;off<0x100u;off+=4u)guest_write32(object-0x30u+off,0x55000000u+off);
        guest_write32(g_esp+4u,object); guest_write32(g_esp+8u,0x3F800000u);
        guest_write32(object,2u); guest_write32(object+0x30u,0x40000000u);
        guest_write32(object+0x64u,0u);
        uint32_t expected[13];
        for(unsigned n=0;n<13u;n++)expected[n]=guest_read32(object+0x3Cu+(direction?(uint32_t)-(4u*n):4u*n));
        invoke(0x00128E90u,4u);
        CHECK(guest_read32(object)==(direction?expected[2]-1u:1u) && g_eax==0xFFFFFFFFu && g_edx==object+8u);
        for(unsigned n=0;n<13u;n++)CHECK(guest_read32(object+8u+(direction?(uint32_t)-(4u*n):4u*n))==(direction && n==2u?expected[n]-1u:expected[n]));
        CHECK(g_ebx==0xB0B0B0B0u && g_ebp==0xBEBEBE01u && g_esi==0x51515151u && g_edi==0xD1D1D1D1u);
    }
    g_df=0;
    /* The compaction destination overwrites saved EDI/ESI/EBP, preserving RET. */
    fp_begin(); g_esp=0x20000u;guest_write32(g_esp,0x10000u);
    const uint32_t alias_object=0x1FFC0u;
    guest_write32(g_esp+4u,alias_object);guest_write32(g_esp+8u,0x3F800000u);
    guest_write32(alias_object,2u);guest_write32(alias_object+0x64u,0u);
    guest_write32(0x20028u,0x12345678u);guest_write32(0x2002Cu,0x23456789u);
    g_edi=0x40000000u;
    invoke(0x00128E90u,4u);
    CHECK(g_edi==0u && g_esi==0x12345678u && g_ebp==0x23456789u && g_ebx==0xB0B0B0B0u);
    CHECK(guest_read32(0x20000u)==0x10000u && guest_read32(alias_object)==1u);
    /* Ordered clamp and two distinct NaNs: original destination payload wins MULSS. */
    const uint32_t inputs[]={0u,0x40000000u,0xC0000000u,0x7FC54321u};
    const uint32_t outputs[]={0u,0x40000000u,0xC0000000u,0x7FC54321u};
    for(unsigned row=0;row<4u;row++) {
        fp_begin();guest_write32(g_esp+4u,inputs[row]);guest_write32(0x7450D0u,1u);
        guest_write32(0x475D24u,row==3u?0xFFC12345u:0x40000000u);
        guest_write32(0x475D1Cu,0x40000000u);guest_write32(0x4785D8u,0xC0000000u);
        invoke(0x0012B8E0u,4u);
        CHECK(g_eax==0x742510u && guest_read32(0x742538u)==outputs[row]);
        CHECK(g_ecx==0x22222222u && g_edx==0x33333333u);
        CHECK(g_xmm0.u[0]==(row==1u?0x40800000u:row==2u?0xC0800000u:outputs[row]));
    }
    /* Cross product and translation, including aliased output and NaN operand order. */
    for(unsigned row=0;row<4u;row++) {
        fp_begin();const uint32_t record=0x13200u,point=0x11003u;
        const uint32_t output=row==1u?point:row==3u?g_esp+4u:0x12003u;
        guest_write32(0x74C458u,0x13000u);
        guest_write32(g_esp+4u,0xABCD0001u);guest_write32(g_esp+8u,point);guest_write32(g_esp+12u,output);
        guest_write32(record+0x134u,0x3F800000u);guest_write32(record+0x138u,row==2u?0x7FC12345u:0x40000000u);
        guest_write32(record+0x13Cu,0x40400000u);
        for(unsigned axis=0;axis<3u;axis++)guest_write32(record+0x140u+4u*axis,0x3F800000u);
        guest_write32(record+0x14Cu,0x41200000u);guest_write32(record+0x150u,0x41A00000u);guest_write32(record+0x154u,0x41F00000u);
        guest_write32(point,0x40800000u);guest_write32(point+4u,0x40A00000u);guest_write32(point+8u,row==2u?0xFFC54321u:0x40C00000u);
        invoke(0x00166060u,4u);
        CHECK(g_eax==record && g_ecx==output && g_edx==0x33333333u);
        CHECK(guest_read32(output)==(row==2u?0x7FC12345u:0x41000000u));
        if(row!=2u)CHECK(guest_read32(output+4u)==0x41C00000u && guest_read32(output+8u)==0x41E00000u);
    }
    /* Ordered linked-list insert: head, middle, equal tail and stack-slot alias. */
    const uint32_t holder=0x10003u,first=0x10103u,next=0x10203u,node=0x10303u;
    for(unsigned row=0;row<3u;row++) {
        fp_begin();guest_write32(g_esp+4u,holder);guest_write32(g_esp+8u,node);
        guest_write32(holder,row==0u?0u:first);guest_write32(first+0x1Cu,0x3F800000u);
        guest_write32(first+0x34u,next);guest_write32(next+0x1Cu,row==1u?0x40400000u:0x40000000u);
        guest_write32(next+0x34u,0u);guest_write32(node+0x1Cu,0x40000000u);
        invoke(0x00166770u,4u);
        if(row==0u)CHECK(guest_read32(holder)==node && guest_read32(node+0x34u)==0u);
        else if(row==1u)CHECK(g_eax==first && g_ecx==next && guest_read32(first+0x34u)==node && guest_read32(node+0x34u)==next);
        else CHECK(g_eax==next && g_ecx==0u && guest_read32(next+0x34u)==node && guest_read32(node+0x34u)==0u);
        CHECK(g_edx==node);
    }
    fp_begin();guest_write32(g_esp+4u,g_esp+8u);guest_write32(g_esp+8u,node);
    guest_write32(node+0x1Cu,0x3F800000u);guest_write32(node+0x34u,0u);
    invoke(0x00166770u,4u);CHECK(g_eax==node && g_ecx==0u && guest_read32(node+0x34u)==node);
}

static void test_t1477_vector001_stack_inputs(void)
{
    const uint32_t setters[]={0x0010EA50u,0x0010EA90u,0x0010EB90u};
    const uint32_t offsets[]={0x60u,0x5Cu,0x74u};
    for(unsigned row=0;row<3u;row++) {
        vec_begin();guest_write32(g_esp+4u,g_esp+4u);guest_write32(g_esp+8u,0x3F800000u);
        invoke(setters[row],4u);
        CHECK(g_eax==0x104u && guest_read32(0x104u+offsets[row])==0x3F800000u);
        CHECK(guest_read32(0x108u)==(row<2u?0x3F800400u:0x3F800000u));
        vec_check_xmm0(0x3F800000u);
    }
    vec_begin();guest_write32(g_esp+4u,0x9Cu);guest_write32(g_esp+8u,0x80000000u);
    guest_write32(g_esp+12u,0x7FC12345u);invoke(0x0010EB10u,4u);
    CHECK(g_eax==0x9Cu && guest_read32(0x104u)==0x80000000u && guest_read32(0x108u)==0x7FC12345u);
    vec_check_xmm0(0x7FC12345u);
    fp_begin();guest_write32(g_esp+4u,0x3F800000u);guest_write32(0x7450D0u,0x04903637u);
    guest_write32(0x475D24u,0x40000000u);guest_write32(0x475D1Cu,0x40000000u);
    guest_write32(0x4785D8u,0xC0000000u);guest_write32(g_esp+8u,0xDEADBEEFu);
    invoke(0x0012B8E0u,4u);CHECK(g_eax==0xE0u && guest_read32(0x108u)==0x40000000u);
}
