"""Generate the shared helper header of src/game/game_fp_step2b.c (T1634) from game_fp_list002.c.

Adds divss, cvttss2si and the full comiss/ucomiss condition set to the T1631 fixed-destination
helper block. Output is a text fragment (default stdout / --out)."""

import argparse
from pathlib import Path

CONDS = {
    "z": ("ccz", "(a == b) || (a != a) || (b != b)"),
    "nz": ("ccnz", "(a < b) || (a > b)"),
    "a": ("cca", "a > b"),
    "ae": ("ccae", "a >= b"),
    "b": ("ccb", "!(a >= b)"),
    "be": ("ccbe", "!(a > b)"),
    "p": ("ccp", "(a != a) || (b != b)"),
    "np": ("ccnp", "(a == a) && (b == b)"),
}

DIV_ASM = """/* divss dest, source: dest = dest / source. */
GAME_FP_HELPER float fp_divss(float dest, float source)
{
    __asm__ volatile("divss %1, %0" : "+x"(dest) : "xm"(source));
    return dest;
}

/* cvttss2si r32, xmm/m32: truncate, out of range or NaN gives 0x80000000 (integer indefinite). */
GAME_FP_HELPER int32_t fp_cvttss2si(float source)
{
    int32_t result;
    __asm__ volatile("cvttss2si %1, %0" : "=r"(result) : "xm"(source));
    return result;
}
"""

DIV_PORT = """GAME_FP_HELPER float fp_divss(float dest, float source)
{
    return fp_x86_ss_result(dest, source, dest / source);
}

GAME_FP_HELPER int32_t fp_cvttss2si(float source)
{
    if (!(source > -2147483648.0f && source < 2147483648.0f)) return (int32_t)0x80000000u;
    return (int32_t)source;
}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="src/game/game_fp_list002.c")
    parser.add_argument("--out", default="-")
    args = parser.parse_args()
    src = Path(args.base).read_text().split("\n")
    start = next(i for i, line in enumerate(src) if line.startswith("#include <string.h>"))
    end = next(i for i, line in enumerate(src) if "END GAME_FP_SCALAR_HELPERS" in line)
    out = "\n".join(src[start : end + 1]) + "\n"
    out = out.replace(
        "__thread GameFpXmm g_xmm4 __attribute__((weak, aligned(8)));",
        "__thread GameFpXmm g_xmm4 __attribute__((weak, aligned(8)));\n"
        "__thread GameFpXmm g_xmm5 __attribute__((weak, aligned(8)));\n"
        "__thread GameFpXmm g_xmm6 __attribute__((weak, aligned(8)));\n"
        "__thread GameFpXmm g_xmm7 __attribute__((weak, aligned(8)));",
    )
    asm = [
        "",
        "/* T1634: full comiss/ucomiss condition set. The suffix is the x86 condition code read",
        " * from the flags the instruction sets (unordered gives ZF=PF=CF=1). */",
        "#if GAME_FP_FIXED_DEST",
    ]
    port = ["#else"]
    for ins in ("comiss", "ucomiss"):
        for key, (cc, expr) in CONDS.items():
            name = f"fp_{ins}_{key}"
            asm += [
                f"GAME_FP_HELPER int {name}(float a, float b)",
                "{",
                "    int taken;",
                f'    __asm__ volatile("{ins} %2, %1" : "=@{cc}"(taken) : "x"(a), "xm"(b));',
                "    return taken;",
                "}",
                "",
            ]
            port += [
                f"GAME_FP_HELPER int {name}(float a, float b)",
                "{",
                f"    return {expr};",
                "}",
                "",
            ]
    out += "\n".join(asm) + DIV_ASM + "\n".join(port) + DIV_PORT + "#endif\n"
    rest = src[end + 1 :]
    stop = next(i for i, line in enumerate(rest) if line.startswith("/* 0x00120220"))
    out += "\n".join(rest[:stop]) + "\n"
    if args.out == "-":
        print(out)
    else:
        Path(args.out).write_text(out)


if __name__ == "__main__":
    main()
