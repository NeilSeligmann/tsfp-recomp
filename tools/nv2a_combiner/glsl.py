# SPDX-License-Identifier: GPL-3.0-or-later
"""Translate a combiner configuration to GLSL 4.50.

TWO FORMS FROM ONE BODY. `combine_function` emits `vec4 nv2a_combine(Nv2aFragment f, out
bool killed)`, the whole texture environment, general stages and final combiner. `fragment_shader`
wraps it in a complete fragment shader (varyings, a constants block, samplers, optional
alpha test). The validation harness wraps the SAME function in a compute shader whose
`nv2a_sample` is a procedural texture, so the numbers it checks are the numbers a
fragment shader would produce. Only the sampling lines differ between the two, and
`docs/combiner-translator.md` says so.

SEMANTICS SNAPSHOT. Both halves of a stage read the registers as they were at the start of
the stage, so every result is computed into a temporary and the writes come last. Writing
the register straight away would let the alpha half read a colour the same stage just
overwrote.

OUTSIDE THE COMBINER. Alpha test, blending, fog blending and the colour mask are fixed
function after the final combiner. This module emits the alpha test only when asked, and
never blends. The fog REGISTER (colour with the fog factor as alpha) is an input to the
combiner and is supplied.
"""

from __future__ import annotations

from tools.nv2a_combiner import config as cfg
from tools.nv2a_combiner import models

# Varying names follow the vertex program's outputs (oD0, oD1, oFog, oT0 to oT3).
VARYINGS = (
    ("vec4", "vs_oD0"),
    ("vec4", "vs_oD1"),
    ("float", "vs_oFog"),
    ("vec4", "vs_oT0"),
    ("vec4", "vs_oT1"),
    ("vec4", "vs_oT2"),
    ("vec4", "vs_oT3"),
)

PRELUDE = """\
struct Nv2aFragment {
    vec4 d0;
    vec4 d1;
    vec4 texcoord[4];
    vec4 fog;
    vec4 factor0[8];
    vec4 factor1[8];
    vec4 final_c0;
    vec4 final_c1;
};
"""

REGISTER_NAMES = {
    cfg.REG_V0: "v0",
    cfg.REG_V1: "v1",
    8: "t0",
    9: "t1",
    10: "t2",
    11: "t3",
    cfg.REG_SPARE0: "r0",
    cfg.REG_SPARE1: "r1",
}

_SAMPLER_TYPES = {
    cfg.TEX_2D_PROJECTIVE: "sampler2D",
    cfg.TEX_3D_PROJECTIVE: "sampler3D",
    cfg.TEX_CUBE_MAP: "samplerCube",
    cfg.TEX_DOT_ST: "sampler2D",
    cfg.TEX_DEPENDENT_AR: "sampler2D",
    cfg.TEX_DEPENDENT_GB: "sampler2D",
}

#: T1490: the texture modes the replay and the live renderer draw. A mode reads its coordinate
#: varying (COORDINATE), and all but the dot product (which only forms dot(oTn.xyz, mapped input))
#: sample a texture.
REPLAY_TEXTURE_MODES = (
    cfg.TEX_NONE,
    cfg.TEX_2D_PROJECTIVE,
    cfg.TEX_CUBE_MAP,
    cfg.TEX_DOT_ST,
    cfg.TEX_DOT_PRODUCT,
)
COORDINATE_TEXTURE_MODES = (
    cfg.TEX_2D_PROJECTIVE,
    cfg.TEX_CUBE_MAP,
    cfg.TEX_DOT_ST,
    cfg.TEX_DOT_PRODUCT,
)

ALPHA_FUNCTIONS = {
    "never": "false",
    "less": "<",
    "equal": "==",
    "lequal": "<=",
    "greater": ">",
    "notequal": "!=",
    "gequal": ">=",
    "always": "true",
}


def literal(value: float) -> str:
    """A GLSL float literal that always carries a decimal point."""
    text = repr(float(value))
    return text if "." in text or "e" in text else text + ".0"


