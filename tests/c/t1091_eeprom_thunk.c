/* SPDX-License-Identifier: GPL-3.0-or-later
 * Declared synthetic authenticated input; actual xemu keys are compared privately
 * by the Python suite. This tests real mapping/patch/publication lifetimes. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "kernel_thunk.h"
#include "kernel_config.h"
#include "kernel_crypto.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

int main(void)
{
    kernel_config_reset();
    assert(!kernel_thunk_publish_eeprom_keys());
    assert(kernel_thunk_map_window());
    uint8_t *annex=(uint8_t *)(uintptr_t)KERNEL_THUNK_ANNEX_VA(0u);
    memset(annex,0xa5,KERNEL_THUNK_ANNEX_BYTES_USED);
    assert(!kernel_thunk_publish_eeprom_keys() && !kernel_thunk_eeprom_keys_available());
    for(unsigned i=0u;i<128u;i++) assert(annex[i]==0xa5u);
    uint32_t *slots=mmap(NULL,4096u,PROT_READ|PROT_WRITE,
                         MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    assert(slots!=MAP_FAILED && (uintptr_t)slots<=UINT32_MAX);
    const uint32_t encoded[]={0x80000000u|321u,0x80000000u|323u,
                             0x80000000u|322u,0x80000000u|324u,
                             0x80000000u|41u,0x80000000u|42u};
    memcpy(slots,encoded,sizeof(encoded));
    size_t skipped=99u;
    assert(kernel_thunk_patch_table((uint32_t)(uintptr_t)slots,6u,&skipped)==6u);
    assert(skipped==0u && slots[0]==0u && slots[1]==0u && slots[4]==0u && slots[5]==0u);
    assert(slots[2]==KERNEL_THUNK_VA_XBOX_HARDWARE_INFO && slots[3]==KERNEL_THUNK_VA_XBOX_KRNL_VERSION);
    uint8_t key[16],eeprom[256],plaintext[28],digest[20],state[KERNEL_RC4_STATE_BYTES];
    for(unsigned i=0u;i<16u;i++) key[i]=(uint8_t)i;
    for(unsigned i=0u;i<28u;i++) plaintext[i]=(uint8_t)(i*3u+2u);
    memset(eeprom,0x5a,sizeof(eeprom));
    kernel_crypto_hmac_sha1(key,16u,plaintext,28u,eeprom);
    kernel_crypto_hmac_sha1(key,16u,eeprom,20u,digest);
    assert(kernel_crypto_rc4_key(state,digest,20u));
    memcpy(eeprom+20u,plaintext,28u);
    assert(kernel_crypto_rc4_crypt(state,eeprom+20u,28u));
    assert(kernel_config_set_eeprom_keyed(eeprom,256u,key,16u));
    assert(kernel_thunk_publish_eeprom_keys() && kernel_thunk_eeprom_keys_available());
    assert(memcmp(annex+96u,key,16u)==0 && memcmp(annex+112u,plaintext+8u,16u)==0);
    for(unsigned i=0u;i<96u;i++) assert(annex[i]==0xa5u);
    memcpy(slots,encoded,sizeof(encoded));
    assert(kernel_thunk_patch_table((uint32_t)(uintptr_t)slots,6u,&skipped)==6u);
    assert(slots[0]==KERNEL_THUNK_VA_EEPROM_KEY && slots[1]==KERNEL_THUNK_VA_HD_KEY);
    assert(slots[2]==KERNEL_THUNK_VA_XBOX_HARDWARE_INFO && slots[3]==KERNEL_THUNK_VA_XBOX_KRNL_VERSION);
    assert(slots[4]==0u && slots[5]==0u);
    assert(kernel_config_set_eeprom(eeprom,256u));
    assert(!kernel_thunk_publish_eeprom_keys() && !kernel_thunk_eeprom_keys_available());
    memcpy(slots,encoded,sizeof(encoded));
    assert(kernel_thunk_patch_table((uint32_t)(uintptr_t)slots,6u,&skipped)==6u);
    assert(slots[0]==0u && slots[1]==0u);
    kernel_thunk_unmap_window();
    assert(!kernel_thunk_eeprom_keys_available() && !kernel_thunk_disk_identity_available());
    assert(kernel_thunk_map_window() && !kernel_thunk_eeprom_keys_available());
    for(unsigned i=0u;i<128u;i++) assert(annex[i]==0u);
    kernel_thunk_unmap_window();
    assert(munmap(slots,4096u)==0);
    return 0;
}
