#ifndef PS2_SYSCALLS_H
#define PS2_SYSCALLS_H
#include "ps2_runtime.h"

namespace ps2_syscalls
{
inline void InitAlarm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { don_rt->calls->host_fn[DON_HF_SYS_InitAlarm](rdram, ctx, runtime); }
inline void InitTLB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { don_rt->calls->host_fn[DON_HF_SYS_InitTLB](rdram, ctx, runtime); }
inline void InitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { don_rt->calls->host_fn[DON_HF_SYS_InitThread](rdram, ctx, runtime); }
inline void StartThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { don_rt->calls->host_fn[DON_HF_SYS_StartThread](rdram, ctx, runtime); }
inline void iRotateThreadReadyQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { don_rt->calls->host_fn[DON_HF_SYS_iRotateThreadReadyQueue](rdram, ctx, runtime); }
inline void iWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { don_rt->calls->host_fn[DON_HF_SYS_iWakeupThread](rdram, ctx, runtime); }
inline void sceSifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { don_rt->calls->host_fn[DON_HF_SYS_sceSifCallRpc](rdram, ctx, runtime); }
inline void sceSifLoadElf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { don_rt->calls->host_fn[DON_HF_SYS_sceSifLoadElf](rdram, ctx, runtime); }
inline void sceSifLoadElfPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { don_rt->calls->host_fn[DON_HF_SYS_sceSifLoadElfPart](rdram, ctx, runtime); }
inline void sceSifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { don_rt->calls->host_fn[DON_HF_SYS_sceSifLoadModule](rdram, ctx, runtime); }
inline void sceSifLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { don_rt->calls->host_fn[DON_HF_SYS_sceSifLoadModuleBuffer](rdram, ctx, runtime); }
inline void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { don_rt->calls->host_fn[DON_HF_SYS_sceSifSendCmd](rdram, ctx, runtime); }
}

#endif // PS2_SYSCALLS_H
