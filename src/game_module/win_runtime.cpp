// SPDX-License-Identifier: GPL-3.0-or-later
// The two symbols that the Microsoft C runtime would give a Windows module, supplied here so that don_game.dll links
// with /NODEFAULTLIB and imports nothing (M7, T-730). Only built for the Windows target.
#ifdef _WIN32

// _fltused: the MSVC-target compiler references it from any object that uses floating point; the CRT defines it only so
// that the linker has something to resolve. The value is never read.
extern "C" int _fltused = 0;

// __chkstk: clang calls it in the prologue of a function whose frame is 4 KiB or larger (x64 stack probe).
// Contract, from Microsoft Learn "x64 prolog and epilog" (learn.microsoft.com/en-us/cpp/build/prolog-and-epilog,
// section "Prolog code"): the prolog loads the allocation size into RAX, calls __chkstk, then does `sub RSP, RAX`; the
// helper probes the to-be-allocated stack range so the stack is extended properly, and modifies no register other
// than R10, R11 and the condition codes: RAX comes back unchanged and RSP is not touched.
// The probe below is the same algorithm as compiler-rt's lib/builtins/x86_64/chkstk.S (LLVM, Apache-2.0 WITH
// LLVM-exception): it only reads (`test`), and it saves and restores RCX and RAX, which the contract allows.
__asm__(
    ".text\n"
    ".def __chkstk; .scl 2; .type 32; .endef\n"
    ".globl __chkstk\n"
    ".p2align 4\n"
    "__chkstk:\n"
    "    pushq %rcx\n"
    "    pushq %rax\n"
    "    cmpq  $0x1000, %rax\n"
    "    leaq  24(%rsp), %rcx\n"  // the caller's RSP: above the two pushes and the return address
    "    jb    2f\n"
    "1:\n"
    "    subq  $0x1000, %rcx\n"
    "    testq %rcx, (%rcx)\n"
    "    subq  $0x1000, %rax\n"
    "    cmpq  $0x1000, %rax\n"
    "    ja    1b\n"
    "2:\n"
    "    subq  %rax, %rcx\n"
    "    testq %rcx, (%rcx)\n"
    "    popq  %rax\n"
    "    popq  %rcx\n"
    "    ret\n");

#endif