def mapped(expression: str, mapping: int) -> str:
    """Apply one of the eight input mappings to a GLSL expression."""
    if mapping == cfg.MAP_UNSIGNED_IDENTITY:
        return f"max({expression}, 0.0)"
    if mapping == cfg.MAP_UNSIGNED_INVERT:
        return f"(1.0 - clamp({expression}, 0.0, 1.0))"
    if mapping == cfg.MAP_EXPAND_NORMAL:
        return f"(2.0 * max({expression}, 0.0) - 1.0)"
    if mapping == cfg.MAP_EXPAND_NEGATE:
        return f"(1.0 - 2.0 * max({expression}, 0.0))"
    if mapping == cfg.MAP_HALF_BIAS_NORMAL:
        return f"(max({expression}, 0.0) - 0.5)"
    if mapping == cfg.MAP_HALF_BIAS_NEGATE:
        return f"(0.5 - max({expression}, 0.0))"
    if mapping == cfg.MAP_SIGNED_IDENTITY:
        return expression
    if mapping == cfg.MAP_SIGNED_NEGATE:
        return f"(-{expression})"
    raise ValueError(f"input mapping {mapping}")


class _Writer:
    def __init__(self) -> None:
        self.lines: list[str] = []
        self.depth = 1

    def add(self, text: str) -> None:
        self.lines.append("    " * self.depth + text)

    def open(self, comment: str) -> None:
        self.add(f"// {comment}")
        self.add("{")
        self.depth += 1

    def close(self) -> None:
        self.depth -= 1
        self.add("}")


def _register(register: int, stage: cfg.Stage | None) -> str:
    """The vec4 expression for a source register inside a general stage or the final."""
    if register == cfg.REG_ZERO:
        return "vec4(0.0)"
    if register == cfg.REG_C0:
        return "final_c0" if stage is None else f"f.factor0[{stage.factor0}]"
    if register == cfg.REG_C1:
        return "final_c1" if stage is None else f"f.factor1[{stage.factor1}]"
    if register == cfg.REG_FOG:
        return "fog"
    if register == cfg.REG_SUM:
        return "sum_register"
    if register == cfg.REG_PROD:
        return "prod_register"
    return REGISTER_NAMES[register]


def _operand(operand: cfg.Input, stage: cfg.Stage | None, *, rgb: bool) -> str:
    base = _register(operand.register, stage)
    if rgb:
        channels = ".aaa" if operand.alpha else ".rgb"
    else:
        channels = ".a" if operand.alpha else ".b"
    return mapped(base + channels, operand.mapping)


def _mux_condition(config: cfg.Config) -> str:
    if config.mux_msb:
        return "r0.a >= 0.5"
    return "(uint(max(r0.a, 0.0) * 255.0 + 0.5) & 1u) == 1u"


