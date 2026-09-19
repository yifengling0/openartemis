#pragma once

// SDL3-on-SDL2 compatibility for VintagePomelo (Windows MinGW + HarmonyOS).
// Included by the fake <SDL3/*.h> headers on the OA_USE_SDL2 include path.

#ifndef OA_USE_SDL2
#define OA_USE_SDL2 1
#endif

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif

#include <SDL.h>
#include <SDL_audio.h>
#include <SDL_events.h>
#include <SDL_hints.h>
#include <SDL_log.h>
#include <SDL_mouse.h>
#include <SDL_timer.h>
#include <SDL_video.h>
#include <cstring>

#ifndef SDL_AUDIO_F32
#define SDL_AUDIO_F32 AUDIO_F32SYS
#endif

#ifndef SDL_HINT_MAIN_CALLBACK_RATE
#define SDL_HINT_MAIN_CALLBACK_RATE "SDL_HINT_MAIN_CALLBACK_RATE"
#endif

enum SDL_AppResult {
    SDL_APP_CONTINUE = 0,
    SDL_APP_SUCCESS = 1,
    SDL_APP_FAILURE = 2
};

#define SDL_EVENT_QUIT SDL_QUIT
#define SDL_EVENT_KEY_DOWN SDL_KEYDOWN
#define SDL_EVENT_KEY_UP SDL_KEYUP
#define SDL_EVENT_MOUSE_MOTION SDL_MOUSEMOTION
#define SDL_EVENT_MOUSE_BUTTON_DOWN SDL_MOUSEBUTTONDOWN
#define SDL_EVENT_MOUSE_BUTTON_UP SDL_MOUSEBUTTONUP
#define SDL_EVENT_MOUSE_WHEEL SDL_MOUSEWHEEL
#define SDL_EVENT_FINGER_DOWN SDL_FINGERDOWN
#define SDL_EVENT_FINGER_MOTION SDL_FINGERMOTION
#define SDL_EVENT_FINGER_UP SDL_FINGERUP
#define SDL_EVENT_WINDOW_RESIZED 0x200
#define SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED 0x201
#define SDL_EVENT_WINDOW_EXPOSED 0x202
#define SDL_EVENT_DID_ENTER_BACKGROUND SDL_APP_DIDENTERBACKGROUND
#define SDL_EVENT_DID_ENTER_FOREGROUND SDL_APP_DIDENTERFOREGROUND

#define SDL_GL_DestroyContext SDL_GL_DeleteContext

#ifndef SDL_VERSIONNUM
#define SDL_VERSIONNUM(major, minor, patch) ((major) * 1000000 + (minor) * 1000 + (patch))
#endif
#ifndef SDL_VERSIONNUM_MAJOR
#define SDL_VERSIONNUM_MAJOR(v) ((int)((v) / 1000000))
#define SDL_VERSIONNUM_MINOR(v) ((int)(((v) / 1000) % 1000))
#define SDL_VERSIONNUM_MICRO(v) ((int)((v) % 1000))
#endif

using OA_SDL2_InitFn = int (*)(Uint32);
using OA_SDL2_CreateWindowFn = SDL_Window* (*)(const char*, int, int, int, int, Uint32);
using OA_SDL2_SetWindowFullscreenFn = int (*)(SDL_Window*, Uint32);
using OA_SDL2_GLMakeCurrentFn = int (*)(SDL_Window*, SDL_GLContext);
using OA_SDL2_GetScancodeFromKeyFn = SDL_Scancode (*)(SDL_Keycode);
using OA_SDL2_GetVersionFn = void (*)(SDL_version*);

inline OA_SDL2_InitFn oa_sdl2_Init() { return SDL_Init; }
inline OA_SDL2_InitFn oa_sdl2_InitSubSystem() { return SDL_InitSubSystem; }
inline OA_SDL2_CreateWindowFn oa_sdl2_CreateWindow() { return SDL_CreateWindow; }
inline OA_SDL2_SetWindowFullscreenFn oa_sdl2_SetWindowFullscreen() { return SDL_SetWindowFullscreen; }
inline OA_SDL2_GLMakeCurrentFn oa_sdl2_GL_MakeCurrent() { return SDL_GL_MakeCurrent; }
inline OA_SDL2_GetScancodeFromKeyFn oa_sdl2_GetScancodeFromKey() { return SDL_GetScancodeFromKey; }
inline OA_SDL2_GetVersionFn oa_sdl2_GetVersion() { return SDL_GetVersion; }

inline bool OA_SDL_Init(Uint32 flags) { return oa_sdl2_Init()(flags) == 0; }
inline bool OA_SDL_InitSubSystem(Uint32 flags) { return oa_sdl2_InitSubSystem()(flags) == 0; }
inline bool OA_SDL_GL_MakeCurrent(SDL_Window* w, SDL_GLContext c) {
    return oa_sdl2_GL_MakeCurrent()(w, c) == 0;
}
inline SDL_Window* OA_SDL_CreateWindow(const char* title, int w, int h, Uint32 flags) {
    return oa_sdl2_CreateWindow()(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
}
inline bool OA_SDL_SetWindowFullscreen(SDL_Window* window, bool fullscreen) {
    return oa_sdl2_SetWindowFullscreen()(window, fullscreen ? SDL_WINDOW_FULLSCREEN : 0) == 0;
}
inline SDL_Scancode OA_SDL_GetScancodeFromKey(SDL_Keycode key, SDL_Keymod* mods) {
    if (mods) *mods = KMOD_NONE;
    return oa_sdl2_GetScancodeFromKey()(key);
}
inline int OA_SDL_GetVersionPacked() {
    SDL_version v{};
    oa_sdl2_GetVersion()(&v);
    // SDL3 packing (major*1e6+minor*1e3+patch). Do not use SDL2's SDL_VERSIONNUM
    // (X*1000+Y*100+Z); that printed as 0.5.210 for SDL 2.30.10.
    return int(v.major) * 1000000 + int(v.minor) * 1000 + int(v.patch);
}
inline Uint64 OA_SDL_GetTicks() {
#if SDL_VERSION_ATLEAST(2, 0, 18)
    return SDL_GetTicks64();
#else
    return (Uint64)::SDL_GetTicks();
#endif
}

#if !SDL_VERSION_ATLEAST(2, 0, 26)
inline void OA_SDL_GetWindowSizeInPixels(SDL_Window* w, int* pw, int* ph) {
    SDL_GetWindowSize(w, pw, ph);
}
#define SDL_GetWindowSizeInPixels OA_SDL_GetWindowSizeInPixels
#endif

#define SDL_Init OA_SDL_Init
#define SDL_InitSubSystem OA_SDL_InitSubSystem
#define SDL_GL_MakeCurrent OA_SDL_GL_MakeCurrent
#define SDL_CreateWindow OA_SDL_CreateWindow
#define SDL_SetWindowFullscreen OA_SDL_SetWindowFullscreen
#define SDL_GetScancodeFromKey OA_SDL_GetScancodeFromKey
#define SDL_GetTicks OA_SDL_GetTicks
#define SDL_GetVersion OA_SDL_GetVersionPacked
