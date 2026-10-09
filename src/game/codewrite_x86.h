/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The general exact interpreter of the code-write replacement ABI (T1508, second admission pass).
 * codewrite_replace.h carries the small interpreter of the constructor-family roots (an
 * instruction subset of about fifteen opcodes without EFLAGS). The remaining rejected roots
 * (_strtol 003C97AB, the 003F720 generator fill, the MD5 wrapper 003C2260 and the bit packers
 * 0038AEB0/0038AF20/0038C010/003B9BC0) call loops, shifts, double shifts, DIV and flag-dependent
 * branches, so this header executes the 32-bit integer instruction set those closures and the
 * bytes a hostile store can leave behind decode to, WITH EFLAGS (jcc, adc/sbb, setcc and cmov
 * consume them), fetching every instruction from the LIVE guest bytes (x86 self-modifying code
 * rule: a store never changes the instruction already decoded).
 *
 * Coverage (post-hoc, chosen from the disassembly of the covered closures and widened to the
 * neighbouring one-byte and 0F opcodes a rewritten byte most often decodes to; every opcode
 * outside the list is a refusal, CODEWRITE_UNSUPPORTED, never a guess):
 *   ALU 00-3D (add or adc sbb and sub xor cmp, all six forms), inc/dec r32, push/pop r32,
 *   push imm, imul r,rm,imm, jcc rel8/rel32, group 1 (80 81 83), test, xchg, mov (88-8B, A0-A3,
 *   B0-BF, C6 C7), lea, nop, cwde/cdq, movs/stos (with rep), test al/eax,imm, shifts and
 *   rotates (C0 C1 D0-D3, rol ror shl shr sar), ret/ret imm16, leave, call/jmp (rel and
 *   r/m), push r/m, group 3 (test not neg mul imul div idiv), 0F movzx/movsx, shld/shrd, imul,
 *   setcc, cmovcc, AAM, INT3 and ICEBP. Operand size 16 (prefix 66) is supported for the forms
 *   above except shld/shrd, mul/div and string ops, which refuse it. Segment, address-size, lock
 *   and F2 prefixes refuse.
 * Flags follow QEMU (what Unicorn runs): undefined results are computed the way its lazy flag
 * evaluation does (AF cleared by logic ops and shifts, OF of a multi-bit shift from the last
 * shifted-out and result sign bits, DIV leaves flags untouched).
 * Memory is bounds-checked by codewrite_read/codewrite_write (the 16 MiB harness window in proof
 * builds). A faulting instruction commits nothing except completed `rep` iterations.
 */
#ifndef TSFP_GAME_CODEWRITE_X86_H
#define TSFP_GAME_CODEWRITE_X86_H

#include "codewrite_replace.h"

#define CW86_CF 0x0001u
#define CW86_PF 0x0004u
#define CW86_AF 0x0010u
#define CW86_ZF 0x0040u
#define CW86_SF 0x0080u
#define CW86_DF 0x0400u
#define CW86_OF 0x0800u
/* Stop kind of an original that has not returned after the step budget: a deterministic stop of
 * the model, reported with the state after exactly max_steps instructions. */
#define CW86_BUDGET 'B'

/* hlt: Unicorn returns normally with eip after the halt (counted as executed); stop kind H. */
#define CW86_HALT 'H'

#define CW86_ARITH (CW86_CF | CW86_PF | CW86_AF | CW86_ZF | CW86_SF | CW86_OF)

typedef struct cw86 {
    uint32_t *reg;
    uint32_t eflags;
    uint32_t eip;     /* start of the current instruction */
    uint32_t length;  /* bytes consumed by the current instruction so far */
    unsigned opsize;  /* 1, 2 or 4 bytes for the current operation */
    int operand16;    /* 66 prefix seen */
    int rep;          /* 1: F3 (rep, repe), 2: F2 (repne) */
    int taken;        /* a control transfer set `target` */
    uint32_t target;
} cw86;

typedef struct cw86_operand {
    int is_register;
    unsigned field; /* modrm.reg */
    unsigned rm;    /* modrm.rm, the register when is_register */
    uint32_t address;
} cw86_operand;

/* Internal status: 0 ok, otherwise a stop kind letter (or CODEWRITE_UNSUPPORTED). */
#define CW86_STATUS_FETCH CODEWRITE_FETCH_FAULT
#define CW86_STATUS_READ CODEWRITE_READ_FAULT
#define CW86_STATUS_WRITE CODEWRITE_WRITE_FAULT
#define CW86_STATUS_DIVIDE 'X'

static inline uint32_t cw86_mask(unsigned size)
{
    return size == 4u ? 0xFFFFFFFFu : (size == 2u ? 0xFFFFu : 0xFFu);
}

static inline unsigned cw86_bits(unsigned size) { return size * 8u; }

static inline uint32_t cw86_sign(unsigned size) { return 1u << (cw86_bits(size) - 1u); }

static inline int cw86_fetch(cw86 *c, unsigned count, uint32_t *value)
{
    uint32_t result = 0u;
    for (unsigned i = 0u; i < count; i++) {
        uint32_t byte;
        if (codewrite_read(c->eip + c->length + i, 1u, &byte)) {
            return CW86_STATUS_FETCH;
        }
        result |= byte << (8u * i);
    }
    c->length += count;
    *value = result;
    return 0;
}

static inline uint32_t cw86_sext(uint32_t value, unsigned size)
{
    return size == 1u ? (uint32_t)(int32_t)(int8_t)value
                      : (size == 2u ? (uint32_t)(int32_t)(int16_t)value : value);
}

#define CW86_TRY(expression) \
    do { int cw86_status_ = (expression); if (cw86_status_) { return cw86_status_; } } while (0)