def _half(
    out: _Writer,
    config: cfg.Config,
    stage: cfg.Stage,
    half: cfg.Half,
    *,
    rgb: bool,
    tag: str,
    model: models.Model | None = None,
) -> list[str]:
    """Emit the temporaries of one half. Returns the register write statements."""
    kind = "vec3" if rgb else "float"
    names = [f"{tag}_{letter}" for letter in "abcd"]
    for name, operand in zip(names, half.inputs, strict=True):
        out.add(f"{kind} {name} = {_operand(operand, stage, rgb=rgb)};")
    o = half.output
    ab_expr = f"vec3(dot({names[0]}, {names[1]}))" if o.ab_dot else f"{names[0]} * {names[1]}"
    cd_expr = f"vec3(dot({names[2]}, {names[3]}))" if o.cd_dot else f"{names[2]} * {names[3]}"
    out.add(f"{kind} {tag}_ab = {ab_expr};")
    out.add(f"{kind} {tag}_cd = {cd_expr};")
    if o.mux:
        out.add(f"{kind} {tag}_sum = ({_mux_condition(config)}) ? {tag}_cd : {tag}_ab;")
    else:
        out.add(f"{kind} {tag}_sum = {tag}_ab + {tag}_cd;")
    bias = "0.5" if o.bias else "0.0"
    writes = []
    for suffix, destination, blue in (
        ("ab", o.ab_dst, o.blue_to_alpha_ab),
        ("cd", o.cd_dst, o.blue_to_alpha_cd),
        ("sum", o.sum_dst, False),
    ):
        if destination == cfg.REG_ZERO:
            continue
        value = f"{tag}_{suffix}_out"
        shaped = f"clamp(({tag}_{suffix} - {bias}) * {literal(o.scale)}, -1.0, 1.0)"
        if model is not None and model.rounds_writes():
            shaped = f"nv2a_q({shaped})"
        out.add(f"{kind} {value} = {shaped};")
        register = REGISTER_NAMES[destination]
        if rgb:
            writes.append(f"{register}.rgb = {value};")
            if blue:
                writes.append(f"{register}.a = {value}.b;")
        else:
            writes.append(f"{register}.a = {value};")
    return writes


def _texture_stage(
    out: _Writer,
    config: cfg.Config,
    stage: int,
    *,
    external: bool,
    model: models.Model | None = None,
) -> None:
    mode = config.texture_modes[stage]
    coord = f"f.texcoord[{stage}]"
    name = f"t{stage}"

    def sample(vector: str) -> str:
        if external:
            return f"nv2a_sample({stage}, {vector})"
        if mode == cfg.TEX_3D_PROJECTIVE or mode == cfg.TEX_CUBE_MAP:
            return f"texture(nv2a_tex{stage}, {vector})"
        return f"texture(nv2a_tex{stage}, ({vector}).xy)"

    previous = cfg.input_stage(config.other_input, stage)
    mapper = (config.dot_mapping >> (4 * (stage - 1))) & 0xF if stage else 0
    source = f"t{previous}.rgb"
    mapped_source = f"({source} * 255.0 - 128.0) / 127.0" if mapper == 1 else source
    if mapper == 1 and model is not None:
        mapped_source = models.DOT_MAPPINGS[model.dot][1].format(s=source)
    if mapper in (2, 3):
        # T1490, xemu psh.c dotmap_minus1_to_1_gl (sign2) and dotmap_minus1_to_1 (sign3)
        byte = f"({source} * 255.0)"
        if mapper == 2:
            mapped_source = (
                f"mix(({byte} + 0.5) / 127.5, ({byte} - 255.5) / 127.5, step(vec3(128.0), {byte}))"
            )
        else:
            mapped_source = (
                f"mix({byte} / 127.0, ({byte} - 256.0) / 127.0, step(vec3(128.0), {byte}))"
            )
    if mode == cfg.TEX_NONE:
        out.add(f"vec4 {name} = vec4(0.0, 0.0, 0.0, 1.0);")
    elif mode == cfg.TEX_2D_PROJECTIVE:
        out.add(f"vec4 {name} = {sample(f'vec3({coord}.xy / {coord}.w, 0.0)')};")
    elif mode == cfg.TEX_3D_PROJECTIVE:
        out.add(f"vec4 {name} = {sample(f'{coord}.xyz / {coord}.w')};")
    elif mode == cfg.TEX_CUBE_MAP:
        out.add(f"vec4 {name} = {sample(f'{coord}.xyz')};")
    elif mode == cfg.TEX_PASS_THROUGH:
        out.add(f"vec4 {name} = {coord};")
    elif mode == cfg.TEX_CLIP_PLANE:
        out.add(f"vec4 {name} = vec4(0.0);")
        out.add(f"if (any(lessThan({coord}, vec4(0.0)))) killed = true;")
    elif mode == cfg.TEX_DEPENDENT_GB:
        out.add(f"vec4 {name} = {sample(f'vec3(t{previous}.gb, 0.0)')};")
    elif mode == cfg.TEX_DEPENDENT_AR:
        out.add(f"vec4 {name} = {sample(f'vec3(t{previous}.ar, 0.0)')};")
    elif mode == cfg.TEX_DOT_PRODUCT:
        out.add(f"dot{stage} = dot({coord}.xyz, {mapped_source});")
        out.add(f"vec4 {name} = vec4(0.0);")
    elif mode == cfg.TEX_DOT_ST:
        out.add(f"dot{stage} = dot({coord}.xyz, {mapped_source});")
        out.add(f"vec4 {name} = {sample(f'vec3(dot{stage - 1}, dot{stage}, 0.0)')};")
    else:
        raise ValueError(f"texture mode {mode}")


