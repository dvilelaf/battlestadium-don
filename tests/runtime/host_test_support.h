// SPDX-License-Identifier: GPL-3.0-or-later
// Shared by the tests that run a real PS2Runtime without generated code (host_loop_test, host_pad_source_test).
// Include in exactly one translation unit of an executable.
//
//  * the generated-code function table that the runtime library expects the program to define;
//  * with raylib in the build (DON_RT_HAS_RAYLIB, Linux): replacements for every raylib function the runtime
//    library references. The test executable links with --allow-multiple-definition, so these win over the real
//    ones and record each call instead of opening a window: a test can then assert that a host-owned runtime
//    called none of them, and a positive control shows the recording works. They never touch a display.
#pragma once

#include "ps2_runtime.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x00000000u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x01000000u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount =
    (g_ps2RecompiledFunctionTableEnd - g_ps2RecompiledFunctionTableBase) >> 2;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[g_ps2RecompiledFunctionTableSlotCount] = {};

namespace rt_test
{
    struct RaylibLog
    {
        std::mutex mutex;
        std::vector<std::string> calls;
        void note(const char *name)
        {
            std::lock_guard<std::mutex> lock(mutex);
            calls.emplace_back(name);
        }
        std::vector<std::string> take()
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::vector<std::string> out;
            out.swap(calls);
            return out;
        }
    };

    inline RaylibLog &raylibLog()
    {
        static RaylibLog log;
        return log;
    }
}

#if defined(DON_RT_HAS_RAYLIB) && defined(__linux__)
#define DON_RT_RAYLIB_INTERPOSED 1
#include "raylib.h"

// Values returned to the runtime by the replacements: no gamepad, no key down, no device ready, unless a
// test sets them.
namespace rt_test
{
    inline bool &audioReady()
    {
        static bool value = false;
        return value;
    }
    inline bool &windowReady()
    {
        static bool value = false;
        return value;
    }
}

#define RL_NOTE(name) rt_test::raylibLog().note(#name)

void SetConfigFlags(unsigned int) { RL_NOTE(SetConfigFlags); }
void InitWindow(int, int, const char *) { RL_NOTE(InitWindow); rt_test::windowReady() = true; }
void CloseWindow(void) { RL_NOTE(CloseWindow); rt_test::windowReady() = false; }
bool IsWindowReady(void) { RL_NOTE(IsWindowReady); return rt_test::windowReady(); }
bool WindowShouldClose(void) { RL_NOTE(WindowShouldClose); return false; }
int GetScreenWidth(void) { RL_NOTE(GetScreenWidth); return 640; }
int GetScreenHeight(void) { RL_NOTE(GetScreenHeight); return 448; }
void SetTargetFPS(int) { RL_NOTE(SetTargetFPS); }
void BeginDrawing(void) { RL_NOTE(BeginDrawing); }
void EndDrawing(void) { RL_NOTE(EndDrawing); }
void ClearBackground(Color) { RL_NOTE(ClearBackground); }
void DrawTexturePro(Texture2D, Rectangle, Rectangle, Vector2, float, Color) { RL_NOTE(DrawTexturePro); }
Image GenImageColor(int, int, Color) { RL_NOTE(GenImageColor); return Image{}; }
void UnloadImage(Image) { RL_NOTE(UnloadImage); }
Texture2D LoadTextureFromImage(Image) { RL_NOTE(LoadTextureFromImage); return Texture2D{}; }
void UpdateTexture(Texture2D, const void *) { RL_NOTE(UpdateTexture); }
void UnloadTexture(Texture2D) { RL_NOTE(UnloadTexture); }
void InitAudioDevice(void) { RL_NOTE(InitAudioDevice); rt_test::audioReady() = true; }
void CloseAudioDevice(void) { RL_NOTE(CloseAudioDevice); rt_test::audioReady() = false; }
bool IsAudioDeviceReady(void) { RL_NOTE(IsAudioDeviceReady); return rt_test::audioReady(); }
Wave LoadWaveFromMemory(const char *, const unsigned char *, int) { RL_NOTE(LoadWaveFromMemory); return Wave{}; }
Sound LoadSoundFromWave(Wave) { RL_NOTE(LoadSoundFromWave); return Sound{}; }
void UnloadWave(Wave) { RL_NOTE(UnloadWave); }
void UnloadSound(Sound) { RL_NOTE(UnloadSound); }
void SetSoundPitch(Sound, float) { RL_NOTE(SetSoundPitch); }
void SetSoundVolume(Sound, float) { RL_NOTE(SetSoundVolume); }
void PlaySound(Sound) { RL_NOTE(PlaySound); }
void StopSound(Sound) { RL_NOTE(StopSound); }
bool IsSoundPlaying(Sound) { RL_NOTE(IsSoundPlaying); return false; }
bool IsKeyDown(int) { RL_NOTE(IsKeyDown); return false; }
bool IsGamepadAvailable(int) { RL_NOTE(IsGamepadAvailable); return false; }
bool IsGamepadButtonDown(int, int) { RL_NOTE(IsGamepadButtonDown); return false; }
float GetGamepadAxisMovement(int, int) { RL_NOTE(GetGamepadAxisMovement); return 0.0f; }
#endif