/* ModRM with SIB and displacement. Registers are the ones at the start of the instruction. */
static inline int cw86_modrm(cw86 *c, cw86_operand *m)
{
    uint32_t byte, displacement = 0u, base = 0u;
    CW86_TRY(cw86_fetch(c, 1u, &byte));
    unsigned mod = byte >> 6, rm = byte & 7u;
    m->field = (byte >> 3) & 7u;
    m->rm = rm;
    m->is_register = mod == 3u;
    m->address = 0u;
    if (mod == 3u) {
        return 0;
    }
    int has_base = 1;
    uint32_t index_part = 0u;
    if (rm == 4u) {
        uint32_t sib;
        CW86_TRY(cw86_fetch(c, 1u, &sib));
        unsigned scale = sib >> 6, index = (sib >> 3) & 7u, sbase = sib & 7u;
        if (index != 4u) {
            index_part = c->reg[index] << scale;
        }
        if (sbase == 5u && mod == 0u) {
            has_base = 0;
        } else {
            base = c->reg[sbase];
        }
    } else if (rm == 5u && mod == 0u) {
        has_base = 0;
    } else {
        base = c->reg[rm];
    }
    if (mod == 1u) {
        CW86_TRY(cw86_fetch(c, 1u, &displacement));
        displacement = cw86_sext(displacement, 1u);
    } else if (mod == 2u || !has_base) {
        CW86_TRY(cw86_fetch(c, 4u, &displacement));
    }
    m->address = (has_base ? base : 0u) + index_part + displacement;
    return 0;
}

static inline uint32_t cw86_get_register(const cw86 *c, unsigned index, unsigned size)
{
    if (size == 1u) {
        return codewrite_get8(c->reg, index);
    }
    return c->reg[index] & cw86_mask(size);
}

static inline void cw86_set_register(cw86 *c, unsigned index, unsigned size, uint32_t value)
{
    if (size == 1u) {
        codewrite_set8(c->reg, index, (uint8_t)value);
    } else if (size == 2u) {
        c->reg[index] = (c->reg[index] & 0xFFFF0000u) | (value & 0xFFFFu);
    } else {
        c->reg[index] = value;
    }
}

static inline int cw86_read_operand(const cw86 *c, const cw86_operand *m, unsigned size,
                                    uint32_t *value)
{
    if (m->is_register) {
        *value = cw86_get_register(c, m->rm, size);
        return 0;
    }
    return codewrite_read(m->address, size, value) ? CW86_STATUS_READ : 0;
}

static inline int cw86_write_operand(cw86 *c, const cw86_operand *m, unsigned size, uint32_t value)
{
    if (m->is_register) {
        cw86_set_register(c, m->rm, size, value);
        return 0;
    }
    return codewrite_write(m->address, value, size) ? CW86_STATUS_WRITE : 0;
}

/* ---- flags ---- */

static inline uint32_t cw86_szp(uint32_t result, unsigned size)
{
    uint32_t flags = 0u;
    result &= cw86_mask(size);
    if (result & cw86_sign(size)) {
        flags |= CW86_SF;
    }
    if (result == 0u) {
        flags |= CW86_ZF;
    }
    if (!__builtin_parity(result & 0xFFu)) {
        flags |= CW86_PF;
    }
    return flags;
}

static inline void cw86_set_arith_flags(cw86 *c, uint32_t flags)
{
    c->eflags = (c->eflags & ~CW86_ARITH) | flags;
}

/* ALU operation 0 add, 1 or, 2 adc, 3 sbb, 4 and, 5 sub, 6 xor, 7 cmp. Returns the result and sets
 * every arithmetic flag the way QEMU does. */
static inline uint32_t cw86_alu(cw86 *c, unsigned operation, uint32_t left, uint32_t right,
                                unsigned size)
{
    const uint32_t mask = cw86_mask(size);
    const uint32_t sign = cw86_sign(size);
    const uint32_t carry_in = ((operation == 2u || operation == 3u) && (c->eflags & CW86_CF)) ? 1u : 0u;
    uint32_t result, flags = 0u;
    left &= mask;
    right &= mask;
    switch (operation) {
    case 0u:
    case 2u: {
        uint64_t wide = (uint64_t)left + right + carry_in;
        result = (uint32_t)wide & mask;
        if (wide >> cw86_bits(size)) {
            flags |= CW86_CF;
        }
        if ((~(left ^ right) & (left ^ result)) & sign) {
            flags |= CW86_OF;
        }
        if ((left ^ right ^ result) & 0x10u) {
            flags |= CW86_AF;
        }
        break;
    }
    case 3u:
    case 5u:
    case 7u: {
        uint64_t wide = (uint64_t)left - right - carry_in;
        result = (uint32_t)wide & mask;
        if ((uint64_t)left < (uint64_t)right + carry_in) {
            flags |= CW86_CF;
        }
        if (((left ^ right) & (left ^ result)) & sign) {
            flags |= CW86_OF;
        }
        if ((left ^ right ^ result) & 0x10u) {
            flags |= CW86_AF;
        }
        break;
    }
    case 1u:
        result = left | right;
        break;
    case 4u:
        result = left & right;
        break;
    default:
        result = left ^ right;
        break;
    }
    flags |= cw86_szp(result, size);
    cw86_set_arith_flags(c, flags);
    return result;
}

/* inc (delta +1) and dec (-1) keep CF. */
static inline uint32_t cw86_incdec(cw86 *c, uint32_t value, int delta, unsigned size)
{
    const uint32_t saved = c->eflags & CW86_CF;
    const uint32_t result = cw86_alu(c, delta > 0 ? 0u : 5u, value, 1u, size);
    c->eflags = (c->eflags & ~CW86_CF) | saved;
    return result;
}

static inline uint32_t cw86_neg(cw86 *c, uint32_t value, unsigned size)
{
    return cw86_alu(c, 5u, 0u, value, size);
}

