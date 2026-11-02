#!/usr/bin/env python3
"""Derive the module build's ps2_runtime_macros.h from the runtime's own header (T-610).

The generated game code is compiled freestanding (no libc/libm headers), so the runtime's macros have their C/C++
library calls replaced by clang builtins and SSE intrinsics. Nothing is copied into git: the result is written
into the module build folder.

usage: gen_shim_macros.py <runtime ps2_runtime_macros.h> <output file>
"""
import re
import sys
from pathlib import Path

HEADER = """// Shim of ps2_runtime_macros.h (T-601): the runtime's macros with the C/C++ library calls replaced by clang
// builtins and intrinsics, so generated code compiles freestanding. Derived from
// third_party/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h (same license, GPL-3.0); the differences are
// marked "T-601". Math that needs libm (round) is kept only for macros no generated file uses.
"""
INCLUDE_BLOCK = re.compile(r"#include <cstdint>\n.*?#endif\n\n", re.S)
PREAMBLE = """#define PS2_RAM_SIZE DON_RAM_SIZE
#define PS2_RAM_MASK DON_RAM_MASK

// T-601: sqrtf without libm (the correctly-rounded SSE instruction is what the runtime build emits too).
static inline float ps2_sqrtf(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
"""
SUBSTITUTIONS = [
    (r"static_cast<uint32_t>\(std::countl_zero\(x\)\)", "static_cast<uint32_t>(x ? __builtin_clz(x) : 32)"),
    (r"std::memcpy\(", "__builtin_memcpy("),
    (r"\bsqrtf\(", "ps2_sqrtf("),
    (r"(?<![\w:])(fabsf|roundf|ceilf|floorf|nearbyintf)\(", r"__builtin_\1("),
    (r"(?<![\w:])isnan\(", "__builtin_isnan("),
]


def derive(text: str) -> str:
    text, n = INCLUDE_BLOCK.subn("", text, count=1)
    if n != 1:
        raise SystemExit("gen_shim_macros: include block not found in the runtime header")
    marker = '#include "ps2_runtime.h"\n'
    if marker not in text:
        raise SystemExit("gen_shim_macros: ps2_runtime.h include not found")
    # The module has no runtime umbrella header: the ABI declarations are what the macros need.
    text = text.replace(marker, '#include "don_guest_decls.h"\n' + PREAMBLE, 1)
    for pattern, repl in SUBSTITUTIONS:
        text = re.sub(pattern, repl, text)
    return HEADER + text


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(derive(src.read_text()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
