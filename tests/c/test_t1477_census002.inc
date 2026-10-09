static void census002_invoke(uint32_t va, uint32_t pop)
{
    const uint32_t sp = g_esp;
    const uint32_t saved[] =
    {
        g_ebx, g_ebp, g_esi, g_edi
    }
    ;
    const uint32_t ret = guest_read32(sp);
    invoke(va, pop);
    CHECK(g_ebx == saved[0] && g_ebp == saved[1]);
    CHECK(g_esi == saved[2] && g_edi == saved[3]);
    CHECK(guest_read32(sp) == ret);
}
static void test_t1477_census002(void)
{
    /* Original 0010E760;
    independent branch, alias and register controls. */
    {
        for(unsigned alias=0u;alias<2u;alias++)
        {
            prepare();
            uint32_t object=0x3000u,source=alias?object+0x28u:0x4000u;
            guest_write32(source,0x12345678u);
            guest_write32(source+4u,0x9ABCDEF0u);
            guest_write32(source+8u,0x87654321u);
            guest_write32(g_esp+4u,object);
            guest_write32(g_esp+8u,source);
            census002_invoke(0x0010E760u,4u);
            CHECK(g_eax==object);
            CHECK(g_ecx==(alias?0x12345678u:0x87654321u));
            CHECK(g_edx==(alias?0x12345678u:0x9ABCDEF0u));
            CHECK(guest_read32(object+0x2Cu)==0x12345678u&&guest_read32(object+0x30u)==g_edx&&guest_read32(object+0x34u)==g_ecx);
        }
        prepare();
        guest_write32(g_esp+4u,0u);
        census002_invoke(0x0010E760u,4u);
        CHECK(g_eax==0u&&g_ecx==0x22222222u&&g_edx==0x33333333u);
    }
    /* Original 001286D0;
    independent branch, alias and register controls. */
    {
        for(unsigned direction=0u;direction<2u;direction++)
        {
            prepare();
            g_df=direction;
            guest_write32(0x7497A8u,0x13579BDFu);
            for(uint32_t i=0u;i<108u;i++)guest_write32(0x742360u+(direction?0u-i*4u:i*4u),0xAABBCCDDu);
            for(uint32_t i=0u;i<380u;i++)guest_write32(0x7ABFA0u+(direction?0u-i*4u:i*4u),0xAABBCCDDu);
            census002_invoke(0x001286D0u,4u);
            CHECK(g_eax==0x13579BDFu&&g_ecx==0u&&g_edx==0x33333333u);
            CHECK(guest_read32(0x74235Cu)==0u&&guest_read32(0x742358u)==0x13579BDFu&&guest_read32(0x7ABF80u)==1u);
            CHECK(guest_read32(0x742360u)==0u&&guest_read32(0x7ABFA0u)==0u);
            CHECK(guest_read32(0x742360u+(direction?0u-107u*4u:107u*4u))==0u);
            CHECK(guest_read32(0x7ABFA0u+(direction?0u-379u*4u:379u*4u))==0u);
        }
        g_df=0;
    }
    /* Original 001553E0;
    independent branch, alias and register controls. */
    {
        for(unsigned alias=0u;alias<2u;alias++)
        {
            prepare();
            if(alias)g_esp=0x4E794Cu;
            guest_write32(g_esp+4u,11u);
            guest_write32(g_esp+8u,22u);
            guest_write32(g_esp+12u,33u);
            guest_write32(g_esp+16u,44u);
            census002_invoke(0x001553E0u,4u);
            CHECK(g_eax==44u&&g_ecx==22u&&g_edx==33u);
            CHECK(guest_read32(0x4E7958u)==11u&&guest_read32(0x4E795Cu)==22u&&guest_read32(0x4E7960u)==33u&&guest_read32(0x4E7964u)==44u);
        }
    }
    /* Original 0015BCA0;
    independent branch, alias and register controls. */
    {
        for(uint32_t index=0u;index<4u;index++)
        {
            prepare();
            guest_write32(g_esp+4u,0x3000u);
            guest_write32(0x307Cu,0x4000u);
            guest_write32(0x74C37Cu,index);
            census002_invoke(0x0015BCA0u,4u);
            CHECK(g_eax==index+1u&&g_ecx==0x4000u&&g_edx==0x33333333u);
            CHECK(guest_read32(0x74C37Cu)==index+1u&&guest_read32(0x7A3580u+index*4u)==0x4000u);
        }
    }
    /* Original 0015BFB0;
    independent branch, alias and register controls. */
    {
        const uint32_t counts[]=
        {
            0u,9u,10u
        }
        ;
        for(uint32_t index=0u;index<3u;index++)
        {
            for(unsigned test=0u;test<3u;test++)
            {
                prepare();
                uint32_t base=0x7A2900u+index*44u;
                guest_write32(0x7A295Cu,index);
                guest_write32(base,counts[test]);
                guest_write32(g_esp+4u,0x13579BDFu);
                census002_invoke(0x0015BFB0u,4u);
                CHECK(g_eax==(counts[test]<10u?counts[test]+1u:counts[test])&&g_ecx==base&&g_edx==0x13579BDFu);
                CHECK(guest_read32(base)==g_eax&&guest_read32(base+4u+counts[test]*4u)==0x13579BDFu);
            }
        }
    }
    /* Original 001665A0;
    independent branch, alias and register controls. */
    {
        const uint32_t counts[]=
        {
            0u,1u,3u,0xFFFFFFFFu
        }
        ;
        for(unsigned i=0u;i<4u;i++)
        {
            prepare();
            guest_write32(0x4E826Cu,counts[i]);
            guest_write32(0x74C468u,0xFFFFFFF0u);
            census002_invoke(0x001665A0u,4u);
            CHECK(g_eax==(counts[i]==1u||counts[i]==3u?0xFFFFFFF0u+(counts[i]-1u)*56u:0u));
            CHECK(g_ecx==(counts[i]==1u||counts[i]==3u?0xFFFFFFF0u:0x22222222u));
            CHECK(g_edx==0x33333333u&&guest_read32(0x4E826Cu)==(counts[i]==1u||counts[i]==3u?counts[i]-1u:counts[i]));
        }
    }
    /* Original 00167A70;
    independent branch, alias and register controls. */
    {
        for(uint32_t index=0u;index<8u;index++)
        {
            for(unsigned high=0u;high<2u;high++)
            {
                prepare();
                guest_write32(0x74C458u,high?0xFFFFFF00u:0x3000u);
                guest_write32(g_esp+4u,index|(high?0xABCD0000u:0u));
                census002_invoke(0x00167A70u,4u);
                CHECK(g_ecx==(high?0xFFFFFF00u:0x3000u));
                CHECK(g_eax==g_ecx+index*512u+0x17Cu&&g_edx==0x33333333u);
            }
        }
    }
    /* Original 001680B0;
    independent branch, alias and register controls. */
    {
        for(uint32_t index=0u;index<520u;index+=73u)
        {
            prepare();
            guest_write32(0x74C458u,0x3001u);
            guest_write32(0x3001u+index*512u+0x1C0u,0x13579BDFu^index);
            guest_write32(g_esp+4u,0xABCD0000u|index);
            census002_invoke(0x001680B0u,4u);
            CHECK(g_eax==(0x13579BDFu^index)&&g_ecx==0x3001u&&g_edx==0x33333333u);
        }
    }
    /* Original 00177E90;
    independent branch, alias and register controls. */
    {
        for(unsigned negative=0u;negative<4u;negative++)
        {
            prepare();
            guest_write32(g_esp+4u,0x3000u);
            guest_write32(0x307Cu,0x4000u);
            guest_write32(0x456Cu,negative==1u?0x80000000u:0u);
            guest_write32(0x4578u,negative==2u?0xFFFFFFFFu:0x7FFFFFFFu);
            guest_write32(0x457Cu,negative==3u?0x80000000u:7u);
            census002_invoke(0x00177E90u,4u);
            CHECK(g_eax==(negative==0u)&&g_edx==0x33333333u);
            CHECK(g_ecx==(negative==1u?0x80000000u:negative==2u?0xFFFFFFFFu:negative==3u?0x80000000u:7u));
        }
    }
}
