// Shim of ps2_log.h (T-601): generated code only reaches it under PS2_FUNCTION_LOG_TRACKER, which the module
// never defines. Provided so the include resolves inside work/t601.
#ifndef PS2_LOG_H
#define PS2_LOG_H
#define PS_LOG_ENTRY(name) ((void)0)
#define PS2_IF_AGRESSIVE_LOGS(code) ((void)0)
#endif