/* Shifts and rotates, operation = modrm.reg of group 2: 0 rol, 1 ror, 4/6 shl, 5 shr, 7 sar. */
static inline int cw86_shift(cw86 *c, unsigned operation, uint32_t value, uint32_t count,
                             unsigned size, uint32_t *out)
{
    const unsigned bits = cw86_bits(size);
    const uint32_t mask = cw86_mask(size);
    uint64_t last, result;
    uint32_t flags;
    count &= 0x1Fu;
    value &= mask;
    *out = value;
    if (count == 0u) {
        return 0;
    }
    switch (operation) {
    case 0u:
    case 1u: {
        const unsigned turn = count % bits;
        uint32_t rotated;
        if (operation == 0u) {
            rotated = turn ? ((value << turn) | (value >> (bits - turn))) & mask : value;
            flags = ((rotated & 1u) ? CW86_CF : 0u) |
                    ((((rotated >> (bits - 1u)) ^ rotated) & 1u) ? CW86_OF : 0u);
        } else {
            rotated = turn ? ((value >> turn) | (value << (bits - turn))) & mask : value;
            flags = ((rotated >> (bits - 1u)) & 1u ? CW86_CF : 0u) |
                    ((((rotated >> (bits - 1u)) ^ (rotated >> (bits - 2u))) & 1u) ? CW86_OF : 0u);
        }
        c->eflags = (c->eflags & ~(CW86_CF | CW86_OF)) | flags;
        *out = rotated;
        return 0;
    }
    case 4u:
    case 6u:
        last = ((uint64_t)value << (count - 1u)) & mask;
        result = (last << 1) & mask;
        flags = ((last >> (bits - 1u)) & 1u ? CW86_CF : 0u);
        break;
    case 5u:
        last = (uint64_t)value >> (count - 1u);
        result = last >> 1;
        flags = (last & 1u) ? CW86_CF : 0u;
        break;
    case 7u: {
        const int64_t signed_value = (int64_t)(int32_t)cw86_sext(value, size);
        last = ((uint64_t)(signed_value >> (count - 1u))) & mask;
        result = ((uint64_t)(signed_value >> count)) & mask;
        flags = (last & 1u) ? CW86_CF : 0u;
        break;
    }
    default:
        return CODEWRITE_UNSUPPORTED; /* rcl and rcr */
    }
    if (((last ^ result) >> (bits - 1u)) & 1u) {
        flags |= CW86_OF;
    }
    flags |= cw86_szp((uint32_t)result, size);
    cw86_set_arith_flags(c, flags);
    *out = (uint32_t)result & mask;
    return 0;
}

static inline int cw86_condition(const cw86 *c, unsigned code)
{
    const uint32_t f = c->eflags;
    const int cf = (f & CW86_CF) != 0, zf = (f & CW86_ZF) != 0, sf = (f & CW86_SF) != 0;
    const int of = (f & CW86_OF) != 0, pf = (f & CW86_PF) != 0;
    int result;
    switch (code >> 1) {
    case 0u: result = of; break;
    case 1u: result = cf; break;
    case 2u: result = zf; break;
    case 3u: result = cf || zf; break;
    case 4u: result = sf; break;
    case 5u: result = pf; break;
    case 6u: result = sf != of; break;
    default: result = zf || (sf != of); break;
    }
    return (code & 1u) ? !result : result;
}

static inline int cw86_push(cw86 *c, uint32_t value, unsigned size)
{
    if (codewrite_write(c->reg[CW_ESP] - size, value, size)) {
        return CW86_STATUS_WRITE;
    }
    c->reg[CW_ESP] -= size;
    return 0;
}

static inline int cw86_pop(cw86 *c, unsigned size, uint32_t *value)
{
    if (codewrite_read(c->reg[CW_ESP], size, value)) {
        return CW86_STATUS_READ;
    }
    c->reg[CW_ESP] += size;
    return 0;
}

/* One-operand MUL/IMUL/DIV/IDIV on size 4 (and DIV/IDIV on size 1). */
static inline int cw86_group3_wide(cw86 *c, unsigned operation, uint32_t operand, unsigned size)
{
    uint32_t *eax = &c->reg[CW_EAX], *edx = &c->reg[CW_EDX];
    if (size == 4u) {
        if (operation == 4u || operation == 5u) { /* mul, imul */
            uint64_t product;
            uint32_t high;
            int overflow;
            if (operation == 4u) {
                product = (uint64_t)*eax * operand;
                high = (uint32_t)(product >> 32);
                overflow = high != 0u;
            } else {
                int64_t signed_product = (int64_t)(int32_t)*eax * (int64_t)(int32_t)operand;
                product = (uint64_t)signed_product;
                high = (uint32_t)(product >> 32);
                overflow = signed_product != (int64_t)(int32_t)(uint32_t)product;
            }
            *eax = (uint32_t)product;
            *edx = high;
            cw86_set_arith_flags(c, cw86_szp(*eax, 4u) | (overflow ? (CW86_CF | CW86_OF) : 0u));
            return 0;
        }
        if (operand == 0u) {
            return CW86_STATUS_DIVIDE;
        }
        if (operation == 6u) { /* div */
            const uint64_t dividend = ((uint64_t)*edx << 32) | *eax;
            const uint64_t quotient = dividend / operand;
            if (quotient > 0xFFFFFFFFu) {
                return CW86_STATUS_DIVIDE;
            }
            *eax = (uint32_t)quotient;
            *edx = (uint32_t)(dividend % operand);
            return 0;
        } else { /* idiv */
            const int64_t dividend = (int64_t)(((uint64_t)*edx << 32) | *eax);
            const int64_t divisor = (int64_t)(int32_t)operand;
            if (dividend == INT64_MIN && divisor == -1) {
                return CW86_STATUS_DIVIDE;
            }
            const int64_t quotient = dividend / divisor;
            if (quotient > INT32_MAX || quotient < INT32_MIN) {
                return CW86_STATUS_DIVIDE;
            }
            *eax = (uint32_t)quotient;
            *edx = (uint32_t)(dividend % divisor);
            return 0;
        }
    }
    if (size == 1u && (operation == 6u || operation == 7u)) {
        const uint32_t ax = c->reg[CW_EAX] & 0xFFFFu;
        uint32_t quotient, remainder;
        if (operand == 0u) {
            return CW86_STATUS_DIVIDE;
        }
        if (operation == 6u) {
            quotient = ax / operand;
            remainder = ax % operand;
            if (quotient > 0xFFu) {
                return CW86_STATUS_DIVIDE;
            }
        } else {
            const int32_t dividend = (int16_t)ax, divisor = (int8_t)operand;
            const int32_t signed_quotient = dividend / divisor;
            if (signed_quotient > 127 || signed_quotient < -128) {
                return CW86_STATUS_DIVIDE;
            }
            quotient = (uint32_t)signed_quotient & 0xFFu;
            remainder = (uint32_t)(dividend % divisor) & 0xFFu;
        }
        c->reg[CW_EAX] = (c->reg[CW_EAX] & 0xFFFF0000u) | (remainder << 8) | (quotient & 0xFFu);
        return 0;
    }
    return CODEWRITE_UNSUPPORTED;
}