def _final(out: _Writer, config: cfg.Config, model: models.Model | None = None) -> None:
    final = config.final
    out.add("vec4 final_c0 = f.final_c0;")
    out.add("vec4 final_c1 = f.final_c1;")
    first = "(1.0 - clamp(r0.rgb, 0.0, 1.0))" if final.complement_r0 else "max(r0.rgb, 0.0)"
    second = "(1.0 - clamp(v1.rgb, 0.0, 1.0))" if final.complement_v1 else "max(v1.rgb, 0.0)"
    total = f"{first} + {second}"
    if final.clamp_sum:
        total = f"clamp({total}, 0.0, 1.0)"
    out.add(f"vec4 sum_register = vec4({total}, 0.0);")
    out.add("vec4 prod_register = vec4(0.0);")
    # E and F may not read the product register, only the others.
    e = _operand(final.e, None, rgb=True)
    f_operand = _operand(final.f, None, rgb=True)
    out.add(f"prod_register.rgb = {e} * {f_operand};")
    letters = {}
    for name in "abcd":
        letters[name] = _operand(getattr(final, name), None, rgb=True)
    out.add(f"vec3 fa = {letters['a']};")
    out.add(f"vec3 fb = {letters['b']};")
    out.add(f"vec3 fc = {letters['c']};")
    out.add(f"vec3 fd = {letters['d']};")
    out.add(f"float fg = {_operand(final.g, None, rgb=False)};")
    out.add("vec3 rgb = clamp(fd + (1.0 - fa) * fc + fa * fb, 0.0, 1.0);")
    result = "vec4(rgb, clamp(fg, 0.0, 1.0))"
    if model is not None and model.rounds_output():
        result = f"nv2a_q({result})"
    out.add(f"return {result};")


def combine_function(
    config: cfg.Config, *, external: bool = False, model: models.Model | None = None
) -> str:
    """`nv2a_combine`: textures, stages and final combiner for one configuration.

    `model` (T102, T103, `models.py`) selects a candidate rounding and dot mapping and needs the
    `models.glsl_functions` prelude. None emits the shipped translation, byte for byte."""
    out = _Writer()
    out.add("killed = false;")
    out.add("vec4 v0 = f.d0;")
    out.add("vec4 v1 = f.d1;")
    out.add("vec4 fog = f.fog;")
    out.add("float dot0 = 0.0, dot1 = 0.0, dot2 = 0.0, dot3 = 0.0;")
    for stage in range(4):
        _texture_stage(out, config, stage, external=external, model=model)
    initial = "t0.a" if config.texture_modes[0] != cfg.TEX_NONE else "1.0"
    out.add(f"vec4 r0 = vec4(0.0, 0.0, 0.0, {initial});")
    out.add("vec4 r1 = vec4(0.0);")
    for index, stage in enumerate(config.stages):
        out.open(f"general combiner stage {index}")
        writes = _half(out, config, stage, stage.rgb, rgb=True, tag=f"s{index}c", model=model)
        writes += _half(out, config, stage, stage.alpha, rgb=False, tag=f"s{index}a", model=model)
        for write in writes:
            out.add(write)
        out.close()
    out.open("final combiner")
    _final(out, config, model)
    out.close()
    body = "\n".join(out.lines)
    return f"vec4 nv2a_combine(const Nv2aFragment f, out bool killed) {{\n{body}\n}}\n"


