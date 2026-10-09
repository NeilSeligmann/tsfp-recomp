/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Function names for host and guest addresses, from the executable's own .symtab.
 * See host_symbols.h. Report time only: this allocates and reads files.
 */

#define _GNU_SOURCE
#include "host_symbols.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A symbol table or string table larger than this is not a real one. */
#define MAX_TABLE_BYTES (512u * 1024u * 1024u)
/* A zero size symbol (hand written assembly) is trusted only this far past its start. */
#define UNSIZED_REACH 0x100u
#define GUEST_NAME_LENGTH 12u /* "sub_" and 8 hex digits */
/* A guest VA this far past the nearest function start is not code of that function: the
 * guest stack's 0xDEAD0000 sentinel and heap addresses would otherwise be named after the
 * last lifted function in the image. The largest lifted function is about 28 KiB. */
#define GUEST_FUNCTION_REACH 0x40000u

typedef struct {
    uintptr_t address;
    uint64_t size;
    uint32_t name;
} host_function;

typedef struct {
    uint32_t guest_va;
    uint32_t name;
} guest_function;

struct host_symbol_table {
    char *names;
    host_function *functions;
    size_t function_count;
    guest_function *guests;
    size_t guest_count;
};

static bool read_exact(int file, void *out, size_t size, uint64_t offset)
{
    unsigned char *cursor = out;
    while (size > 0u) {
        const ssize_t got = pread(file, cursor, size, (off_t)offset);
        if (got <= 0) {
            return false;
        }
        cursor += got;
        size -= (size_t)got;
        offset += (uint64_t)got;
    }
    return true;
}

static int compare_host(const void *left, const void *right)
{
    const host_function *a = left;
    const host_function *b = right;
    return a->address < b->address ? -1 : a->address > b->address;
}

static int compare_guest(const void *left, const void *right)
{
    const guest_function *a = left;
    const guest_function *b = right;
    return a->guest_va < b->guest_va ? -1 : a->guest_va > b->guest_va;
}

