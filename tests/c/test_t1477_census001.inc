static void census001_invoke(uint32_t va, uint32_t pop)
{
    const uint32_t sp = g_esp;
    const uint32_t saved[] = {g_ebx, g_ebp, g_esi, g_edi};
    const uint32_t ret = guest_read32(sp);
    invoke(va, pop);
    CHECK(g_ebx == saved[0] && g_ebp == saved[1]);
    CHECK(g_esi == saved[2] && g_edi == saved[3]);
    CHECK(guest_read32(sp) == ret);
}
static void test_t1477_census001(void)
{
    /* Original 0010E9A0;
    independent branch, alias and register controls. */
    {
        for(unsigned alias=0u;alias<2u;alias++)
        {
            prepare();
            uint32_t object=0x3000u,source=alias?object+0x94u:0x4000u;
            guest_write32(source,0x11123456u);
            guest_write32(source+4u,0x22789ABCu);
            guest_write32(source+8u,0x33DEF012u);
            guest_write32(object+4u,0x40u);
            guest_write32(g_esp+4u,object);
            guest_write32(g_esp+8u,source);
            census001_invoke(0x0010E9A0u,4u);
            CHECK(g_eax==object);
            CHECK(g_edx==(alias?0x11123456u:0x22789ABCu));
            CHECK(g_ecx==(alias?0x11123456u:0x33DEF012u));
            CHECK(guest_read32(object+0x98u)==0x11123456u);
            CHECK(guest_read32(object+0x9Cu)==(alias?0x11123456u:0x22789ABCu));
            CHECK(guest_read32(object+0xA0u)==(alias?0x11123456u:0x33DEF012u));
            CHECK(guest_read32(object+4u)==0x4040u);
        }
        prepare();
        guest_write32(g_esp+4u,0u);
        census001_invoke(0x0010E9A0u,4u);
        CHECK(g_eax==0u&&g_ecx==0x22222222u&&g_edx==0x33333333u);
    }
    /* Original 0010E9D0;
    independent branch, alias and register controls. */
    {
        prepare();
        guest_write32(0x7AD28Cu,0x3000u);
        guest_write32(0x4000u,0x12u);
        guest_write32(0x4004u,0x34u);
        guest_write32(0x4008u,0x56u);
        guest_write32(g_esp+4u,0x4000u);
        guest_write32(0x3004u,0x10u);
        census001_invoke(0x0010E9D0u,4u);
        CHECK(g_eax==0x3000u&&g_ecx==0x3000u&&g_edx==0x3000u);
        CHECK(guest_read32(0x3098u)==0x12u&&guest_read32(0x309Cu)==0x34u&&guest_read32(0x30A0u)==0x56u);
        CHECK(guest_read32(0x3004u)==0x4010u);
        prepare();
        guest_write32(0x7AD28Cu,0u);
        census001_invoke(0x0010E9D0u,4u);
        CHECK(g_ecx==0u&&g_eax==0x11111111u&&g_edx==0x33333333u);
    }
    /* Original 0010EBD0;
    independent branch, alias and register controls. */
    {
        for(uint32_t kind=0u;kind<5u;kind++)
        {
            prepare();
            guest_write32(g_esp+4u,0x3000u);
            guest_write32(0x3050u,kind);
            census001_invoke(0x0010EBD0u,4u);
            CHECK(g_eax==(kind==2u)&&g_ecx==(kind==2u)&&g_edx==kind);
        }
        prepare();
        guest_write32(g_esp+4u,0u);
        census001_invoke(0x0010EBD0u,4u);
        CHECK(g_eax==0u&&g_ecx==0x22222222u&&g_edx==0x33333333u);
    }
    /* Original 00115EA0;
    independent branch, alias and register controls. */
    {
        for(uint32_t active=0u;active<2u;active++)
        {
            prepare();
            guest_write32(0x6B7AA8u,active);
            guest_write32(0x73D96Cu,0x13579BDFu);
            census001_invoke(0x00115EA0u,4u);
            CHECK(g_eax==active);
            CHECK(guest_read32(0x73D96Cu)==(active?0x13579BDFu:0u));
            CHECK(g_ecx==0x22222222u&&g_edx==0x33333333u);
        }
    }
    /* Original 0012B960;
    independent branch, alias and register controls. */
    {
        for(uint32_t clock=0u;clock<3u;clock++)
        {
            for(uint32_t value=0u;value<256u;value+=17u)
            {
                prepare();
                uint32_t color=0xA5000000u|(value<<16)|((255u-value)<<8)|value;
                guest_write32(g_esp+4u,color);
                guest_write32(0x7450D0u,clock);
                g_ebx=0xBBBBBBBBu;
                g_esi=0x55555555u;
                census001_invoke(0x0012B960u,4u);
                uint32_t index=clock?clock-1u:199u;
                uint32_t packed=(value/3u<<24)|((255u-value)/3u<<16)|(value/3u<<8);
                CHECK(guest_read32(0x742544u+index*56u)==packed);
                CHECK(g_ecx==index*56u&&g_edx==value/3u);
                CHECK(g_eax==(uint32_t)((uint64_t)0xAAAAAAABu*value));
                CHECK(g_ebx==0xBBBBBBBBu&&g_esi==0x55555555u);
                CHECK(guest_read32(g_esp-8u)==0xBBBBBBBBu&&guest_read32(g_esp-12u)==0x55555555u);
            }
        }
    }
    /* Original 00158D40;
    independent branch, alias and register controls. */
    {
        for(unsigned first=0u;first<2u;first++)
        {
            for(unsigned second=0u;second<2u;second++)
            {
                prepare();
                guest_write32(g_esp+4u,0x3000u);
                guest_write32(0x307Cu,0x4000u);
                guest_write32(0x4028u,0xFFFFFFFFu);
                guest_write32(0x402Cu,0xFFFFFFFFu);
                guest_write32(g_esp+8u,first?0x04000000u:0u);
                guest_write32(g_esp+16u,second?0x08000000u:0u);
                g_esi=0x51525354u;
                census001_invoke(0x00158D40u,4u);
                CHECK(guest_read32(0x402Cu)==0xFFFFFBDEu);
                CHECK(guest_read32(0x4028u)==(second&&!first?0xFFFBB77Cu:0xFFFBB7FCu));
                CHECK(g_eax==0x4000u&&g_edx==0u);
                CHECK(g_ecx==(second?(first?0x04000000u:0u):0u));
                CHECK(g_esi==0x51525354u);
            }
        }
    }
    /* Original 00160E80;
    independent branch, alias and register controls. */
    {
        for(unsigned selected=0u;selected<20u;selected++)
        {
            prepare();
            for(unsigned i=0u;i<20u;i++)guest_write32(0x7A2D84u+i*96u,i==selected?1u:0u);
            guest_write32(g_esp+4u,1u<<selected);
            g_esi=0x51525354u;
            census001_invoke(0x00160E80u,4u);
            CHECK(g_eax==1u&&g_ecx==selected&&g_edx==(1u<<selected));
            CHECK(g_esi==0x51525354u);
        }
        prepare();
        guest_write32(g_esp+4u,0u);
        census001_invoke(0x00160E80u,4u);
        CHECK(g_eax==0u&&g_ecx==20u&&g_edx==0u);
    }
    /* Original 001656B0;
    independent branch, alias and register controls. */
    {
        for(unsigned high=0u;high<2u;high++)
        {
            for(uint32_t index=0u;index<520u;index+=73u)
            {
                prepare();
                guest_write32(0x74C46Cu,0x3001u);
                guest_write32(0x3001u+index*4u,0x13579BDFu^index);
                guest_write32(g_esp+4u,index|(high?0xFFFF0000u:0u));
                census001_invoke(0x001656B0u,4u);
                CHECK(g_eax==(0x13579BDFu^index)&&g_ecx==0x3001u&&g_edx==0x33333333u);
            }
        }
    }
    /* Original 00167A10;
    independent branch, alias and register controls. */
    {
        for(uint32_t index=0u;index<520u;index+=73u)
        {
            for(unsigned high=0u;high<2u;high++)
            {
                prepare();
                guest_write32(0x74C458u,high?0xFFFFFF00u:0x3000u);
                guest_write32(g_esp+4u,index|(high?0xABCD0000u:0u));
                census001_invoke(0x00167A10u,4u);
                CHECK(g_ecx==(high?0xFFFFFF00u:0x3000u));
                CHECK(g_eax==g_ecx+index*512u+0x134u&&g_edx==0x33333333u);
            }
        }
    }
}