def fragment_shader(config: cfg.Config, *, alpha_test: str | None = None, digest: str = "") -> str:
    """A complete GLSL 4.50 fragment shader for `config`."""
    used = sorted(
        {
            (stage, _SAMPLER_TYPES[mode])
            for stage, mode in enumerate(config.texture_modes)
            if mode in _SAMPLER_TYPES
        }
    )
    lines = ["#version 450", f"// NV2A register combiner translation {digest}".rstrip()]
    for location, (kind, name) in enumerate(VARYINGS):
        lines.append(f"layout(location = {location}) in {kind} {name};")
    lines.append("layout(location = 0) out vec4 frag_color;")
    lines.append("layout(set = 0, binding = 0, std140) uniform Nv2aConstants {")
    lines += [
        "    vec4 factor0[8];",
        "    vec4 factor1[8];",
        "    vec4 final_c0;",
        "    vec4 final_c1;",
        "    vec4 fog_color;",
        "    vec4 alpha_ref;",
        "} nv2a_constants;",
    ]
    for stage, kind in used:
        lines.append(f"layout(set = 0, binding = {1 + stage}) uniform {kind} nv2a_tex{stage};")
    lines.append(PRELUDE)
    lines.append(combine_function(config))
    lines.append("void main() {")
    lines.append("    Nv2aFragment f;")
    lines.append("    f.d0 = vs_oD0;")
    lines.append("    f.d1 = vs_oD1;")
    lines.append("    f.texcoord[0] = vs_oT0;")
    lines.append("    f.texcoord[1] = vs_oT1;")
    lines.append("    f.texcoord[2] = vs_oT2;")
    lines.append("    f.texcoord[3] = vs_oT3;")
    lines.append("    f.fog = vec4(nv2a_constants.fog_color.rgb, clamp(vs_oFog, 0.0, 1.0));")
    lines.append("    f.factor0 = nv2a_constants.factor0;")
    lines.append("    f.factor1 = nv2a_constants.factor1;")
    lines.append("    f.final_c0 = nv2a_constants.final_c0;")
    lines.append("    f.final_c1 = nv2a_constants.final_c1;")
    lines.append("    bool killed;")
    lines.append("    vec4 colour = nv2a_combine(f, killed);")
    lines.append("    if (killed) discard;")
    if alpha_test is not None:
        if alpha_test not in ALPHA_FUNCTIONS:
            raise ValueError(f"alpha function {alpha_test}")
        operator = ALPHA_FUNCTIONS[alpha_test]
        if operator == "false":
            lines.append("    discard;")
        elif operator != "true":
            lines.append("    float alpha8 = round(colour.a * 255.0);")
            lines.append(f"    if (!(alpha8 {operator} nv2a_constants.alpha_ref.x)) discard;")
    lines.append("    frag_color = colour;")
    lines.append("}")
    return "\n".join(lines) + "\n"