/* "sub_00383DF3" to 0x00383DF3. Exactly eight hex digits, so `sub_00383DF3_x` is not one. */
static bool parse_guest_name(const char *name, uint32_t *va)
{
    if (strlen(name) != GUEST_NAME_LENGTH || strncmp(name, "sub_", 4u) != 0) {
        return false;
    }
    uint32_t value = 0u;
    for (size_t i = 4u; i < GUEST_NAME_LENGTH; i++) {
        const char c = name[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = (uint32_t)(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            digit = (uint32_t)(c - 'A') + 10u;
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    *va = value;
    return true;
}

static bool load_sections(int file, host_symbol_table *table, uintptr_t load_bias)
{
    Elf64_Ehdr header;
    if (!read_exact(file, &header, sizeof(header), 0u) ||
        memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_shentsize != sizeof(Elf64_Shdr) || header.e_shnum == 0u) {
        return false;
    }
    Elf64_Shdr *sections = malloc((size_t)header.e_shnum * sizeof(*sections));
    if (sections == NULL) {
        return false;
    }
    bool ok = read_exact(file, sections, (size_t)header.e_shnum * sizeof(*sections), header.e_shoff);
    const Elf64_Shdr *symtab = NULL;
    for (unsigned i = 0u; ok && i < header.e_shnum; i++) {
        if (sections[i].sh_type == SHT_SYMTAB) {
            symtab = &sections[i];
        }
    }
    Elf64_Sym *symbols = NULL;
    if (ok && symtab != NULL && symtab->sh_link < header.e_shnum) {
        const Elf64_Shdr *strings = &sections[symtab->sh_link];
        ok = symtab->sh_size <= MAX_TABLE_BYTES && strings->sh_size <= MAX_TABLE_BYTES &&
             symtab->sh_entsize == sizeof(Elf64_Sym) && strings->sh_size > 0u;
        if (ok) {
            symbols = malloc((size_t)symtab->sh_size);
            table->names = malloc((size_t)strings->sh_size);
            ok = symbols != NULL && table->names != NULL &&
                 read_exact(file, symbols, (size_t)symtab->sh_size, symtab->sh_offset) &&
                 read_exact(file, table->names, (size_t)strings->sh_size, strings->sh_offset);
        }
        const size_t count = ok ? (size_t)(symtab->sh_size / sizeof(Elf64_Sym)) : 0u;
        if (ok) {
            table->names[strings->sh_size - 1u] = '\0';
            table->functions = malloc((count + 1u) * sizeof(*table->functions));
            table->guests = malloc((count + 1u) * sizeof(*table->guests));
            ok = table->functions != NULL && table->guests != NULL;
        }
        for (size_t i = 0u; ok && i < count; i++) {
            const Elf64_Sym *symbol = &symbols[i];
            uint32_t va;
            if (ELF64_ST_TYPE(symbol->st_info) != STT_FUNC || symbol->st_shndx == SHN_UNDEF ||
                symbol->st_value == 0u || symbol->st_name >= strings->sh_size) {
                continue;
            }
            table->functions[table->function_count++] = (host_function){
                .address = (uintptr_t)symbol->st_value + load_bias,
                .size = symbol->st_size,
                .name = symbol->st_name,
            };
            if (parse_guest_name(table->names + symbol->st_name, &va)) {
                table->guests[table->guest_count++] =
                    (guest_function){.guest_va = va, .name = symbol->st_name};
            }
        }
    } else {
        ok = false;
    }
    free(symbols);
    free(sections);
    if (ok) {
        qsort(table->functions, table->function_count, sizeof(*table->functions), compare_host);
        qsort(table->guests, table->guest_count, sizeof(*table->guests), compare_guest);
    }
    return ok && table->function_count > 0u;
}

host_symbol_table *host_symbol_table_load(const char *elf_path, uintptr_t load_bias)
{
    const int file = open(elf_path, O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        return NULL;
    }
    host_symbol_table *table = calloc(1u, sizeof(*table));
    const bool ok = table != NULL && load_sections(file, table, load_bias);
    (void)close(file);
    if (!ok) {
        host_symbol_table_free(table);
        return NULL;
    }
    return table;
}

void host_symbol_table_free(host_symbol_table *table)
{
    if (table == NULL) {
        return;
    }
    free(table->names);
    free(table->functions);
    free(table->guests);
    free(table);
}

bool host_symbol_table_describe(const host_symbol_table *table, uintptr_t address, char *out,
                                size_t size)
{
    if (table == NULL || out == NULL || size == 0u) {
        return false;
    }
    /* Greatest function start at or below the address. */
    size_t low = 0u;
    size_t high = table->function_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2u;
        if (table->functions[middle].address <= address) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    if (low == 0u) {
        return false;
    }
    const host_function *function = &table->functions[low - 1u];
    const uint64_t offset = address - function->address;
    const bool inside = function->size != 0u ? offset < function->size : offset < UNSIZED_REACH;
    if (!inside) {
        return false;
    }
    (void)snprintf(out, size, "%s+0x%llX", table->names + function->name,
                   (unsigned long long)offset);
    return true;
}

bool host_symbol_table_guest_function(const host_symbol_table *table, uint32_t guest_va,
                                      char *out, size_t size)
{
    if (table == NULL || out == NULL || size == 0u) {
        return false;
    }
    size_t low = 0u;
    size_t high = table->guest_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2u;
        if (table->guests[middle].guest_va <= guest_va) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    if (low == 0u) {
        return false;
    }
    const guest_function *function = &table->guests[low - 1u];
    if (guest_va - function->guest_va >= GUEST_FUNCTION_REACH) {
        return false;
    }
    (void)snprintf(out, size, "%s+0x%X", table->names + function->name,
                   (unsigned)(guest_va - function->guest_va));
    return true;
}

/* --- the running executable ------------------------------------------------ */

static pthread_once_t self_once = PTHREAD_ONCE_INIT;
static host_symbol_table *self_table;

/* The first object dl_iterate_phdr reports is the main program. */
static int first_object(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    *(uintptr_t *)data = (uintptr_t)info->dlpi_addr;
    return 1;
}

static void load_self(void)
{
    uintptr_t bias = 0u;
    (void)dl_iterate_phdr(first_object, &bias);
    self_table = host_symbol_table_load("/proc/self/exe", bias);
}

bool host_symbols_describe(uintptr_t address, char *out, size_t size)
{
    (void)pthread_once(&self_once, load_self);
    if (host_symbol_table_describe(self_table, address, out, size)) {
        return true;
    }
    Dl_info info;
    if (out != NULL && size > 0u && dladdr((const void *)address, &info) != 0 &&
        info.dli_sname != NULL && info.dli_saddr != NULL) {
        const char *file = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;
        file = file ? file + 1 : info.dli_fname;
        (void)snprintf(out, size, "%s!%s+0x%lX", file ? file : "?", info.dli_sname,
                       (unsigned long)(address - (uintptr_t)info.dli_saddr));
        return true;
    }
    return false;
}

bool host_symbols_guest_function(uint32_t guest_va, char *out, size_t size)
{
    (void)pthread_once(&self_once, load_self);
    return host_symbol_table_guest_function(self_table, guest_va, out, size);
}
