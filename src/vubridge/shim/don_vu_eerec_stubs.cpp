// SPDX-License-Identifier: GPL-3.0-or-later
//
// EE-side symbols referenced by code this library compiles but never runs.
//
// x86/microVU.cpp is one translation unit that also contains microVU_Macro.inl, PCSX2's COP2 "macro
// mode" recompiler for the EE (VU0 instructions issued by the R5900 recompiler), and
// microVU_IR.h's register allocator has a COP2 mode that shares registers with the EE recompiler.
// This library only runs VU1 micro mode, so none of it is reachable; the definitions below exist
// only to satisfy the linker and fail loudly if something does reach them.
//
// Adapted from KillzoneRecomp, https://github.com/drakolordx7/KillzoneRecomp, commit
// 5f0700079f9ac4e14799d66d01747dbcdcec55f6, ext/kzvu/shim/KzvuEeRecStubs.cpp (GPL-3.0-or-later);
// the list of symbols is theirs, without the renames.

#include "don_vu_prefix.h"

#include "vtlb.h"

[[noreturn]] static void Unreachable(const char* what)
{
	pxFailRel(what);
	std::abort();
}
#define DONVU_UNREACHABLE() Unreachable(__FUNCTION__)

// ---- EE recompiler state -----------------------------------------------------------
u32 pc;
bool s_nBlockInterlocked;
alignas(16) GPR_reg64 g_cpuConstRegs[32];
u32 g_cpuHasConstReg, g_cpuFlushedConstReg;
_x86regs x86regs[iREGCNT_GPR], s_saveX86regs[iREGCNT_GPR];
_xmmregs xmmregs[iREGCNT_XMM], s_saveXMMregs[iREGCNT_XMM];
EEINST* g_pCurInstInfo;
u16 g_x86AllocCounter;

namespace EmuFolders
{
	std::string Logs; // microVU program dumps (mVUlogProg, off)
}

// ---- EE recompiler register allocation (iCore.cpp / iR5900.cpp) ---------------------------------------------------------
int _allocX86reg(int type, int reg, int mode) { DONVU_UNREACHABLE(); }
bool _hasX86reg(int type, int reg, int required_mode) { DONVU_UNREACHABLE(); }
void _freeX86reg(const x86Emitter::xRegister32& x86reg) { DONVU_UNREACHABLE(); }
void _freeX86reg(int x86reg) { DONVU_UNREACHABLE(); }
void _freeX86regWithoutWriteback(int x86reg) { DONVU_UNREACHABLE(); }
int _allocTempXMMreg(XMMSSEType type) { DONVU_UNREACHABLE(); }
int _allocGPRtoXMMreg(int gprreg, int mode) { DONVU_UNREACHABLE(); }
void _reallocateXMMreg(int xmmreg, int newtype, int newreg, int newmode, bool writeback) { DONVU_UNREACHABLE(); }
int _checkXMMreg(int type, int reg, int mode) { DONVU_UNREACHABLE(); }
void _freeXMMreg(int xmmreg) { DONVU_UNREACHABLE(); }
void _freeXMMregWithoutWriteback(int xmmreg) { DONVU_UNREACHABLE(); }
int _allocVFtoXMMreg(int vfreg, int mode) { DONVU_UNREACHABLE(); }
int _allocIfUsedGPRtoX86(int gprreg, int mode) { DONVU_UNREACHABLE(); }
int _allocIfUsedVItoX86(int vireg, int mode) { DONVU_UNREACHABLE(); }
int _allocIfUsedGPRtoXMM(int gprreg, int mode) { DONVU_UNREACHABLE(); }
bool TrySwapDelaySlot(u32 rs, u32 rt, u32 rd, bool allow_loadstore) { DONVU_UNREACHABLE(); }
void iFlushCall(int flushtype) { DONVU_UNREACHABLE(); }
void recCall(void (*func)()) { DONVU_UNREACHABLE(); }
u32 scaleblockcycles_clear() { DONVU_UNREACHABLE(); }
void R5900::Dynarec::recDoBranchImm(u32 branchTo, u32* jmpSkip, bool isLikely, bool swappedDelaySlot) { DONVU_UNREACHABLE(); }
void _eeMoveGPRtoR(const x86Emitter::xRegister32& to, int fromgpr, bool allow_preload) { DONVU_UNREACHABLE(); }
void _eeMoveGPRtoM(uptr to, int fromgpr) { DONVU_UNREACHABLE(); }
void _eeFlushAllDirty() { DONVU_UNREACHABLE(); }
void _deleteEEreg128(int reg) { DONVU_UNREACHABLE(); }

// ---- EE memory access (vtlb.cpp) ----------------------------------------------------------------------------------------
int vtlb_DynGenReadQuad(u32 bits, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc) { DONVU_UNREACHABLE(); }
int vtlb_DynGenReadQuad_Const(u32 bits, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc) { DONVU_UNREACHABLE(); }
void vtlb_DynGenWrite(u32 sz, bool xmm, int addr_reg, int value_reg) { DONVU_UNREACHABLE(); }
void vtlb_DynGenWrite_Const(u32 bits, bool xmm, u32 addr_const, int value_reg) { DONVU_UNREACHABLE(); }

// ---- EE-side VU0/VU1 control (VU0.cpp, VU0micro.cpp, VU1micro.cpp, VUmicro.cpp) ---------------------------------------
void BaseVUmicroCPU::ExecuteBlockJIT(BaseVUmicroCPU* cpu, bool interlocked) { DONVU_UNREACHABLE(); }
void vu0ResetRegs() { DONVU_UNREACHABLE(); }
void _vu0WaitMicro() { DONVU_UNREACHABLE(); }
void _vu0FinishMicro() { DONVU_UNREACHABLE(); }
void vu1Finish(bool add_cycles) { DONVU_UNREACHABLE(); }
void vu1ResetRegs() { DONVU_UNREACHABLE(); }
void vu1ExecMicro(u32 addr) { DONVU_UNREACHABLE(); }
void VCALLMS() { DONVU_UNREACHABLE(); }
void VCALLMSR() { DONVU_UNREACHABLE(); }
