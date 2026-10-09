"""Original F6/F7 signed multiplies must honor implicit partial-register writes."""
import ctypes
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))
from tools.recomp.disasm import Disassembler
from tools.recomp.lifter import Lifter, _make_condition
from tools.recomp.translator import FunctionTranslator
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX,
    UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI,
    UC_X86_REG_EDI, UC_X86_REG_EFLAGS)

REGS = (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
        UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI)
NAMES = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')
FORMS = ((1, bytes.fromhex('f6ea')), (2, bytes.fromhex('66f7ea')),
         (4, bytes.fromhex('f7ea')), (1, bytes.fromhex('f6e8')),
         (1, bytes.fromhex('f6ec')),
         (1, bytes.fromhex('f62d00200100')),
         (2, bytes.fromhex('66f72d00200100')),
         (4, bytes.fromhex('f72d00200100')))


def test_native_original_bytes_and_flags():
    compiler = shutil.which('gcc') or shutil.which('cc')
    if not compiler:
        return
    prelude = '''#include <stdint.h>
#define MEM8(a) (*(uint8_t *)&_memory)
#define MEM16(a) (*(uint16_t *)&_memory)
#define MEM32(a) (_memory)
#define LO8(r) ((uint8_t)((r) & 0xFF))
#define HI8(r) ((uint8_t)(((r) >> 8) & 0xFF))
#define LO16(r) ((uint16_t)((r) & 0xFFFF))
#define SET_LO16(r,v) ((r)=((r)&0xFFFF0000u)|((uint32_t)(uint16_t)(v)))
'''
    definitions = []
    for form, (width, raw) in enumerate(FORMS):
        instruction = Disassembler().disassemble_function(raw, 0x10000, 0x10000+len(raw))[0]
        for track in (False, True):
            lifter = Lifter()
            lifter.needs_cf = track
            fragment = '\n'.join(lifter.lift_instruction(instruction))
            branches = []
            for cc, raw_branch in (('jo','7000'),('jno','7100'),('jc','7200'),('jnc','7300')):
                insns=Disassembler().disassemble_function(raw+bytes.fromhex(raw_branch),0x10000,0x10000+len(raw)+2)
                assert FunctionTranslator._function_needs_cf(insns),cc
                branches.append(_make_condition(insns[-1].mnemonic,'imul',instruction.operands)[0])
            flag_value='_cf' + ''.join(f' | ((uint32_t)!!({expr}) << {i+1})' for i,expr in enumerate(branches))
            definitions.append(f'void f{form}_{int(track)}(uint32_t *r, uint32_t *flags) {{\n'
                + '\n'.join(f'uint32_t {name}=r[{i}];' for i,name in enumerate(NAMES))
                + '\nuint32_t _memory=edx; int _cf=0;\n' + fragment
                + '\n' + '\n'.join(f'r[{i}]={name};' for i,name in enumerate(NAMES))
                + '\n*flags=' + flag_value + ';\n}')
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory); source=path/'multiply.c'
        source.write_text(prelude+'\n'.join(definitions))
        libraries=[]
        ctypes.CDLL("libubsan.so.1", mode=ctypes.RTLD_GLOBAL)
        for optimization in ('-O0','-O3'):
            binary=path/(optimization+'.so')
            result=subprocess.run([compiler,'-std=gnu11',optimization,'-shared','-fPIC',
                '-fsanitize=undefined','-fno-sanitize-recover=undefined',str(source),
                '-o',str(binary)],capture_output=True,text=True)
            assert result.returncode==0,result.stderr
            libraries.append(ctypes.CDLL(str(binary)))
        uc=Uc(UC_ARCH_X86,UC_MODE_32);uc.mem_map(0x10000,0x3000)
        total=0
        for form,(width,raw) in enumerate(FORMS):
            uc.mem_write(0x10000,raw)
            mask=(1<<(width*8))-1
            values=(0,1,2,6,mask>>1,(mask>>1)-1,(mask>>1)+1,mask-1,mask)
            for a in values:
                for b in values:
                    registers=[0xA5A57700,0x12345678,0x5A5A3300,0x87654321,
                               0xE80080,0xE81000,0xDEADBEEF,0x2468ACE0]
                    registers[0]=(registers[0]&~mask)|a
                    registers[2]=(registers[2]&~mask)|b
                    for register,value in zip(REGS,registers):uc.reg_write(register,value)
                    uc.mem_write(0x12000,registers[2].to_bytes(4,"little"))
                    uc.reg_write(UC_X86_REG_EFLAGS,0x202)
                    uc.emu_start(0x10000,0x10000+len(raw))
                    expected=[uc.reg_read(register) for register in REGS]
                    flags=uc.reg_read(UC_X86_REG_EFLAGS)
                    assert bool(flags&1)==bool(flags&0x800)
                    for library in libraries:
                        for track in (False,True):
                            actual=(ctypes.c_uint32*8)(*registers);carry=ctypes.c_uint32()
                            getattr(library,f'f{form}_{int(track)}')(actual,ctypes.byref(carry))
                            assert list(actual)==expected,(raw.hex(),a,b,list(actual),expected)
                            if track:
                                cf=flags&1
                                expected_flags=cf | (cf<<1) | ((not cf)<<2) | (cf<<3) | ((not cf)<<4)
                                assert carry.value==expected_flags,(raw.hex(),a,b,carry.value,flags)
                    total+=1
        assert total==648


if __name__=='__main__':
    test_native_original_bytes_and_flags()
    print('imul_width: ALL PASS')