def compute_shader(config: cfg.Config, model: models.Model | None = None) -> str:
    """The validation wrapper. One invocation evaluates one fragment from a storage buffer.

    Input layout, in vec4s: d0, d1, texcoord 0 to 3, fog, factor0 x8, factor1 x8, final c0,
    final c1, then 5 sampler coefficient vec4s per texture stage (base, du, dv, dw, duv).
    Output, in vec4s: the colour, then (killed, 0, 0, 0).

    `model` is a candidate of `models.py` (T102, T103). None is the shipped translation.
    """
    prelude = models.glsl_functions(model.rounding) if model is not None else ""
    texel = "clamp(texel, 0.0, 1.0)"
    if model is not None and model.texels8:
        texel = "nv2a_texel8(clamp(texel, 0.0, 1.0))"
    return (
        "#version 450\n"
        "layout(local_size_x = 64) in;\n"
        "layout(std430, set = 0, binding = 0) readonly buffer Input { vec4 data[]; } in_buffer;\n"
        "layout(std430, set = 0, binding = 1) buffer Output { vec4 data[]; } out_buffer;\n"
        "layout(push_constant) uniform Push { uint count; } push;\n"
        f"{PRELUDE}\n"
        f"{prelude}"
        "const uint STRIDE = 45u;\n"
        "uint base_index;\n"
        "vec4 nv2a_sample(int stage, vec3 coord) {\n"
        "    uint at = base_index + 25u + uint(stage) * 5u;\n"
        "    vec4 texel = in_buffer.data[at] + in_buffer.data[at + 1u] * coord.x\n"
        "        + in_buffer.data[at + 2u] * coord.y + in_buffer.data[at + 3u] * coord.z\n"
        "        + in_buffer.data[at + 4u] * (coord.x * coord.y);\n"
        f"    return {texel};\n"
        "}\n\n"
        f"{combine_function(config, external=True, model=model)}\n"
        "void main() {\n"
        "    uint index = gl_GlobalInvocationID.x;\n"
        "    if (index >= push.count) return;\n"
        "    base_index = index * STRIDE;\n"
        "    Nv2aFragment f;\n"
        "    f.d0 = in_buffer.data[base_index];\n"
        "    f.d1 = in_buffer.data[base_index + 1u];\n"
        "    for (uint i = 0u; i < 4u; ++i) f.texcoord[i] = in_buffer.data[base_index + 2u + i];\n"
        "    f.fog = in_buffer.data[base_index + 6u];\n"
        "    for (uint i = 0u; i < 8u; ++i) {\n"
        "        f.factor0[i] = in_buffer.data[base_index + 7u + i];\n"
        "        f.factor1[i] = in_buffer.data[base_index + 15u + i];\n"
        "    }\n"
        "    f.final_c0 = in_buffer.data[base_index + 23u];\n"
        "    f.final_c1 = in_buffer.data[base_index + 24u];\n"
        "    bool killed;\n"
        "    vec4 colour = nv2a_combine(f, killed);\n"
        "    out_buffer.data[index * 2u] = colour;\n"
        "    out_buffer.data[index * 2u + 1u] = vec4(killed ? 1.0 : 0.0, 0.0, 0.0, 0.0);\n"
        "}\n"
    )


