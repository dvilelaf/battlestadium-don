// SPDX-License-Identifier: GPL-3.0-or-later
// T-518: the processor check that runs first in main (src/host/main.cpp), before any code the
// x86-64-v3 build may have compiled with an AVX2, FMA or BMI2 instruction.
#pragma once

// 0 when this processor can run the build (AVX2, FMA and BMI2; the build is -march=x86-64-v3) or
// when the architecture has nothing to check; 5 after showing knowledge 22 section 7.2's message
// (a message box and the same line on stderr) when it cannot. Never returns another code.
//
// Implemented in cpu_check.cpp, which is compiled for baseline x86-64 (the per-file -march=x86-64
// in src/host/CMakeLists.txt): the check itself must not use the instructions it is checking for.
int don_cpu_check();