/* Executes one instruction whose first byte (after prefixes) is `opcode`. Returns 0 or a stop
 * kind. A control transfer sets c->target. `*stop_eip` overrides the reported EIP of a stop (INT3). */
#define CW86_JUMP(address) do { c->target = (address); c->taken = 1; } while (0)

static inline int cw86_execute(cw86 *c, uint32_t opcode, uint32_t *stop_eip)
{
    cw86_operand m = {0, 0, 0, 0u};
    uint32_t left, right, result, immediate;
    const unsigned size = c->opsize;

    if (opcode < 0x40u && (opcode & 7u) < 6u) { /* ALU */
        const unsigned operation = opcode >> 3, form = opcode & 7u;
        const unsigned width = (form & 1u) ? size : 1u;
        if (form < 4u) {
            CW86_TRY(cw86_modrm(c, &m));
            CW86_TRY(cw86_read_operand(c, &m, width, &left));
            right = cw86_get_register(c, m.field, width);
            if (form >= 2u) { /* Gv, Ev: the register is the destination */
                const uint32_t tmp = left;
                left = right;
                right = tmp;
                result = cw86_alu(c, operation, left, right, width);
                if (operation != 7u) {
                    cw86_set_register(c, m.field, width, result);
                }
            } else {
                result = cw86_alu(c, operation, left, right, width);
                if (operation != 7u) {
                    CW86_TRY(cw86_write_operand(c, &m, width, result));
                }
            }
        } else {
            CW86_TRY(cw86_fetch(c, width, &immediate));
            result = cw86_alu(c, operation, cw86_get_register(c, CW_EAX, width), immediate, width);
            if (operation != 7u) {
                cw86_set_register(c, CW_EAX, width, result);
            }
        }
    } else if (opcode >= 0x40u && opcode <= 0x4Fu) {
        const unsigned index = opcode & 7u;
        result = cw86_incdec(c, cw86_get_register(c, index, size), opcode < 0x48u ? 1 : -1, size);
        cw86_set_register(c, index, size, result);
    } else if (opcode >= 0x50u && opcode <= 0x57u) {
        if (size == 2u) {
            return CODEWRITE_UNSUPPORTED;
        }
        CW86_TRY(cw86_push(c, c->reg[opcode - 0x50u], 4u));
    } else if (opcode >= 0x58u && opcode <= 0x5Fu) {
        if (size == 2u) {
            return CODEWRITE_UNSUPPORTED;
        }
        CW86_TRY(cw86_pop(c, 4u, &left));
        c->reg[opcode - 0x58u] = left;
    } else if (opcode == 0x68u || opcode == 0x6Au) {
        if (size == 2u) {
            return CODEWRITE_UNSUPPORTED;
        }
        CW86_TRY(cw86_fetch(c, opcode == 0x68u ? 4u : 1u, &immediate));
        CW86_TRY(cw86_push(c, opcode == 0x68u ? immediate : cw86_sext(immediate, 1u), 4u));
    } else if (opcode == 0x69u || opcode == 0x6Bu || opcode == 0x1AFu) {
        /* imul r, r/m, imm (69, 6B) and imul r, r/m (0F AF encoded as 0x1AF by the caller) */
        int64_t product;
        CW86_TRY(cw86_modrm(c, &m));
        CW86_TRY(cw86_read_operand(c, &m, size, &left));
        if (opcode == 0x1AFu) {
            right = cw86_get_register(c, m.field, size);
        } else {
            CW86_TRY(cw86_fetch(c, opcode == 0x69u ? size : 1u, &right));
            if (opcode == 0x6Bu) {
                right = cw86_sext(right, 1u);
            }
        }
        product = (int64_t)(int32_t)cw86_sext(left, size) * (int64_t)(int32_t)cw86_sext(right, size);
        result = (uint32_t)product & cw86_mask(size);
        cw86_set_arith_flags(
            c, cw86_szp(result, size) |
                   (product != (int64_t)(int32_t)cw86_sext(result, size) ? (CW86_CF | CW86_OF) : 0u));
        cw86_set_register(c, m.field, size, result);
    } else if (opcode >= 0x70u && opcode <= 0x7Fu) {
        CW86_TRY(cw86_fetch(c, 1u, &immediate));
        if (cw86_condition(c, opcode & 0xFu)) {
            CW86_JUMP(c->eip + c->length + cw86_sext(immediate, 1u));
        }
    } else if (opcode == 0x80u || opcode == 0x81u || opcode == 0x83u) {
        const unsigned width = opcode == 0x80u ? 1u : size;
        CW86_TRY(cw86_modrm(c, &m));
        CW86_TRY(cw86_read_operand(c, &m, width, &left));
        CW86_TRY(cw86_fetch(c, opcode == 0x81u ? width : 1u, &immediate));
        if (opcode == 0x83u) {
            immediate = cw86_sext(immediate, 1u);
        }
        result = cw86_alu(c, m.field, left, immediate, width);
        if (m.field != 7u) {
            CW86_TRY(cw86_write_operand(c, &m, width, result));
        }
    } else if (opcode == 0x84u || opcode == 0x85u) {
        const unsigned width = opcode == 0x84u ? 1u : size;
        CW86_TRY(cw86_modrm(c, &m));
        CW86_TRY(cw86_read_operand(c, &m, width, &left));
        (void)cw86_alu(c, 4u, left, cw86_get_register(c, m.field, width), width);
    } else if (opcode == 0x86u || opcode == 0x87u) { /* xchg */
        const unsigned width = opcode == 0x86u ? 1u : size;
        CW86_TRY(cw86_modrm(c, &m));
        CW86_TRY(cw86_read_operand(c, &m, width, &left));
        right = cw86_get_register(c, m.field, width);
        CW86_TRY(cw86_write_operand(c, &m, width, right));
        cw86_set_register(c, m.field, width, left);
    } else if (opcode >= 0x88u && opcode <= 0x8Bu) { /* mov */
        const unsigned width = (opcode & 1u) ? size : 1u;
        CW86_TRY(cw86_modrm(c, &m));
        if (opcode < 0x8Au) {
            CW86_TRY(cw86_write_operand(c, &m, width, cw86_get_register(c, m.field, width)));
        } else {
            CW86_TRY(cw86_read_operand(c, &m, width, &left));
            cw86_set_register(c, m.field, width, left);
        }
    } else if (opcode == 0x8Du) { /* lea */
        CW86_TRY(cw86_modrm(c, &m));
        if (m.is_register) {
            return CODEWRITE_UNSUPPORTED;
        }
        cw86_set_register(c, m.field, size, m.address);
    } else if (opcode == 0x90u) {
        /* nop */
    } else if (opcode == 0x98u) { /* cwde */
        if (size == 2u) {
            return CODEWRITE_UNSUPPORTED;
        }
        c->reg[CW_EAX] = cw86_sext(c->reg[CW_EAX] & 0xFFFFu, 2u);
    } else if (opcode == 0x99u) { /* cdq */
        if (size == 2u) {
            return CODEWRITE_UNSUPPORTED;
        }
        c->reg[CW_EDX] = (c->reg[CW_EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0u;
    } else if (opcode >= 0xA0u && opcode <= 0xA3u) { /* mov al/eax, moffs and back */
        const unsigned width = (opcode & 1u) ? size : 1u;
        CW86_TRY(cw86_fetch(c, 4u, &immediate));
        if (opcode < 0xA2u) {
            if (codewrite_read(immediate, width, &left)) {
                return CW86_STATUS_READ;
            }
            cw86_set_register(c, CW_EAX, width, left);
        } else if (codewrite_write(immediate, cw86_get_register(c, CW_EAX, width), width)) {
            return CW86_STATUS_WRITE;
        }
    } else if ((opcode >= 0xA4u && opcode <= 0xA7u) || (opcode >= 0xAAu && opcode <= 0xAFu) ||
               (opcode >= 0x6Cu && opcode <= 0x6Fu)) {
        /* movs cmps stos lods scas, one iteration per loop turn so a fault keeps the completed
         * ones. F3 repeats (repe for cmps/scas), F2 repeats (repne for cmps/scas). */
        const unsigned width = (opcode & 1u) ? size : 1u;
        const int compares = opcode == 0xA6u || opcode == 0xA7u || opcode == 0xAEu || opcode == 0xAFu;
        if (size == 2u) {
            return CODEWRITE_UNSUPPORTED;
        }
        for (;;) {
            if (c->rep && c->reg[CW_ECX] == 0u) {
                break;
            }
            switch (opcode & 0xFEu) {
            case 0xA4u: /* movs */
                if (codewrite_read(c->reg[CW_ESI], width, &left)) {
                    return CW86_STATUS_READ;
                }
                if (codewrite_write(c->reg[CW_EDI], left, width)) {
                    return CW86_STATUS_WRITE;
                }
                break;
            case 0xA6u: /* cmps: [esi] - [edi] */
                if (codewrite_read(c->reg[CW_ESI], width, &left)) {
                    return CW86_STATUS_READ;
                }
                if (codewrite_read(c->reg[CW_EDI], width, &right)) {
                    return CW86_STATUS_READ;
                }
                (void)cw86_alu(c, 7u, left, right, width);
                break;
            case 0x6Cu: /* ins: the port read returns zero (MEASURED) */
                if (codewrite_write(c->reg[CW_EDI], 0u, width)) {
                    return CW86_STATUS_WRITE;
                }
                break;
            case 0x6Eu: /* outs: reads [esi], the port write has no effect */
                if (codewrite_read(c->reg[CW_ESI], width, &left)) {
                    return CW86_STATUS_READ;
                }
                break;
            case 0xAAu: /* stos */
                if (codewrite_write(c->reg[CW_EDI], cw86_get_register(c, CW_EAX, width), width)) {
                    return CW86_STATUS_WRITE;
                }
                break;
            case 0xACu: /* lods */
                if (codewrite_read(c->reg[CW_ESI], width, &left)) {
                    return CW86_STATUS_READ;
                }
                cw86_set_register(c, CW_EAX, width, left);
                break;
            default: /* scas: eax - [edi] */
                if (codewrite_read(c->reg[CW_EDI], width, &right)) {
                    return CW86_STATUS_READ;
                }
                (void)cw86_alu(c, 7u, cw86_get_register(c, CW_EAX, width), right, width);
                break;
            }
            {
                const uint32_t step = (c->eflags & CW86_DF) ? 0u - width : width;
                const unsigned family = opcode & 0xFEu;
                if (family != 0xAAu && family != 0xAEu && family != 0x6Cu) {
                    c->reg[CW_ESI] += step;
                }
                if (family != 0xACu && family != 0x6Eu) {
                    c->reg[CW_EDI] += step;
                }
            }
            if (!c->rep) {
                break;
            }
            c->reg[CW_ECX] -= 1u;
            if (compares) {
                const int equal = (c->eflags & CW86_ZF) != 0;
                if ((c->rep == 1 && !equal) || (c->rep == 2 && equal)) {
                    break;
                }
            }
        }
    } else if (opcode >= 0x91u && opcode <= 0x97u) { /* xchg eax, r */
        const unsigned index = opcode & 7u;
        left = cw86_get_register(c, CW_EAX, size);
        cw86_set_register(c, CW_EAX, size, cw86_get_register(c, index, size));
        cw86_set_register(c, index, size, left);
    } else if (opcode == 0xA8u || opcode == 0xA9u) {
        const unsigned width = opcode == 0xA8u ? 1u : size;
        CW86_TRY(cw86_fetch(c, width, &immediate));
        (void)cw86_alu(c, 4u, cw86_get_register(c, CW_EAX, width), immediate, width);
    } else if (opcode >= 0xB0u && opcode <= 0xB7u) {
        CW86_TRY(cw86_fetch(c, 1u, &immediate));
        cw86_set_register(c, opcode - 0xB0u, 1u, immediate);
    } else if (opcode >= 0xB8u && opcode <= 0xBFu) {
        CW86_TRY(cw86_fetch(c, size, &immediate));
        cw86_set_register(c, opcode - 0xB8u, size, immediate);
    } else if (opcode == 0xC0u || opcode == 0xC1u || (opcode >= 0xD0u && opcode <= 0xD3u)) {
        const unsigned width = (opcode & 1u) ? size : 1u;
        uint32_t count;
        CW86_TRY(cw86_modrm(c, &m));
        CW86_TRY(cw86_read_operand(c, &m, width, &left));
        if (opcode <= 0xC1u) {
            CW86_TRY(cw86_fetch(c, 1u, &count));
        } else {
            count = opcode <= 0xD1u ? 1u : (c->reg[CW_ECX] & 0xFFu);
        }
        CW86_TRY(cw86_shift(c, m.field, left, count, width, &result));
        CW86_TRY(cw86_write_operand(c, &m, width, result));
    } else if (opcode == 0xC2u || opcode == 0xC3u) {
        uint32_t extra = 0u;
        if (opcode == 0xC2u) {
            CW86_TRY(cw86_fetch(c, 2u, &extra));
        }
        CW86_TRY(cw86_pop(c, 4u, &left));
        c->reg[CW_ESP] += extra;
        CW86_JUMP(left);
    } else if (opcode == 0xC6u || opcode == 0xC7u) {
        const unsigned width = opcode == 0xC6u ? 1u : size;
        CW86_TRY(cw86_modrm(c, &m));
        if (m.field != 0u) {
            return CODEWRITE_UNSUPPORTED;
        }
        CW86_TRY(cw86_fetch(c, width, &immediate));
        CW86_TRY(cw86_write_operand(c, &m, width, immediate));
    } else if (opcode == 0xC9u) { /* leave */
        const uint32_t base = c->reg[CW_EBP];
        if (codewrite_read(base, 4u, &left)) {
            return CW86_STATUS_READ;
        }
        c->reg[CW_ESP] = base + 4u;
        c->reg[CW_EBP] = left;
    } else if (opcode == 0x9Cu) { /* pushfd (POST-HOC: seen after a store rewrote 3C97BE) */
        if (size == 2u) {
            return CODEWRITE_UNSUPPORTED;
        }
        CW86_TRY(cw86_push(c, (c->eflags | 2u) & ~0x00030000u, 4u));
    } else if (opcode == 0x9Du) { /* popfd; a popped trap flag would single-step: refused */
        if (size == 2u) {
            return CODEWRITE_UNSUPPORTED;
        }
        CW86_TRY(cw86_pop(c, 4u, &left));
        if (left & 0x100u) {
            c->reg[CW_ESP] -= 4u;
            return CODEWRITE_UNSUPPORTED;
        }
        c->eflags = (c->eflags & ~0x00254FD5u) | (left & 0x00254FD5u) | 2u;
    } else if (opcode == 0x60u) { /* pushad */
        const uint32_t top = c->reg[CW_ESP];
        if (size == 2u) {
            return CODEWRITE_UNSUPPORTED;
        }
        for (unsigned i = 0u; i < 8u; i++) {
            if (codewrite_write(top - 4u * (i + 1u), i == CW_ESP ? top : c->reg[i], 4u)) {
                return CW86_STATUS_WRITE;
            }
        }
        c->reg[CW_ESP] = top - 32u;
    } else if (opcode == 0x61u) { /* popad: registers load one by one, esp is skipped */
        const uint32_t top = c->reg[CW_ESP];
        if (size == 2u) {
            return CODEWRITE_UNSUPPORTED;
        }
        for (unsigned i = 0u; i < 8u; i++) {
            const unsigned index = 7u - i;
            if (codewrite_read(top + 4u * i, 4u, &left)) {
                return CW86_STATUS_READ;
            }
            if (index != CW_ESP) {
                c->reg[index] = left;
            }
        }
        c->reg[CW_ESP] = top + 32u;
    } else if (opcode == 0xC8u) { /* enter size, 0 (nesting level above zero is refused) */
        uint32_t level;
        if (size == 2u) {
            return CODEWRITE_UNSUPPORTED;
        }
        CW86_TRY(cw86_fetch(c, 2u, &immediate));
        CW86_TRY(cw86_fetch(c, 1u, &level));
        if ((level & 0x1Fu) != 0u) {
            return CODEWRITE_UNSUPPORTED;
        }
        CW86_TRY(cw86_push(c, c->reg[CW_EBP], 4u));
        c->reg[CW_EBP] = c->reg[CW_ESP];
        c->reg[CW_ESP] -= immediate;
    } else if (opcode == 0xFCu) {
        c->eflags &= ~CW86_DF;
    } else if (opcode == 0xFDu) {
        c->eflags |= CW86_DF;
    } else if (opcode == 0xF8u) {
        c->eflags &= ~CW86_CF;
    } else if (opcode == 0xF9u) {
        c->eflags |= CW86_CF;
    } else if (opcode == 0xF5u) {
        c->eflags ^= CW86_CF;
    } else if (opcode == 0x9Eu) { /* sahf */
        c->eflags = (c->eflags & ~0xD5u) | (((c->reg[CW_EAX] >> 8) & 0xD5u)) | 2u;
    } else if (opcode == 0x9Fu) { /* lahf */
        cw86_set_register(c, 4u, 1u, (c->eflags & 0xD5u) | 2u);
    } else if (opcode == 0xE4u || opcode == 0xE5u || opcode == 0xECu || opcode == 0xEDu) {
        /* in: Unicorn has no port hook, a read returns zero (MEASURED with a one-instruction
         * probe, docs/t1508-code-write-isolation.md) */
        if (opcode == 0xE4u || opcode == 0xE5u) {
            CW86_TRY(cw86_fetch(c, 1u, &immediate));
        }
        cw86_set_register(c, CW_EAX, (opcode & 1u) ? size : 1u, 0u);
    } else if (opcode == 0xE6u || opcode == 0xE7u || opcode == 0xEEu || opcode == 0xEFu) {
        if (opcode == 0xE6u || opcode == 0xE7u) {
            CW86_TRY(cw86_fetch(c, 1u, &immediate));
        }
    } else if (opcode == 0xFAu) {
        c->eflags &= ~0x200u;
    } else if (opcode == 0xFBu) {
        c->eflags |= 0x200u;
    } else if (opcode >= 0xE0u && opcode <= 0xE3u) { /* loopne loope loop jecxz */
        CW86_TRY(cw86_fetch(c, 1u, &immediate));
        if (size == 2u) {
            return CODEWRITE_UNSUPPORTED;
        }
        int take;
        if (opcode == 0xE3u) {
            take = c->reg[CW_ECX] == 0u;
        } else {
            c->reg[CW_ECX] -= 1u;
            take = c->reg[CW_ECX] != 0u;
            if (opcode == 0xE0u) {
                take = take && !(c->eflags & CW86_ZF);
            } else if (opcode == 0xE1u) {
                take = take && (c->eflags & CW86_ZF);
            }
        }
        if (take) {
            CW86_JUMP(c->eip + c->length + cw86_sext(immediate, 1u));
        }
    } else if (opcode == 0x8Cu) { /* mov r/m16, sreg: every selector reads zero in the oracle */
        CW86_TRY(cw86_modrm(c, &m));
        CW86_TRY(cw86_write_operand(c, &m, m.is_register ? size : 2u, 0u));
    } else if (opcode == 0xF4u) {
        *stop_eip = c->eip + 1u;
        return CW86_HALT;
    } else if (opcode == 0xCCu) {
        *stop_eip = c->eip + 1u;
        return CODEWRITE_BREAKPOINT;
    } else if (opcode == 0xD4u) { /* aam imm8 */
        CW86_TRY(cw86_fetch(c, 1u, &immediate));
        if (immediate == 0u) {
            return CW86_STATUS_DIVIDE;
        }
        left = c->reg[CW_EAX] & 0xFFu;
        cw86_set_register(c, 4u, 1u, left / immediate);
        cw86_set_register(c, 0u, 1u, left % immediate);
        cw86_set_arith_flags(c, cw86_szp(c->reg[CW_EAX], 1u));
    } else if (opcode == 0xE8u) {
        CW86_TRY(cw86_fetch(c, 4u, &immediate));
        CW86_TRY(cw86_push(c, c->eip + c->length, 4u));
        CW86_JUMP(c->eip + c->length + immediate);
    } else if (opcode == 0xE9u || opcode == 0xEBu) {
        CW86_TRY(cw86_fetch(c, opcode == 0xE9u ? 4u : 1u, &immediate));
        CW86_JUMP(c->eip + c->length + (opcode == 0xEBu ? cw86_sext(immediate, 1u) : immediate));
    } else if (opcode == 0xF1u) {
        return CODEWRITE_ICEBP;
    } else if (opcode == 0xF6u || opcode == 0xF7u) {
        const unsigned width = opcode == 0xF6u ? 1u : size;
        CW86_TRY(cw86_modrm(c, &m));
        CW86_TRY(cw86_read_operand(c, &m, width, &left));
        if (m.field <= 1u) { /* test r/m, imm */
            CW86_TRY(cw86_fetch(c, width, &immediate));
            (void)cw86_alu(c, 4u, left, immediate, width);
        } else if (m.field == 2u) {
            CW86_TRY(cw86_write_operand(c, &m, width, ~left));
        } else if (m.field == 3u) {
            result = cw86_neg(c, left, width);
            CW86_TRY(cw86_write_operand(c, &m, width, result));
        } else {
            CW86_TRY(cw86_group3_wide(c, m.field, left, width));
        }
    } else if (opcode == 0xFEu || opcode == 0xFFu) {
        const unsigned width = opcode == 0xFEu ? 1u : size;
        CW86_TRY(cw86_modrm(c, &m));
        if (m.field <= 1u) {
            CW86_TRY(cw86_read_operand(c, &m, width, &left));
            result = cw86_incdec(c, left, m.field == 0u ? 1 : -1, width);
            CW86_TRY(cw86_write_operand(c, &m, width, result));
        } else if (opcode == 0xFFu && size == 4u && m.field == 6u) {
            CW86_TRY(cw86_read_operand(c, &m, 4u, &left));
            CW86_TRY(cw86_push(c, left, 4u));
        } else if (opcode == 0xFFu && size == 4u && (m.field == 2u || m.field == 4u)) {
            CW86_TRY(cw86_read_operand(c, &m, 4u, &left));
            if (m.field == 2u) {
                CW86_TRY(cw86_push(c, c->eip + c->length, 4u));
            }
            CW86_JUMP(left);
        } else {
            return CODEWRITE_UNSUPPORTED;
        }
    } else if (opcode == 0x1B6u || opcode == 0x1B7u || opcode == 0x1BEu || opcode == 0x1BFu) {
        const unsigned source = (opcode & 1u) ? 2u : 1u;
        CW86_TRY(cw86_modrm(c, &m));
        CW86_TRY(cw86_read_operand(c, &m, source, &left));
        cw86_set_register(c, m.field, size, (opcode & 8u) ? cw86_sext(left, source) : left);
    } else if (opcode == 0x1A4u || opcode == 0x1A5u || opcode == 0x1ACu || opcode == 0x1ADu) {
        /* shld / shrd r/m32, r32, imm8 | cl */
        const int right_shift = opcode >= 0x1ACu;
        uint32_t count;
        if (size != 4u) {
            return CODEWRITE_UNSUPPORTED;
        }
        CW86_TRY(cw86_modrm(c, &m));
        CW86_TRY(cw86_read_operand(c, &m, 4u, &left));
        right = c->reg[m.field];
        if (opcode & 1u) {
            count = c->reg[CW_ECX];
        } else {
            CW86_TRY(cw86_fetch(c, 1u, &count));
        }
        count &= 0x1Fu;
        if (count != 0u) {
            uint64_t last, wide_result;
            uint32_t flags;
            if (!right_shift) {
                last = ((uint64_t)left << (count - 1u)) | ((uint64_t)right >> (32u - count + 1u));
                wide_result = ((uint64_t)left << count) | ((uint64_t)right >> (32u - count));
                flags = (last >> 31) & 1u ? CW86_CF : 0u;
            } else {
                last = ((uint64_t)left >> (count - 1u)) | ((uint64_t)right << (32u - count + 1u));
                wide_result = ((uint64_t)left >> count) | ((uint64_t)right << (32u - count));
                flags = (last & 1u) ? CW86_CF : 0u;
            }
            result = (uint32_t)wide_result;
            if ((((uint32_t)last ^ result) >> 31) & 1u) {
                flags |= CW86_OF;
            }
            flags |= cw86_szp(result, 4u);
            cw86_set_arith_flags(c, flags);
            CW86_TRY(cw86_write_operand(c, &m, 4u, result));
        }
    } else if (opcode >= 0x180u && opcode <= 0x18Fu) { /* jcc rel32 */
        CW86_TRY(cw86_fetch(c, size == 2u ? 2u : 4u, &immediate));
        if (size == 2u) {
            return CODEWRITE_UNSUPPORTED;
        }
        if (cw86_condition(c, opcode & 0xFu)) {
            CW86_JUMP(c->eip + c->length + immediate);
        }
    } else if (opcode >= 0x190u && opcode <= 0x19Fu) { /* setcc r/m8 */
        CW86_TRY(cw86_modrm(c, &m));
        CW86_TRY(cw86_write_operand(c, &m, 1u, cw86_condition(c, opcode & 0xFu) ? 1u : 0u));
    } else if (opcode >= 0x140u && opcode <= 0x14Fu) { /* cmovcc r, r/m */
        CW86_TRY(cw86_modrm(c, &m));
        CW86_TRY(cw86_read_operand(c, &m, size, &left));
        if (cw86_condition(c, opcode & 0xFu)) {
            cw86_set_register(c, m.field, size, left);
        }
    } else {
        return CODEWRITE_UNSUPPORTED;
    }
    return 0;
}

/* Executes the live bytes from `entry` until control reaches `sentinel` (or a stop). `reg` holds
 * eax..edi, `*eflags` the flags; both are updated in place. */
static inline codewrite_outcome cw86_run(uint32_t reg[8], uint32_t *eflags, uint32_t entry,
                                         uint32_t sentinel, uint32_t max_steps)
{
    cw86 c = {reg, *eflags, entry, 0u, 4u, 0, 0, 0, 0u};
    uint32_t steps = 0u;
    codewrite_outcome outcome = {CODEWRITE_DONE, entry, 0u};
    while (c.eip != sentinel) {
        uint32_t opcode, stop_eip;
        int status;
        if (steps >= max_steps) {
            outcome.kind = CW86_BUDGET;
            outcome.eip = c.eip;
            outcome.steps = steps;
            goto finished;
        }
        c.length = 0u;
        c.operand16 = 0;
        c.rep = 0;
        c.taken = 0;
        c.opsize = 4u;
        status = cw86_fetch(&c, 1u, &opcode);
        /* POST-HOC: F2, and the flat es/cs/ss/ds overrides (no-ops, base 0), added after random
         * bytes the roots store over their own tail decoded to them. */
        while (status == 0 && (opcode == 0x66u || opcode == 0xF3u || opcode == 0xF2u ||
                               opcode == 0x26u || opcode == 0x2Eu || opcode == 0x36u ||
                               opcode == 0x3Eu)) {
            if (opcode == 0x66u) {
                c.operand16 = 1;
                c.opsize = 2u;
            } else if (opcode == 0xF3u) {
                c.rep = 1;
            } else if (opcode == 0xF2u) {
                c.rep = 2;
            }
            status = cw86_fetch(&c, 1u, &opcode);
        }
        if (status == 0 && (opcode == 0x64u || opcode == 0x65u || opcode == 0x67u ||
                            opcode == 0xF0u)) {
            status = CODEWRITE_UNSUPPORTED;
        }
        if (status == 0 && opcode == 0x0Fu) {
            status = cw86_fetch(&c, 1u, &opcode);
            opcode += 0x100u;
        }
        stop_eip = c.eip;
        if (status == 0) {
            status = cw86_execute(&c, opcode, &stop_eip);
        }
        if (status != 0) {
            outcome.kind = (unsigned)status;
            outcome.eip = stop_eip;
            outcome.steps = status == CW86_HALT ? steps + 1u : steps;
            goto finished;
        }
        steps++;
        c.eip = c.taken ? c.target : c.eip + c.length;
    }
    outcome.eip = c.eip;
    outcome.steps = steps;
finished:
    *eflags = c.eflags;
    return outcome;
}

#endif /* TSFP_GAME_CODEWRITE_X86_H */