def replay_fragment_shader(
    config: cfg.Config,
    *,
    reads: int,
    digest: str = "",
    alpha_test: bool = False,
    live_fog: bool = False,
) -> str:
    """The fragment shader of the pushbuffer replay (T75), `gpu_vsh_fragment` of gpu_vsh_draw.h.

    A different interface from `fragment_shader`, because the replay's vertex constants already
    own set 0 binding 0: the combiner constants block is at binding 1 and the samplers at
    2 + stage. Only the varyings the combiner reads are declared (`reads` is the register mask of
    `replay_modules.config_reads`: v0 is oD0 at location 0, v1 is oD1 at location 1, a sampled stage
    n takes oTn at location 3 + n), so a vertex program that does not write the others is not
    refused and no input is ever read undefined. Texture modes other than none and 2D projective,
    and the fog register, are refused here as the replay refuses them.
    """
    for stage, mode in enumerate(config.texture_modes):
        if mode not in REPLAY_TEXTURE_MODES:
            raise ValueError(f"texture stage {stage} mode {mode} is not modelled by the replay")
    if reads & (1 << cfg.REG_FOG) and not live_fog:
        raise ValueError("the replay does not model the fog register")
    # a stage that reads its coordinate takes the varying oTn (a dot product stage samples nothing)
    sampled = [
        stage for stage, mode in enumerate(config.texture_modes) if mode in COORDINATE_TEXTURE_MODES
    ]
    sampler_types = {
        stage: _SAMPLER_TYPES[mode]
        for stage, mode in enumerate(config.texture_modes)
        if mode in _SAMPLER_TYPES
    }
    lines = ["#version 450", f"// NV2A register combiner, pushbuffer replay form {digest}".rstrip()]
    if reads & (1 << cfg.REG_V0):
        lines.append("layout(location = 0) in vec4 vs_oD0;")
    if reads & (1 << cfg.REG_V1):
        lines.append("layout(location = 1) in vec4 vs_oD1;")
    if reads & (1 << cfg.REG_FOG):
        lines.append("layout(location = 2) in float vs_oFog;")
    for stage in sampled:
        lines.append(f"layout(location = {3 + stage}) in vec4 vs_oT{stage};")
    lines.append("layout(location = 0) out vec4 frag_color;")
    lines.append("layout(set = 0, binding = 1, std140) uniform Nv2aConstants {")
    lines += [
        "    vec4 factor0[8];",
        "    vec4 factor1[8];",
        "    vec4 final_c0;",
        "    vec4 final_c1;",
        "    vec4 fog_color;",
        "    vec4 alpha_ref;",
        "} nv2a_constants;",
    ]
    for stage, kind in sampler_types.items():
        lines.append(f"layout(set = 0, binding = {2 + stage}) uniform {kind} nv2a_tex{stage};")
    lines.append(PRELUDE)
    lines.append(combine_function(config))
    lines.append("void main() {")
    lines.append("    Nv2aFragment f;")
    lines.append("    f.d0 = " + ("vs_oD0;" if reads & (1 << cfg.REG_V0) else "vec4(0.0);"))
    lines.append("    f.d1 = " + ("vs_oD1;" if reads & (1 << cfg.REG_V1) else "vec4(0.0);"))
    for stage in range(4):
        source = f"vs_oT{stage};" if stage in sampled else "vec4(0.0);"
        lines.append(f"    f.texcoord[{stage}] = {source}")
    lines.append(
        "    f.fog = vec4(nv2a_constants.fog_color.rgb, clamp(vs_oFog, 0.0, 1.0));"
        if reads & (1 << cfg.REG_FOG)
        else "    f.fog = vec4(0.0);"
    )
    lines.append("    f.factor0 = nv2a_constants.factor0;")
    lines.append("    f.factor1 = nv2a_constants.factor1;")
    lines.append("    f.final_c0 = nv2a_constants.final_c0;")
    lines.append("    f.final_c1 = nv2a_constants.final_c1;")
    lines.append("    bool killed;")
    lines.append("    vec4 colour = nv2a_combine(f, killed);")
    lines.append("    if (killed) discard;")
    if alpha_test:
        # T860: the NV2A alpha test (xemu psh.c: int(round(alpha * 255)) against the reference,
        # discard on a failed comparison). The block's alpha_ref slot holds the reference in x and
        # the function 0..6 (NEVER to GEQUAL) in y.
        lines += [
            "    int alpha8 = int(round(clamp(colour.a, 0.0, 1.0) * 255.0));",
            "    int alpha_reference = int(nv2a_constants.alpha_ref.x);",
            "    int alpha_function = int(nv2a_constants.alpha_ref.y);",
            "    bool alpha_pass = true;",
            "    if (alpha_function == 0) alpha_pass = false;",
            "    else if (alpha_function == 1) alpha_pass = alpha8 < alpha_reference;",
            "    else if (alpha_function == 2) alpha_pass = alpha8 == alpha_reference;",
            "    else if (alpha_function == 3) alpha_pass = alpha8 <= alpha_reference;",
            "    else if (alpha_function == 4) alpha_pass = alpha8 > alpha_reference;",
            "    else if (alpha_function == 5) alpha_pass = alpha8 != alpha_reference;",
            "    else if (alpha_function == 6) alpha_pass = alpha8 >= alpha_reference;",
            "    if (!alpha_pass) discard;",
        ]
    lines.append("    frag_color = colour;")
    lines.append("}")
    return "\n".join(lines) + "\n"
