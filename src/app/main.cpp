// openartemis — SDL3 host (M5b integration): boots GameRuntime on a real PFS
// project, ticks with SDL input, and renders the Lua-driven layer events
// through oa::render::Compositor (textures resolved via magic paths + .png).
#ifndef OA_USE_SDL2
#define SDL_MAIN_USE_CALLBACKS
#endif
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_video.h>
#ifdef OA_USE_SDL2
#include <SDL.h>
#endif
#include <algorithm>
#include <filesystem>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <cstdint>
#include <cstring>
#include <sys/stat.h>
#include <vector>

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_save.h"
#include "core/media/image.h"
#include "core/render/renderer.h"
#include "core/render/layer.h"
#include "core/util/path_utf8.h"
#include "core/version.h"

// ---------------------------------------------------------------------------
// openartemis / openartemis_test split.
//
// main.cpp is the plain user host (`openartemis`: CLI options, input, render,
// status line). The same TU is compiled a second time as main_test.cpp with
// OA_TEST_BUILD=1, which only adds small TEST-ONLY hooks (extra AppState
// fields in app_host.h, per-frame probe blocks below). The autodrive
// machinery itself lives in a REAL second TU — src/app/app_test_drive.cpp —
// that openartemis_test links (main.cpp keeps zero lines of it; the entry
// declarations are in app_host.h).
//
// Everything below marked `#if OA_TEST_BUILD` is TEST-ONLY: the user binary
// contains none of it (no probes, no synthetic input, no extra env vars).
// The two binaries behave identically except for those test facilities.
// See docs/TESTING.md and the header of app_test_drive.cpp.
// ---------------------------------------------------------------------------
#ifndef OA_TEST_BUILD
#define OA_TEST_BUILD 0
#endif

#include "app_host.h"   // Options + AppState (shared with app_test_drive.cpp, test build only)
#include "platform/Platform.h" // oa::plat host platform layer

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#ifdef __OHOS__
#include <hilog/log.h>
#endif

namespace {
/// SDL3 keycode/scancode -> Artemis Windows virtual-key code. The FPM's
/// keyconfig table (csv.advkey.def) is keyed by Windows VK codes (13/32/37-40
/// arrows, F1-F12 = 112-123, letters/digits, Ctrl 17 …); SDL delivers its own
/// keycode space, so real keyboards need this translation (total-acceptance:
/// F10 = VK121 opens the config screen).
/// Returns 0 for keys that carry no engine key (Escape is the host quit key).
int sdl_key_to_vk(SDL_Keycode key) {
    if (key == SDLK_ESCAPE) return 0;
    SDL_Keymod mods = KMOD_NONE;
    const SDL_Scancode sc = SDL_GetScancodeFromKey(key, &mods);
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_0 && sc <= SDL_SCANCODE_9)
        return '0' + (sc - SDL_SCANCODE_0);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return 112 + (sc - SDL_SCANCODE_F1);
    switch (key) {
        case SDLK_RETURN: return 13;
        case SDLK_SPACE: return 32;
        case SDLK_BACKSPACE: return 8;
        case SDLK_TAB: return 9;
        case SDLK_LEFT: return 37;
        case SDLK_UP: return 38;
        case SDLK_RIGHT: return 39;
        case SDLK_DOWN: return 40;
        case SDLK_HOME: return 36;
        case SDLK_END: return 35;
        case SDLK_PAGEUP: return 33;
        case SDLK_PAGEDOWN: return 34;
        case SDLK_INSERT: return 45;
        case SDLK_DELETE: return 46;
        default: break;
    }
    if (sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL) return 17;
    if (sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT) return 16;
    if (sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT) return 18;
    return 0;
}

// ---------------------------------------------------------------------------
// CLI options (hand-rolled parsing; no argument library)
// ---------------------------------------------------------------------------

void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [options] [project.pfs]\n"
        "\n"
        "openartemis: SDL3 desktop host / headless driver for an Artemis FPM\n"
        "project (GameRuntime + Lua layer events rendered via oa::render).\n"
        "\n"
        "positional:\n"
        "  project.pfs          PFS archive to boot (its 'windows' project); or the\n"
        "                       path of an extracted project tree (has system.ini).\n"
        "                       Omit it to fall back to $OA_PFS; with neither, the run\n"
        "                       fails with this usage (no machine-specific default).\n"
        "\n"
        "options:\n"
        "  --frames N           Run N frames, then print the end-of-run self-check\n"
        "                       statistics and exit 0 (CI / smoke runs). Without this\n"
        "                       flag the demo keeps running until the window is closed,\n"
        "                       Esc is pressed, or the game itself requests exit\n"
        "                       (rt->exit_requested()).\n"
        "  --fps N              Windowed tick/present cap (default 60). GLES vsync is\n"
        "                       always on; this only changes the 1/N-second Lua+frame\n"
        "                       grid. Ignored in --headless (virtual 16 ms ticks).\n"
        "  --headless           No window, no rendering: drive GameRuntime with virtual\n"
        "                       16 ms ticks as fast as the CPU allows. Combine with\n"
        "                       --frames for a finite run, or run it indefinitely and\n"
        "                       terminate externally. Prints a status line every ~5 s.\n"
        "  --platform NAME      Boot the project as NAME (system.ini section + game 'os'\n"
        "                       variable). Default: this build's OS family (windows on\n"
        "                       desktop, android in the APK, wasm in the browser build).\n"
        "                       Multi-platform games (e.g. thyt) keep per-OS path tables\n"
        "                       under list_<NAME>.tbl.\n"
        "  --renderer NAME      Render backend line: 'sdl' (default — the\n"
        "                       historical SDL3 SDL_Render path) or 'gles' (native GLES\n"
        "                       renderer, same scene semantics). Windowed runs only;\n"
        "                       --headless never creates a renderer.\n"
        "  --dump PATH          At end-of-run (windowed --frames), also write the final\n"
        "                       rendered frame as a PPM to PATH.\n"
        "  -h, --help           Show this help and exit.\n"
        "\n"
        "env:\n"
        "  OA_PFS=path          default project when no positional argument is given\n"
        "  OA_DUMP_FRAME=1      same as --dump <tempdir>/oa_frame.ppm (windowed\n"
        "                       end-of-run; temp dir = $TMPDIR/%%TEMP%%, no fixed path)\n"
        "  OA_RENDERER=sdl|gles  same as --renderer NAME (windowed runs)\n"
        "\n"
        "notes:\n"
        "  The demo never injects synthetic clicks/keys: with no input it simply sits\n"
        "  in the game's own logic (mouse-only state unchanged).\n",
        argv0 ? argv0 : "openartemis");
}

// Returns 0 on success, 1 when --help was shown, 2 on a usage error (message
// and usage already printed to the appropriate stream).
int parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto usage_error = [=](const std::string& msg) {
            std::fprintf(stderr, "openartemis: %s\n", msg.c_str());
            print_usage(argv[0]);
            return 2;
        };
        if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return 1;
        }
        if (a == "--") continue; // treat everything after as positional
        // --flag=value and --flag value forms
        const auto eq = a.find('=');
        const std::string name = eq == std::string::npos ? a : a.substr(0, eq);
        auto value_of = [&](std::string& out) -> bool {
            if (eq != std::string::npos) {
                out = a.substr(eq + 1);
                return true;
            }
            if (i + 1 < argc) {
                out = argv[++i];
                return true;
            }
            return false;
        };
        if (name == "--frames") {
            std::string v;
            if (!value_of(v)) return usage_error("--frames needs a positive frame count");
            char* end = nullptr;
            errno = 0;
            const long long n = std::strtoll(v.c_str(), &end, 10);
            if (errno || !end || *end || n <= 0)
                return usage_error("--frames: expected a positive integer, got '" + v + "'");
            o.frames_target = (uint64_t)n;
        } else if (name == "--fps") {
            std::string v;
            if (!value_of(v)) return usage_error("--fps needs a positive frame rate");
            char* end = nullptr;
            errno = 0;
            const long n = std::strtol(v.c_str(), &end, 10);
            if (errno || !end || *end || n <= 0)
                return usage_error("--fps: expected a positive integer, got '" + v + "'");
            o.fps = (int)n;
        } else if (name == "--platform") {
            std::string v;
            if (!value_of(v) || v.empty()) return usage_error("--platform needs a name");
            o.platform = v;
        } else if (name == "--renderer") {
            std::string v;
            if (!value_of(v) || v.empty()) return usage_error("--renderer needs a name");
            if (v != "sdl" && v != "gles")
                return usage_error("--renderer: expected 'sdl' or 'gles', got '" + v + "'");
            o.renderer = v;
        } else if (name == "--dump") {
            std::string v;
            if (!value_of(v) || v.empty()) return usage_error("--dump needs a file path");
            o.dump = v;
            o.dump_on_end = true;
        } else if (a == "--headless") {
            o.headless = true;
        } else if (!a.empty() && a[0] == '-' && a != "-") {
            return usage_error("unknown option '" + a + "'");
        } else {
            o.pfs = a; // positional project.pfs (first non-option wins)
        }
    }
    // OA_RENDERER env override (test-build harness convention like
    // OA_WIN_W/OA_DUMP_FRAME; also honored by the plain user binary).
    if (o.renderer == "sdl") {
        if (const char* e = std::getenv("OA_RENDERER"); e && *e) {
            const std::string v = e;
            if (v == "sdl" || v == "gles") o.renderer = v;
            else
                std::fprintf(stderr, "openartemis: ignoring OA_RENDERER='%s' "
                                     "(expected sdl or gles)\n", e);
        }
    }
    if (!o.dump_on_end) {
        if (const char* e = std::getenv("OA_DUMP_FRAME"); e && *e) {
            // Cross-platform dump default: the OS temp dir ($TMPDIR / %TEMP%),
            // not a fixed /tmp path (no /tmp on Windows). OA_SAVE_ROOT-style
            // env override is not needed: --dump PATH or OA_DUMP_FRAME only
            // choose the LOCATION CLASS; tests pass explicit --dump paths.
            o.dump_on_end = true;
            std::error_code tec;
            const std::filesystem::path td = std::filesystem::temp_directory_path(tec);
            o.dump = oa::util::path_to_utf8(td / "oa_frame.ppm");
            if (td.empty()) o.dump = "oa_frame.ppm"; // no temp dir at all -> cwd
        }
    }
    return 0;
}

} // namespace


// 余量封顶（vsync 之外的剩余帧时间用 SDL_Delay 让出）。桌面线在 vsync 关
// 或面板非 60 整数倍（90/144Hz）时靠它锁 60Hz；OHOS 此前完全靠
// SwapInterval(1) 阻塞（一个 vblank = 一个 tick）——120Hz 面板上 Lua
// onEnterFrame/tween/音频 pacing 全部跑 120 次/秒，是 60Hz 设计的 2 倍
// （docs/PERFORMANCE_OPTIMIZATION_PLAN.md §3.1）。OHOS 同样启用：SwapWindow
// 先阻塞到 vblank，再 delay 掉余量 → 逻辑帧率恒 60Hz，与桌面线逐帧等价。
static int frame_pace_hz(const Options& o) {
    return o.fps > 0 ? o.fps : 60;
}

static void pace_windowed_frame(int hz, Uint64 frame_start) {
    if (hz <= 0) return;
    static Uint64 epoch_ms = 0;
    static uint64_t pace_i = 0;
    static int paced_hz = 0;
    if (paced_hz != hz) {
        epoch_ms = 0;
        pace_i = 0;
        paced_hz = hz;
    }
    if (epoch_ms == 0) epoch_ms = frame_start;
    ++pace_i;
    const Uint64 due = epoch_ms + (pace_i * 1000ull) / (unsigned)hz;
    const Uint64 now = SDL_GetTicks();
    if (now < due) {
        SDL_Delay((Uint32)(due - now));
    } else if (now > due + 250) {
        // Hitch (sync PNG decode, etc.): resync instead of a catch-up burst.
        epoch_ms = now - (pace_i * 1000ull) / (unsigned)hz;
    }
}

// Shared status heartbeat: frames / current wait / layer count
// ("what step is the game parked on").
static void print_status(AppState* state) {
    std::printf("[app] status frames=%llu wait=%s layers=%zu\n",
        (unsigned long long)state->frames, wait_desc(state->rt->current_wait()).c_str(),
        state->rt->scene().size());
    std::fflush(stdout);
};

// ---------------------------------------------------------------------------
// Frame profiler (P1 "可测量优先", docs/PERFORMANCE_OPTIMIZATION_PLAN.md).
//
// OA_PROFILE=1 makes every status beat print one fixed-format line of DELTAS
// since the previous beat, so a real-game run yields the numbers the plan's
// acceptance criteria ask for (draw calls / uploads / decode time) without a
// debugger attached:
//
//   [prof] dt=5.0s frames=300 fps=60.0 | draws/f=12.4 batches/f=8.1 binds/f=3.2
//          | tex=f  uploads/f=9.0 upMB/s=14.2 | decode/f=1.2 decode_ms/f=18.4
//          readMB/s=22.1 miss=0 | layers=75
//
// Counters come from the render backend (backend.h RenderStats) and the
// renderer's asset face (RenderEngine::AssetStats).
// ---------------------------------------------------------------------------
struct ProfileSnapshot {
    uint64_t frames = 0;
    uint64_t draws = 0;
    uint64_t batches = 0;
    uint64_t binds = 0;
    uint64_t textures = 0;
    uint64_t uploads = 0;
    uint64_t upload_bytes = 0;
    uint64_t presents = 0;
    uint64_t decodes = 0;
    uint64_t decode_ms = 0;
    uint64_t read_bytes = 0;
    uint64_t misses = 0;
    uint64_t metric_hits = 0;
    uint64_t metric_misses = 0;
    uint64_t tick_us = 0;
    uint64_t script_us = 0;
    uint64_t content_us = 0;
    uint64_t other_us = 0;
    uint64_t group_premul = 0;
    uint64_t group_readback = 0;
    Uint64 ms = 0;
};

static ProfileSnapshot profile_snapshot(const AppState* state) {
    ProfileSnapshot s;
    if (!state->rt) return s;
    s.frames = state->frames;
    s.ms = SDL_GetTicks();
    if (state->oaRender) {
        const oa::render::RenderStats& rs = state->oaRender->render_stats();
        s.draws = rs.draw_calls;
        s.batches = rs.batches;
        s.binds = rs.texture_binds;
        s.textures = rs.textures_created;
        s.uploads = rs.texture_uploads;
        s.upload_bytes = rs.upload_bytes;
        s.presents = rs.presents;
        const oa::render::RenderEngine::AssetStats& as =
            state->oaRender->asset_stats();
        s.decodes = as.image_decodes;
        s.decode_ms = as.decode_ms;
        s.read_bytes = as.read_bytes;
        s.misses = as.misses;
        const oa::render::RenderEngine::FontCacheStats fs =
            state->oaRender->font_cache_stats();
        s.metric_hits = fs.metrics_hits;
        s.metric_misses = fs.metrics_misses;
        const oa::render::RenderEngine::GroupStats gs =
            state->oaRender->group_stats();
        s.group_premul = gs.premul;
        s.group_readback = gs.readback;
    }
    {
        const oa::runtime::GameRuntime::TickProfile& tp = state->rt->tick_profile();
        s.tick_us = tp.script_us + tp.content_us + tp.other_us;
        s.script_us = tp.script_us;
        s.content_us = tp.content_us;
        s.other_us = tp.other_us;
    }
    return s;
}

static void profile_report(const AppState* state, const ProfileSnapshot& prev,
                           const ProfileSnapshot& cur) {
    const double dt_s = double(cur.ms - prev.ms) / 1000.0;
    if (dt_s <= 0.0) return;
    const uint64_t df = cur.frames - prev.frames;
    const double per_frame = df > 0 ? 1.0 / double(df) : 0.0;
    auto per = [&](uint64_t now, uint64_t before) {
        return double(now - before) * per_frame;
    };
    std::printf(
        "[prof] dt=%.1fs frames=%llu fps=%.1f | draws/f=%.1f batches/f=%.1f "
        "binds/f=%.1f | tex=%llu uploads/f=%.1f upMB/s=%.1f | "
        "decode/f=%.1f decode_ms/f=%.1f readMB/s=%.1f miss=%llu | "
        "glyphs h/f=%.1f m/f=%.1f | tick/f=%.2fms script=%.2fms content=%.2fms "
        "other=%.2fms | groups premul/f=%.2f readback/f=%.2f | layers=%zu\n",
        dt_s, (unsigned long long)df, double(df) / dt_s,
        per(cur.draws, prev.draws), per(cur.batches, prev.batches),
        per(cur.binds, prev.binds), (unsigned long long)(cur.textures - prev.textures),
        per(cur.uploads, prev.uploads),
        double(cur.upload_bytes - prev.upload_bytes) / (1024.0 * 1024.0) / dt_s,
        per(cur.decodes, prev.decodes), per(cur.decode_ms, prev.decode_ms),
        double(cur.read_bytes - prev.read_bytes) / (1024.0 * 1024.0) / dt_s,
        (unsigned long long)(cur.misses - prev.misses),
        per(cur.metric_hits, prev.metric_hits),
        per(cur.metric_misses, prev.metric_misses),
        per(cur.tick_us, prev.tick_us) / 1000.0,
        per(cur.script_us, prev.script_us) / 1000.0,
        per(cur.content_us, prev.content_us) / 1000.0,
        per(cur.other_us, prev.other_us) / 1000.0,
        per(cur.group_premul, prev.group_premul),
        per(cur.group_readback, prev.group_readback),
        state->rt ? state->rt->scene().size() : 0);
    std::fflush(stdout);
}

/// One profiler beat: no-op unless OA_PROFILE is set. Keeps its own previous
/// snapshot in a function-local static (one process = one run).
static ProfileSnapshot g_profile_start;
static bool g_profile_started = false;

/// Capture the steady-state baseline (called once the project has booted, so
/// load-time decoding is not billed to the first beat).
static void profile_begin(const AppState* state) {
    if (!std::getenv("OA_PROFILE")) return;
    g_profile_start = profile_snapshot(state);
    g_profile_started = true;
}

static void profile_beat(const AppState* state) {
    static const bool enabled = std::getenv("OA_PROFILE") != nullptr;
    if (!enabled) return;
    static ProfileSnapshot prev;
    static bool have_prev = false;
    if (!have_prev) {
        prev = g_profile_started ? g_profile_start : profile_snapshot(state);
        have_prev = true;
    }
    const ProfileSnapshot cur = profile_snapshot(state);
    if (cur.ms - prev.ms < 1000) return;   // at most one line per second
    profile_report(state, prev, cur);
    prev = cur;
}

// Last-step breadcrumb next to the exe. Heap smash (0xC0000374) will not
// unwind C++ catch; this file is unbuffered so the last Lua/tick/draw
// phase survives the process dying.
static std::string g_last_fn;
static std::string g_last_tag;

static FILE* crash_trace_file() {
    static FILE* f = nullptr;
    static bool inited = false;
    if (inited) return f;
    inited = true;
#ifdef _WIN32
    wchar_t mod[MAX_PATH] = {};
    // Wide module path: the exe may live under a CJK directory (the narrow
    // API answers in the ANSI code page and the narrow CRT open would fail).
    if (GetModuleFileNameW(nullptr, mod, MAX_PATH)) {
        const std::filesystem::path p =
            std::filesystem::path(mod).parent_path() / L"oa_last.log";
        f = _wfopen(p.c_str(), L"w");
        if (f)
            std::printf("[app] crash trace: %s\n", oa::util::path_to_utf8(p).c_str());
    }
#endif
    if (!f) f = std::fopen("oa_last.log", "w");
    if (f) setvbuf(f, nullptr, _IONBF, 0);
    return f;
}

static void crash_note(AppState* state, const char* phase) {
    FILE* f = crash_trace_file();
    if (!f) return;
    std::string wait = "?";
    size_t layers = 0;
    if (state && state->rt) {
        wait = wait_desc(state->rt->current_wait());
        layers = state->rt->scene().size();
    }
    std::fprintf(f, "f=%llu %s wait=%s fn=%s tag=%s layers=%zu\n",
                 (unsigned long long)(state ? state->frames : 0), phase,
                 wait.c_str(), g_last_fn.c_str(), g_last_tag.c_str(), layers);
}

#ifdef _WIN32
static LONG CALLBACK oa_vectored_crash(PEXCEPTION_POINTERS info) {
    const DWORD code = info && info->ExceptionRecord
                           ? info->ExceptionRecord->ExceptionCode
                           : 0;
    std::fprintf(stderr, "[app] win exception 0x%08lX at %p phase_fn=%s tag=%s\n",
                 static_cast<unsigned long>(code),
                 info && info->ExceptionRecord
                     ? info->ExceptionRecord->ExceptionAddress
                     : nullptr,
                 g_last_fn.c_str(), g_last_tag.c_str());
    std::fflush(stderr);
    if (FILE* f = crash_trace_file()) {
        std::fprintf(f, "EXCEPTION 0x%08lX at %p fn=%s tag=%s\n",
                     static_cast<unsigned long>(code),
                     info && info->ExceptionRecord
                         ? info->ExceptionRecord->ExceptionAddress
                         : nullptr,
                     g_last_fn.c_str(), g_last_tag.c_str());
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// OA_AUTO_START=1: drive language (bt_cn / bt_cn_next) then 开始游戏
// (bt_start) so a production exe can reproduce the start-game crash
// without OA_TEST_BUILD autodrive.
static void auto_start_pre_tick(AppState* state) {
    if (!std::getenv("OA_AUTO_START") || !state->rt || !state->oaRender) return;
    static int nav = 0;
    static const char* keys[] = {"bt_cn", "bt_cn_next", "bt_start"};
    static int hover_n = 0;
    static int cooldown = 0;
    static bool done = false;
    static bool release_click = false;
    // A held left_down would keep firing HUD buttons (config/quit) after
    // 开始游戏 replaces the title — tmny31 then confirmed bt_end.
    if (release_click) {
        state->input.left_down = false;
        release_click = false;
    }
    if (done) return;
    if (cooldown > 0) {
        --cooldown;
        return;
    }
    if (nav >= 3) {
        done = true;
        return;
    }
    const char* want = keys[nav];
    const oa::render::Layer* hit = nullptr;
    for (const oa::render::Layer* l : state->rt->scene().draw_order()) {
        const auto* h = state->rt->scene().find_event_handler(l->id, "click");
        if (!h) continue;
        const auto k = h->params.find("key");
        if (k != h->params.end() && k->second == want) {
            hit = l;
            break;
        }
    }
    if (!hit) {
        // Language keys are gone once title_init has run; skip them.
        if (std::strcmp(want, "bt_start") != 0 && state->saw_title_init) {
            std::printf("[app] AUTO_START skip %s (title already up)\n", want);
            ++nav;
            hover_n = 0;
        }
        return;
    }
    const auto r = state->oaRender->layer_world_rect(*hit);
    if (!r) return;
    const double x0 = (*r)[0];
    const double y0 = (*r)[1];
    const double rw = (*r)[2];
    const double rh = (*r)[3];
    if (x0 < 0) {
        hover_n = 0;
        return;
    }
    const int cx = int(x0 + rw / 2);
    const int cy = int(y0 + rh / 2);
    state->input.mouse_x = cx;
    state->input.mouse_y = cy;
    ++hover_n;
    if (hover_n == 1) {
        std::printf("[app] AUTO_START hover %s (%d,%d) f=%llu\n", want, cx, cy,
                    (unsigned long long)state->frames);
        std::fflush(stdout);
    }
    if (hover_n >= 90) {
        state->input.left_down = true;
        state->input.left_click_edge = true;
        release_click = true;
        std::printf("[app] AUTO_START click %s (%d,%d) f=%llu\n", want, cx, cy,
                    (unsigned long long)state->frames);
        std::fflush(stdout);
        crash_note(state, "auto-click");
        hover_n = 0;
        cooldown = 45;
        ++nav;
        if (std::strcmp(want, "bt_start") == 0) done = true;
    }
}

// ---------------------------------------------------------------------------
// app-host video: host texture key for the fullscreen surface + per-frame
// upload of decoded frames. Fullscreen frames are uploaded
// under the OverlayFrame domain key and blitted over the stage by the host;
// layer video frames are uploaded under the layer's VideoFrame domain key —
// the same key the scene resolves for the bound layer (layer-model:
// 资源读取域由角色实例决定,缓存按域分桶,不再有保留命名空间
// 拼写;engine render paths resolve the cache by key, so a bound layer draws
// the live frame through the normal texture lookup).
// ---------------------------------------------------------------------------

#if OA_TEST_BUILD
static uint64_t video_rgba_checksum(const uint8_t* rgba, size_t bytes) {
    uint64_t c = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; i += 4093) {
        c = (c ^ rgba[i]) * 1099511628211ull;
    }
    return c;
}
#endif

/// Upload every decoded frame that changed since the last pump. Returns true
/// while any video channel is showing decoded frames (forces per-frame
/// redraws — video is not a static frame).
/// Upload every stored emote static frame once per revision (the emote
/// engine stores the rendered figure; hosts stream it to the GPU like the
/// video layer frames). Returns true after uploading (forces a redraw).
static bool emote_pump_frames(AppState* state) {
    oa::runtime::GameRuntime* rt = state->rt.get();
    if (!rt) return false;
    // GPU compositing by default when a renderer exists —
    // the pose geometry (CPU mesh subdivision only) is rasterised into the
    // layer canvas with SDL_RenderGeometry; OA_EMOTE_GPU=0 restores the CPU
    // pixel fill + upload path. In GPU mode the players skip their own CPU
    // raster (external-pose mode) so pose updates stay cheap.
    static const bool gpu = std::getenv("OA_EMOTE_GPU") == nullptr ||
                            std::getenv("OA_EMOTE_GPU")[0] != '0';
    bool any = false;
    for (const auto& [id, st] : rt->emote_layers()) {
        if (!st.player) continue;
        st.player->set_external_pose(gpu);
        if (st.revision == 0 || state->layer_emote_rev[id] == st.revision) continue;
        // 读取域键 = 角色实例决定(emote 画布域,名 = 层 id;与场景侧
        // content_role_of(layer).texture_key 同源单点)。
        const oa::render::TextureKey tkey = oa::render::EmoteContent::canvas_key(id);
        bool presented = false;
        if (gpu && state->oaRender->renderer_ok()) {
            std::vector<oa::emote::EmoteDrawPart> parts;
            std::string err;
            if (st.player->collect_pose_parts(&parts, &err) &&
                state->oaRender->emote_render_parts(tkey, st.player->file(), parts,
                                                    st.width, st.height)) {
                presented = true;
                if (std::getenv("OA_EMOTE_DEBUG"))
                    std::fprintf(stderr, "[emote] host gpu-composited layer '%s' "
                                         "%dx%d parts=%zu\n",
                                 id.c_str(), st.width, st.height, parts.size());
            }
        }
        if (!presented) {
            const auto& rgba = st.player->rgba();
            if (state->oaRender->upload_host_frame(tkey, st.width, st.height,
                                                   rgba.data())) {
                presented = true;
                if (std::getenv("OA_EMOTE_DEBUG"))
                    std::fprintf(stderr, "[emote] host uploaded layer '%s' %dx%d\n",
                                 id.c_str(), st.width, st.height);
            }
        }
        if (presented) {
            state->layer_emote_rev[id] = st.revision;
            // layer-model 簿记: 实际上传/GPU 合成 = HostFrame 内容写
            // （与 video 帧同款；redraw 门由调用方用返回的 any 消费）。
            state->rt->scene().mark_dirty(id, oa::render::DirtyAspect::HostFrame);
            any = true;
        }
    }
    return any;
}

// Artemis layer (overlay) movies are effect strips drawn over
// the composed scene — snll's title petal layer (500.z.mv, sakura.ogv) is a
// white-petals-on-black canvas (98.9% of pixels luma<8, ~1% near-white) that
// must composite with black -> transparent, otherwise the opaque canvas
// blacks out the whole title underneath (the layer sits on top by design:
// the 500.z zone follows the system zzlogo/zzamask overlay convention). The
// keyed copy is uploaded under the layer's VideoFrame domain key.
// The key alpha map was retuned (oa::media::layer_video_key_alpha, shared with
// the runtime bind gate): canvas blacks (luma<=16) stay transparent but the
// mid-grey content of in-story weather strips (btjy snow03.ogv flakes live
// at luma ~16..190) now draws as authored — the old 32..224 ramp attenuated
// every mid-tone strip to near-zero alpha and the story-start snowfall was
// invisible. Bright strips (white petals / noise / flashes) are unchanged.
static void key_overlay_frame(const uint8_t* rgba, int w, int h,
                              std::vector<uint8_t>& out) {
    const size_t n = size_t(w) * size_t(h);
    out.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t lum = oa::media::rgba_luma(&rgba[i * 4]);
        out[i * 4 + 0] = rgba[i * 4 + 0];
        out[i * 4 + 1] = rgba[i * 4 + 1];
        out[i * 4 + 2] = rgba[i * 4 + 2];
        out[i * 4 + 3] = oa::media::layer_video_key_alpha(lum);
    }
}

static bool upload_layer_video_frame(AppState* state, const std::string& id,
                                     int w, int h, const uint8_t* rgba,
                                     bool key) {
    // This path only ever serves LAYER videos. A channel with
    // an auto-detected `_m` mask partner (ch.mask_on) already carries the
    // engine-level composite alpha (luma-key of the main picture x mask
    // gray), so its frames upload verbatim; unmasked channels get the
    // host-side luma-key copy (overlay frames take the raw branch in
    // video_pump_frames).
    // 读取域键 = 角色实例决定(视频帧域,名 = 通道 id = 层 id;与场景侧
    // content_role_of(layer).texture_key 同源单点)。
    const oa::render::TextureKey tkey = oa::render::VideoContent::frame_key(id);
    if (!key) return state->oaRender->upload_host_frame(tkey, w, h, rgba);
    static thread_local std::vector<uint8_t> keyed;
    key_overlay_frame(rgba, w, h, keyed);
    return state->oaRender->upload_host_frame(tkey, w, h, keyed.data());
}

static bool video_pump_frames(AppState* state) {
    oa::runtime::GameRuntime* rt = state->rt.get();
    oa::media::VideoEngine& ve = rt->video();
    bool active = false;
    // Layer videos: upload new frames under each layer's VideoFrame key.
    const auto vsnap = ve.state();
    for (const auto& [id, ch] : vsnap.video_layers) {
        if (!ch.playing || !ch.decoded) continue;
        active = true;
        const uint64_t rev = ve.frame_revision(id);
        if (rev == 0 || rev == state->layer_video_rev[id]) continue;
        int w = 0, h = 0;
        const uint8_t* rgba = nullptr;
        if (ve.video_frame(id, &w, &h, &rgba, nullptr) &&
            upload_layer_video_frame(state, id, w, h, rgba, !ch.mask_on)) {
            state->layer_video_rev[id] = rev;
            // layer-model 簿记: 实际上传 = HostFrame 内容写(与现状
            // video_active 帧项同帧,见泵写面)
            state->rt->scene().mark_dirty(id, oa::render::DirtyAspect::HostFrame);
        }
    }
    // overlay video (host-drawn surface).
    if (ve.is_overlay_playing()) {
        active = true;
        const uint64_t rev = ve.frame_revision("");
        if (rev != state->fs_video_rev) {
            int w = 0, h = 0;
            const uint8_t* rgba = nullptr;
            if (ve.video_frame("", &w, &h, &rgba, nullptr) &&
                state->oaRender->upload_host_frame(
                    oa::render::OverlayContent::frame_key(), w, h, rgba)) {
                state->fs_video_rev = rev;
                // layer-model 簿记: overlay 上传 = HostFrame 写
                state->rt->scene().mark_dirty(oa::render::kOverlayNodeId,
                                              oa::render::DirtyAspect::HostFrame);
#if OA_TEST_BUILD
                if (!state->video_demo_print_first || rev % 25 == 0 || rev == 1) {
                    state->video_demo_print_first = true;
                    std::printf("[video] frame rev=%llu %dx%d cksum=%016llx\n",
                        (unsigned long long)rev, w, h,
                        (unsigned long long)video_rgba_checksum(rgba,
                            size_t(w) * size_t(h) * 4));
                }
#endif
            }
        }
    }
    return active;
}

#if OA_TEST_BUILD
static void video_demo_start(AppState* state) {
    if (state->video_demo_file.empty() || state->video_demo_started) return;
    if (!state->rt->video().state().overlay_video) {
        if (std::getenv("OA_VIDEO_DEBUG")) {
            const auto pb = state->rt->fs()->read(
                state->rt->interpreter().resolve_magic_path(state->video_demo_file));
            std::fprintf(stderr, "[video] demo loader probe '%s' -> %s\n",
                state->video_demo_file.c_str(),
                pb ? "bytes" : "no bytes");
        }
        oa::media::VideoConfig cfg;
        cfg.file = state->video_demo_file; // logical name, magic-path resolved
        cfg.skippable = true;
        state->rt->video().play_overlay(cfg);
        state->video_demo_started = true;
        state->video_demo_start_frame = state->frames;
        std::printf("[video] demo play file=%s f=%llu\n",
            state->video_demo_file.c_str(), (unsigned long long)state->frames);
    }
}

static void video_demo_end(AppState* state, bool* quit) {
    if (state->video_demo_file.empty() || !state->video_demo_started ||
        state->video_demo_finished)
        return;
    if (state->rt->video().is_overlay_playing() ||
        state->frames <= state->video_demo_start_frame + 5)
        return;
    state->video_demo_finished = true;
    std::printf("[video] demo EOF f=%llu decoded_revs=%llu\n",
        (unsigned long long)state->frames,
        (unsigned long long)state->fs_video_rev);
    if (state->fs_video_rev == 0)
        std::fprintf(stderr, "[video] demo FAILED: no decoded frame appeared\n");
    *quit = true;
}

#endif // OA_TEST_BUILD

// ---------------------------------------------------------------------------
// Platform shell seams (windows / linux / android / wasm).
//
// The host keeps the SDL3 main-callback shape (AppInit / AppEvent /
// AppIterate / AppQuit), which desktop, Android (SDLActivity) and wasm (rAF)
// all drive unchanged — there is no #ifdef __linux__/__ANDROID__/EMSCRIPTEN
// and no SDL_GetPlatform branch in this file. The desktop-specific decisions
// are confined to three seams, all injectable from outside:
//   1. Data source: positional argument or $OA_PFS (never a machine path).
//      The IFileSystem mount below (DirFileSystem / PfsFileSystem + sidecar
//      dir layer) is chosen once here; an Android shell points it at the
//      app assets, a wasm shell at a preloaded/IDBFS file.
//   2. Save root: host_save_root() below — OA_SAVE_ROOT wins (any host can
//      set it), the test build falls back to the OS temp dir for autodrive,
//      otherwise the platform default: oa::plat::default_save_root()
//      (src/app/platform/Platform.h) — desktop answers the
//      game dir; android/wasm shells answer their writable location
//      (app-private storage / IDBFS "/save") through the same call. Core
//      stays oblivious: GameRuntime only sees the injected
//      oa::runtime::SaveStore (set_save_store, runtime_save.cpp), and
//      DirSaveStore is constructed with the resolved root string.
//   3. Presentation: --headless never touches SDL video; the windowed path
//      below (SDL_CreateWindow + RenderEngine::create_renderer) differs per
//      platform only in what SDL hands the host (native window / Android
//      surface / browser canvas); the window size comes from the project's
//      system.ini, not from the host.
// All env knobs read via getenv() work on the four targets (MSVC runtime /
// bionic / emscripten ENV).
// ---------------------------------------------------------------------------

// Save-store root policy — platform seam #2 above. Kept as one named
// function so a non-SDL host TU can mirror or reuse the exact rule. The
// env override and the test-build autodrive fallback are HOST policy; the
// final default (game dir on desktop) is the platform layer's answer
// (oa::plat::default_save_root).
static std::string host_save_root(const std::string& pfs_path, bool is_dir) {
    std::string save_root;
    const std::string root = oa::util::env_utf8("OA_SAVE_ROOT");
    if (!root.empty()) {
        save_root = root;
#if OA_TEST_BUILD
    } else if (std::getenv("OA_AUTODRIVE") != nullptr) {
        // Harness runs must never write into the game directory; fall
        // back to the OS temp dir (cross-platform; harness runs pass
        // OA_SAVE_ROOT explicitly anyway).
        std::error_code tec;
        const std::filesystem::path td = std::filesystem::temp_directory_path(tec);
        save_root = oa::util::path_to_utf8(td / "oa_autodrive_save");
        if (td.empty()) save_root = "oa_autodrive_save";
#endif
    } else {
        // Platform default: desktop = the game data directory itself
        // (parent of a .pfs archive / the folder tree); android = app
        // private storage; wasm = IDBFS "/save".
        save_root = oa::plat::default_save_root(pfs_path, is_dir);
    }
    return save_root;
}

static SDL_AppResult app_fail(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SDL_SetError("%s", buf);
    std::fprintf(stderr, "%s\n", buf);
#ifdef __OHOS__
    OH_LOG_Print(LOG_APP, LOG_ERROR, 0xFF00, "openartemis", "%{public}s", buf);
#endif
    return SDL_APP_FAILURE;
}

// Host argv normalization (Windows): the CRT answers argv in the process ANSI
// code page while every engine path is UTF-8. Convert in place, keeping the
// converted bytes alive for the process lifetime (argv must stay valid).
// POSIX already hands out the native bytes: identity.
static void normalize_host_argv(int argc, char** argv) {
#ifdef _WIN32
    static std::vector<std::string> converted;
    converted.reserve(size_t(argc) + 1);
    for (int i = 0; i < argc && argv[i]; ++i)
        converted.push_back(oa::util::host_bytes_to_utf8(argv[i]));
    for (int i = 0; i < argc && argv[i]; ++i)
        argv[i] = converted[size_t(i)].data();
#else
    (void)argc;
    (void)argv;
#endif
}

// Compat-manifest input gate (core/fs/compat_config.h): a console-style port
// may need the keyboard/wheel suppressed while mouse/touch still works.
// Absent config = today's behaviour (everything enabled).
static bool compat_keyboard_enabled(const AppState* state) {
    if (!state->rt) return true;
    const oa::fs::CompatConfig& c = state->rt->compat_config();
    return !c.has_input_gate || c.gate_keyboard;
}

static bool compat_wheel_enabled(const AppState* state) {
    if (!state->rt) return true;
    const oa::fs::CompatConfig& c = state->rt->compat_config();
    return !c.has_input_gate || c.gate_wheel;
}

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
    // the SDL3 main-callback driver runs event-gated when the
    // host sets SDL_MAIN_CALLBACK_RATE=waitevent (power-saving desktops /
    // launch wrappers): the generic loop blocks in SDL_WaitEvent until an
    // input event and then iterates exactly once, so every engine clock
    // (media decode pump, audio, scene tweens, timed waits) advances one
    // step per click — user-visible as "the title petal video plays a bit
    // then stops; each click advances one frame". The engine is real-time
    // by design (ticks carry all clocks), so iteration must never be
    // event-gated: force the rate to "0" (as-fast-as-the-app-paces; the
    // iterate tail's 60 Hz vsync/cap applies) with OVERRIDE priority, which
    // beats the environment variable (SDL hints: env < override).
    SDL_SetHintWithPriority(SDL_HINT_MAIN_CALLBACK_RATE, "0", SDL_HINT_OVERRIDE);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef _WIN32
    AddVectoredExceptionHandler(1, oa_vectored_crash);
#endif

    // 创建app
    AppState* state = new AppState();
    *appstate = state;

    // Host byte normalization (Windows): the CRT answers argv in the process
    // ANSI code page, but every engine path string is UTF-8 (research/129).
    // Re-encode the arguments ONCE here so a CJK install directory works on
    // every entry point (positional project path, --dump PATH, ...).
    normalize_host_argv(argc, argv);

    const int parsed = parse_args(argc, argv, state->opt);
    if (parsed == 1) return SDL_APP_SUCCESS; // --help printed
    if (parsed == 2) return SDL_APP_FAILURE; // usage error printed
    // Project default is never a machine path: positional arg wins, then
    // $OA_PFS; with neither, fail with usage (windows/android/wasm hosts and
    // CI must always name their data source explicitly).
    if (state->opt.pfs.empty()) {
        const std::string env_pfs = oa::util::env_utf8("OA_PFS");
        if (!env_pfs.empty()) state->opt.pfs = env_pfs;
    }
    if (state->opt.pfs.empty()) {
        print_usage(argv[0]);
        return app_fail("openartemis: no project given (positional argument or OA_PFS)");
    }
    const std::string pfs_path = state->opt.pfs;
    // Platform services (oa::plat): wasm mounts its IDBFS save
    // dir and wires page persistence here (before the save store below);
    // desktop/android TUs are documented no-ops.
    oa::plat::init();
    oa::log_build_info();
#if OA_TEST_BUILD
    if (const char* vd = std::getenv("OA_VIDEO_DEMO"); vd && *vd) {
        state->video_demo_file = vd;
        std::printf("[app] U9 video demo file: %s\n", vd);
    }
#endif

    // Folder start / unified source mount: a positional argument naming
    // a DIRECTORY is an extracted project tree (root contains system.ini); a
    // file is a PFS archive. Both mount through the same PhysicsFS-backed
    // PhysFileSystem (single VFS, core/fs/physfs_fs.h): an archive source
    // additionally overlays its own real parent directory with dir-first
    // priority (loose "patch" files shadow pack content), so installs that
    // keep big media outside the pack (snll movie/*.mp4 etc.) resolve like
    // the original engine.
    std::error_code ec;
    const bool is_dir =
        std::filesystem::is_directory(oa::util::native_path_from_utf8(pfs_path), ec);
    std::shared_ptr<oa::fs::IFileSystem> fs;
    try {
        fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path);
        if (is_dir) {
            std::printf("[app] folder project: %s\n", pfs_path.c_str());
        } else if (std::strcmp(fs->kind(), "pfs") == 0) {
            std::printf("[app] pfs project: %s\n", pfs_path.c_str());
        } else {
            const std::string side =
                oa::util::path_to_utf8(oa::util::native_path_from_utf8(pfs_path)
                                           .parent_path());
            std::printf("[app] pfs project: %s (sidecar dir layer: %s, dir "
                        "files take priority)\n",
                        pfs_path.c_str(), side.c_str());
        }
    }
    catch (const std::exception& e) {
        return app_fail("cannot open project source: %s", e.what());
    }
    state->rt = std::move(std::make_unique<oa::runtime::GameRuntime>(fs));
    // Media decode pool (audio/video worker host; see DecodePool): thread
    // count = OA_DECODE_THREADS env / platform default (wasm 1, native
    // min(4, cores)); decode runs off the tick thread, playback stays
    // deterministic (workers prefetch the same stream).
    state->rt->enable_decode_pool();
    // Real save store — root policy lives in host_save_root()
    // (seam #2 in the comment above; per-platform injection points listed
    // there). Tests and harness runs MUST pass OA_SAVE_ROOT=<tmp> and never
    // write into the game directory (assets are read-only).
    {
        const std::string save_root = host_save_root(pfs_path, is_dir);
        auto store = std::make_shared<oa::runtime::DirSaveStore>(save_root);
        std::error_code sec;
        std::filesystem::create_directories(
            oa::util::native_path_from_utf8(save_root), sec);
        state->rt->set_save_store(store);
        std::printf("[app] save store root: %s (default: game dir; override via "
                    "OA_SAVE_ROOT)\n",
                    oa::util::path_to_utf8(std::filesystem::absolute(
                        oa::util::native_path_from_utf8(save_root)))
                        .c_str());
    }
    try {
        state->rt->open_project(state->opt.platform);
    }
    catch (const std::exception& e) {
        return app_fail("project open failed: %s", e.what());
    }
    state->rt->interpreter().on_step = [=](const std::string&, size_t, const oa::runtime::Instruction& i) {
        const std::string* f = i.get("function");
        if (f && *f == "title_init" && !state->saw_title_init) {
            state->saw_title_init = true;
            std::printf("[app] title_init executed\n");
            std::fflush(stdout);
        }
        if (f && !f->empty()) {
            g_last_fn = *f;
        } else if (i.kind == oa::runtime::Instruction::Kind::Tag && !i.tag.empty()) {
            g_last_tag = i.tag;
        }
        };

    // RenderEngine must be created before open_project: its constructor wires
    // the text-metrics hook (message_layer_metrics) that Lua get_fontsize
    // needs during boot-time font_init → get_fontdata.
    state->oaRender = std::move(std::make_unique<oa::render::RenderEngine>(
        fs.get(), state->rt.get()));

    // Per-game compat manifest (core/fs/compat_config.h). CLI/env already won
    // where they apply (--platform, charset stays project data); the manifest
    // adds the pieces only the project knows: a font override for 汉化 patches
    // whose script fonts lack the translated glyphs, and an input gate for
    // console-style keyboard/wheel suppression.
    {
        const oa::fs::CompatConfig& compat = state->rt->compat_config();
        if (compat.has_font_override)
            state->oaRender->set_font_override(compat.font_override);
        if (compat.has_input_gate && !compat.gate_keyboard)
            std::printf("[app] input gate: keyboard suppressed by the manifest\n");
        if (compat.has_input_gate && !compat.gate_wheel)
            std::printf("[app] input gate: wheel->key mapping suppressed\n");
    }

    try {
        state->rt->boot_project();
    }
    catch (const std::exception& e) {
        return app_fail("project boot failed: %s", e.what());
    }
    std::printf("[app] project booted; %s\n",
        state->opt.headless ? "headless: driving GameRuntime with virtual 16ms ticks"
        : "rendering Lua layer events");
    profile_begin(state);  // OA_PROFILE steady-state baseline

    if (state->opt.headless) {
        // No pixels exist headless: complete the capture immediately so a
        // [trans] type!=0 cannot deadlock the wait machine (same fallback the
        // windowed path below uses when its snapshot fails).
        state->rt->set_transition_capture_callback([=]() { state->rt->mark_transition_captured(); });
    }
    else {
        // [trans] capture is a pure GPU copy now — the
        // "old scene" is still in the stage offscreen target at event-apply
        // time (last render_end left it there; nothing cleared it since), so
        // capture_transition_source swaps render targets and blits it into
        // the overlay texture. No SDL_RenderReadPixels anywhere on this
        // path. A false return (no stage frame rendered yet / backend
        // without render targets) still marks the capture complete so the
        // transition cannot deadlock the wait machine — it then fades in
        // without the old-scene overlay, like the old snapshot-failure
        // fallback.
        state->rt->set_transition_capture_callback([=]() {
            if (std::getenv("OA_TRANSDBG"))
                std::printf("[transcap] f=%llu gpu-copy old scene "
                            "(valid=%d)\n",
                            (unsigned long long)state->frames,
                            state->oaRender->stage_frame_ready());
            state->oaRender->capture_transition_source();
            state->rt->mark_transition_captured();
            });
    }

    // layer-event registries, hover/click/drag/push dispatch and Lua
    // calls moved into GameRuntime. The host keeps
    // an observer that reproduces the smoke diagnostics (hover-in/out,
    // click) with the same wording; actual dispatch runs inside rt->tick.
#if OA_TEST_BUILD
    using PD = oa::runtime::GameRuntime::PointerDispatch;
    {
        static size_t hover_prints = 0;
        static int click_prints = 0;
        state->rt->set_pointer_observer([=](const PD& d) {
            switch (d.kind) {
            case PD::Kind::HoverIn:
                if (hover_prints++ < 600)
                    std::printf("[app] hover-in  %-14s over=%s key=%s\n", d.layer.c_str(),
                        d.function.c_str(), d.key.c_str());
                break;
            case PD::Kind::HoverOut:
                if (hover_prints++ < 600)
                    std::printf("[app] hover-out %-14s out=%s\n", d.layer.c_str(),
                        d.function.c_str());
                break;
            case PD::Kind::Click:
                if (click_prints++ < 120)
                    std::printf("[app] click layer %-14s key=%s\n", d.layer.c_str(),
                        d.key.c_str());
                break;
            default:
                break; // push/drag diagnostics are not part of the smoke log
            }
            });
    }
#endif // OA_TEST_BUILD

#if !OA_TEST_BUILD
    {
        using PD = oa::runtime::GameRuntime::PointerDispatch;
        state->rt->set_pointer_observer([=](const PD& d) {
            if (d.kind != PD::Kind::Click) return;
            static int n = 0;
            if (n++ < 40) {
                std::printf("[app] click layer %s key=%s\n", d.layer.c_str(),
                            d.key.c_str());
                std::fflush(stdout);
            }
            g_last_tag = std::string("click:") + d.key;
            crash_note(state, "click");
        });
    }
#endif

    // The runtime owns pointer dispatch — it needs the texture sizes and
    // the alpha sampler above, plus the mouse/keys per tick (FrameInput).
    state->rt->set_hit_providers(oa::render::RenderEngine::hit_size, oa::render::RenderEngine::alpha_sampler, static_cast<void*>(state->oaRender.get()));

    if (state->opt.headless) {
        // ---- headless driver: no window/renderer, virtual 16 ms ticks -----
        // A fresh empty FrameInput every tick: no synthetic clicks/keys/pointer
        // are ever injected, so the game only ever follows its own logic.
        bool hquit = false;
        auto last_status = std::chrono::steady_clock::now();
        while (!hquit) {
#if OA_TEST_BUILD
            // OA_VIDEO_DEMO also works headless (decodes on the virtual
            // 16 ms clock; no pixels are uploaded without a renderer). Starts
            // past the boot movie request (~frame 74) for the same reason as
            // the windowed gate above.
            if (state->frames >= 200 && !state->video_demo_file.empty())
                video_demo_start(state);
#endif
            const oa::runtime::FrameInput hinput;
            try {
                state->rt->tick(16, hinput);
            }
            catch (const std::exception& e) {
                std::fprintf(stderr, "[app] tick error: %s\n", e.what());
                break;
            }
            if (state->rt->exit_requested()) hquit = true;
            for (const auto& e : state->rt->drain_events()) state->oaRender->process_event(e);
            ++state->frames;
#if OA_TEST_BUILD
            if (!state->video_demo_file.empty()) {
                const uint64_t rev = state->rt->video().frame_revision("");
                if (!state->video_demo_print_first && rev > 0) {
                    state->video_demo_print_first = true;
                    std::printf("[video] headless first rev=%llu\n",
                        (unsigned long long)rev);
                }
                if (rev > state->fs_video_rev) state->fs_video_rev = rev;
                if (state->video_demo_started &&
                    !state->rt->video().is_overlay_playing() &&
                    state->frames > state->video_demo_start_frame + 8) {
                    video_demo_end(state, &hquit);
                }
            }
#endif
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_status)
                .count() >= 5000) {
                last_status = now;
                print_status(state);
                profile_beat(state);
            }
            if (state->opt.frames_target > 0 && state->frames >= state->opt.frames_target) {
                // headless end-of-run stats (no pixels, so no luma snapshot)
                size_t handler_layers = 0;
                for (const oa::render::Layer* l : state->rt->scene().draw_order())
                    if (!l->event_handlers.empty()) ++handler_layers;
                std::printf("[app] headless end frames=%llu layers=%zu layer_ev=%zu "
                    "text_ev=%zu switch_ev=%zu msg_lines=%zu handlers=%zu wait=%s "
                    "media_ev=%zu audio_players=%zu bgm=%d se=%zu voice=%zu\n",
                    (unsigned long long)state->frames, state->rt->scene().size(),
                    state->rt->scene_layer_events(), state->rt->text_events(),
                    state->rt->message_switch_events(),
                    state->rt->text().visible_content_layers().size(), handler_layers,
                    wait_desc(state->rt->current_wait()).c_str(), state->rt->media_events(),
                    state->rt->media_players().active_players(),
                    (int)state->rt->audio().is_bgm_playing(),
                    state->rt->audio().state().se_channels.size(),
                    state->rt->audio().state().voice_channels.size());
                hquit = true;
            }
        }
        return SDL_APP_SUCCESS;
    }

    // ---- windowed host: create the window + renderer now (SDL video only
    // touched here, so --headless never needs a display) --------------------
#if defined(__OHOS__)
    // VintagePomelo's SDL_OHOS host already called SDL_Init. Re-running
    // VIDEO|AUDIO|EVENTS here can fail the whole boot on AUDIO, and SDL
    // may unwind VIDEO while the XComponent is live. Match KR2: only
    // create the window / GLES context.
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            return app_fail("SDL_Init(VIDEO) failed: %s", SDL_GetError());
        }
    }
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            std::fprintf(stderr, "[app] SDL_InitSubSystem(AUDIO) failed: %s\n",
                         SDL_GetError());
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "openartemis",
                         "SDL_InitSubSystem(AUDIO) failed: %{public}s", SDL_GetError());
        }
    }
#else
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        return app_fail("SDL_Init failed: %s", SDL_GetError());
    }
#endif
    // resizable window + letterbox presentation. Content
    // always renders into the fixed stage offscreen target; the letterbox
    // logical presentation (SDL_LOGICAL_PRESENTATION_LETTERBOX) scales that
    // stage frame aspect-preserving, centered
    // and maximally fitted to ANY window size (a 16:9 window fills it
    // completely; other aspect ratios letterbox with black bars — the
    // present path clears the backbuffer black). Renderer scale mode
    // defaults to LINEAR (SDL3), so resized text/graphics stay smooth;
    // pointer events already convert window -> logical stage coordinates
    // (get_renderer_coordinates), so hit-testing is
    // resize-proof with no further work.
    Uint32 win_flags = SDL_WINDOW_RESIZABLE;
    // Mobile shells ask for a fullscreen window (the krkrsdl3 reference does the
    // same for its embedded targets; desktop keeps the resizable windowed path).
    // A resizable window makes no sense here, so the flag is dropped.
    //
    // NOTE: SDL's Android backend does NOT apply the immersive style from the
    // creation flag — Android_CreateWindow never touches the window style; only
    // Android_SetWindowFullscreen does. So this flag alone decides nothing on
    // Android; the style is applied by the SDL_SetWindowFullscreen call below.
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || defined(__IPHONEOS__)
    win_flags = (win_flags | SDL_WINDOW_FULLSCREEN) & ~SDL_WINDOW_RESIZABLE;
#endif
    // GLES line: the window must carry SDL_WINDOW_OPENGL so the
    // backend can create its GLES context on it (SDL_GL_CreateContext); the
    // sdl line never adds the flag (SDL_Render handles its own surfaces).
    if (state->opt.renderer == "gles") win_flags |= SDL_WINDOW_OPENGL;
#if OA_TEST_BUILD
    // auto-drive: OA_AUTODRIVE=exit|title drives a deterministic windowed
    // journey (hidden window — renderer pixels still valid) so presented-pixel
    // residue checks run unattended. Auto-drive forces SDL_WINDOW_HIDDEN
    // below (deterministic windowed pixel baselines; logical stage
    // untouched); OA_AD_VISIBLE=1 shows the window (Xvfb mouse journeys).
    if (const char* ad = std::getenv("OA_AUTODRIVE"); ad && *ad) {
        // auto-drive debugging: unbuffered stdout so a crash keeps the tail
        // (otherwise the buffered log is lost with the process).
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        state->ad_flow = ad;
        // the hidden-window override must keep SDL_WINDOW_OPENGL
        // for the gles line (its backend creates the GL context on the
        // window; dropping the flag made hidden gles journeys fail with
        // "window isn't an OpenGL window").
        if (!std::getenv("OA_AD_VISIBLE")) {
            win_flags = SDL_WINDOW_HIDDEN;
            if (state->opt.renderer == "gles") win_flags |= SDL_WINDOW_OPENGL;
        } else {
            std::printf("[ad] visible window (OA_AD_VISIBLE)\n");
        }
        const std::string ui_out = oa::util::env_utf8("OA_UI_OUT");
        if (!ui_out.empty()) {
            state->ad_out = ui_out;
            std::error_code ec;
            std::filesystem::create_directories(
                oa::util::native_path_from_utf8(state->ad_out), ec);
        }
        std::printf("[ad] auto-drive flow=%s out=%s\n", ad, state->ad_out.c_str());
        if (const char* v = std::getenv("OA_R10_ALTER"); v && *v)
            state->r10_alt_max = std::atoi(v);
        if (const char* v = std::getenv("OA_R10_DWELL"); v && *v)
            state->r10_dwell_fixed = std::max(1, std::atoi(v));
        if (const char* v = std::getenv("OA_R10_SEED"); v && *v)
            state->r10_seed = (unsigned)std::atoi(v);
        if (const char* v = std::getenv("OA_R10_PNG"); v && *v)
            state->r10_png = std::atoi(v) != 0;
        if (const char* v = std::getenv("OA_R10_PAIR"); v && *v)
            state->r10_pair = v;
        if (const char* v = std::getenv("OA_R10_TRACE"); v && *v)
            state->r10_trace_left = std::max(0, std::atoi(v));
    }
#endif // OA_TEST_BUILD
    // Window opens at the project stage size (system.ini [WINDOWS]
    // WIDTH/HEIGHT; FPM 1280x720, NekoMiko 1920x1080 — hardcoding 1280x720
    // rendered later games partially/cropped). OA_WIN_W/OA_WIN_H (test build)
    // override the WINDOW size for presentation probes; the stage logical
    // size and every pixel read stay project-fixed.
    int stage_w = 1280, stage_h = 720;
    if (state->rt && state->rt->project_.config.stage_width > 0) {
        stage_w = state->rt->project_.config.stage_width;
        stage_h = state->rt->project_.config.stage_height;
    }
#if OA_TEST_BUILD
    if (const char* v = std::getenv("OA_WIN_W"); v && *v && std::atoi(v) > 0)
        stage_w = std::atoi(v);
    if (const char* v = std::getenv("OA_WIN_H"); v && *v && std::atoi(v) > 0)
        stage_h = std::atoi(v);
#endif
#if defined(__OHOS__)
    // Match KR2 (krkrsdl_harmony.cpp): disable SDL's touch↔mouse synthesis
    // (we map fingers ourselves) and do not pop the IME on window create.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
#ifdef SDL_HINT_ENABLE_SCREEN_KEYBOARD
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
#endif
    // Match KR2 (krkrsdl_harmony.cpp) and RPGRunner (sdl_misc.c): GLES
    // attributes first, then a 0×0 FULLSCREEN_DESKTOP window so the host
    // XComponent owns the surface. Scaling the 1920×1080 stage into that
    // surface is the engine's job (KR2 SDL_GL_DrawTexture / RPGRunner
    // recalculate_viewport), not an SDL2 fork.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP;
    state->window = oa_sdl2_CreateWindow()(
        "openartemis",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        0, 0, win_flags);
#else
    state->window = SDL_CreateWindow("openartemis", stage_w, stage_h, win_flags);
#endif
    if (!state->window) {
        return app_fail("SDL_CreateWindow failed: %s", SDL_GetError());
    }
#if defined(__OHOS__)
    // KR2 (krkrsdl_harmony.cpp): CreateContext immediately after the 0x0
    // FULLSCREEN_DESKTOP window, then MakeCurrent (return value ignored).
    {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(state->window, &ww, &wh);
        SDL_Log("[oa-kr2gl] window %dx%d, creating GLES context", ww, wh);
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "openartemis",
                     "[oa-kr2gl] window %{public}dx%{public}d", ww, wh);
        SDL_GLContext glctx = SDL_GL_CreateContext(state->window);
        if (!glctx) {
            SDL_ClearError();
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
            glctx = SDL_GL_CreateContext(state->window);
        }
        if (!glctx) {
            return app_fail("[oa-kr2gl] SDL_GL_CreateContext failed: %s", SDL_GetError());
        }
        // CreateContext already called eglMakeCurrent. A second
        // OHOS_GLES_MakeCurrent can recreate the XComponent surface and
        // unbind the working context (shader compile then sees no GL).
        if (SDL_GL_GetCurrentContext() != glctx)
            (void)oa_sdl2_GL_MakeCurrent()(state->window, glctx);
        // KR2: interval 1 immediately after CreateContext. Panel Hz is the
        // tick rate; this is not a 60 fps lock.
        SDL_GL_SetSwapInterval(1);
        SDL_StopTextInput();
        SDL_ShowWindow(state->window);
        {
            int dw = 0, dh = 0, ww = 0, wh = 0;
            SDL_GetWindowSize(state->window, &ww, &wh);
            SDL_GL_GetDrawableSize(state->window, &dw, &dh);
            SDL_Log("[oa-kr2gl] GLES context ready (current=%p swap=%d win=%dx%d drawable=%dx%d)",
                    (void*)SDL_GL_GetCurrentContext(), SDL_GL_GetSwapInterval(),
                    ww, wh, dw, dh);
            OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "openartemis",
                         "[oa-kr2gl] GLES ready win=%{public}dx%{public}d drawable=%{public}dx%{public}d",
                         ww, wh, dw, dh);
        }
    }
#endif
    // Mobile: apply SDL's own immersive fullscreen now that the window exists.
    //
    // This is the ONLY thing that actually enters fullscreen on Android (see the
    // win_flags comment above), and it is a window-STYLE change driven through
    // the Java layer (SDLActivity.setWindowStyle -> COMMAND_CHANGE_WINDOW_STYLE),
    // not a plain window property. It therefore runs before the renderer is
    // created, so nothing later has to survive a style change mid-flight.
    //
    // OA_ANDROID_FULLSCREEN=0 turns it off (A/B switch: it is the newest thing
    // on this path, so it must be possible to rule it out from the device
    // without a rebuild).
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || defined(__IPHONEOS__)
    bool want_fullscreen = true;
    if (const char* fs = std::getenv("OA_ANDROID_FULLSCREEN"); fs && *fs)
        want_fullscreen = std::atoi(fs) != 0;
    if (want_fullscreen) {
        if (!SDL_SetWindowFullscreen(state->window, true))
            std::fprintf(stderr, "[app] SDL_SetWindowFullscreen failed: %s\n", SDL_GetError());
        else
            std::printf("[app] immersive fullscreen requested\n");
    } else {
        std::printf("[app] immersive fullscreen disabled (OA_ANDROID_FULLSCREEN=0)\n");
    }
#endif
    if (!state->oaRender->create_renderer(state->window,
                                          state->opt.renderer)) {
        const char* se = SDL_GetError();
        return app_fail("[app] renderer creation failed (kind=%s): %s",
                        state->opt.renderer.c_str(),
                        (se && *se) ? se : "unknown");
    }
#if defined(__OHOS__)
    {
        int dw = 0, dh = 0, ww = 0, wh = 0;
        SDL_GetWindowSize(state->window, &ww, &wh);
        SDL_GL_GetDrawableSize(state->window, &dw, &dh);
        if (dw > 1 && dh > 1)
            state->oaRender->note_window_size(dw, dh);
        else if (ww > 1 && wh > 1)
            state->oaRender->note_window_size(ww, wh);
    }
#endif
    // Presentation diagnostics: the letterbox scale is what decides whether the
    // stage fills the screen or lands tiny in a corner, and it is derived from
    // the window's size vs its pixel size. A logical size far below the pixel
    // size (or vice versa) is what makes the stage occupy a fraction of the
    // surface, so print both once on startup.
    {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(state->window, &ww, &wh);
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(state->window, &pw, &ph);
        int dw = 0, dh = 0;
        (void)state->oaRender->present_size(&dw, &dh);
        std::printf("[app] geometry window=%dx%d pixels=%dx%d present=%dx%d stage=%dx%d\n",
                    ww, wh, pw, ph, dw, dh, stage_w, stage_h);
#if defined(__OHOS__)
        std::printf("[app] frame pace = display vsync (SwapInterval 1; not a 60 Hz lock)\n");
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "openartemis",
                     "[app] geometry win=%{public}dx%{public}d present=%{public}dx%{public}d stage=%{public}dx%{public}d",
                     ww, wh, dw, dh, stage_w, stage_h);
#else
        std::printf("[app] frame pace %d Hz (vsync + remainder cap; skip present when static)\n",
                    frame_pace_hz(state->opt));
#endif
        std::fflush(stdout);
    }

    // [mouse] pointer-warp ([mouse] config tag): the runtime asks the host to
    // move the OS pointer to a stage point — FPM's exit-confirm dialog flies
    // the cursor onto the YES button (dialog.lua yesno_active ->
    // mouse_autocursor, 10 eased steps). Warp in window coordinates
    // (letterbox-aware); SDL synthesizes real mouse-motion events, so hover /
    // rollover / click dispatch follows the normal chain. Windowed host only:
    // headless has no pointer and the request stays a no-op (counter only).
    state->rt->set_pointer_warp_callback([state](int sx, int sy) {
        if (!state->window || !state->oaRender) return;
        float wx = 0, wy = 0;
        if (state->oaRender->stage_to_window_coordinates(float(sx), float(sy), &wx, &wy))
            SDL_WarpMouseInWindow(state->window, wx, wy);
#if OA_TEST_BUILD
        // bounded diagnostics (a dialog animation emits ~10 warps)
        static size_t warp_prints = 0;
        if (warp_prints++ < 200)
            std::printf("[app] pointer-warp f=%llu (%d,%d)\n",
                        (unsigned long long)state->frames, sx, sy);
        state->ad_warp_x = sx;
        state->ad_warp_y = sy;
#endif
    });

    std::printf("[app] opening audio device...\n");
    std::fflush(stdout);
    state->rt->media_players().init();
    std::fflush(stdout);
    // movie (container) audio rides the SAME SDL stream the
    // sound players mix into. VideoEngine pushes its decoded 44100 stereo
    // through MediaPlayers (SDL_AudioStream serializes internally, so the
    // video driver thread may push while the tick thread mixes channels).
    // Headless/no-device runs drop the samples silently:
    // the video engine still decodes and counts without the hook.
    state->rt->video().set_audio_output(
        [mp = &state->rt->media_players()](const float* d, size_t n) {
            mp->push_video_audio(d, n);
        });
    // deterministic movie-stream flush at the video lifecycle
    // edges (start / EOF / every stop path) — a finished or stopped movie's
    // buffered tail must never keep playing over the next scene.
    state->rt->video().set_audio_flush(
        [mp = &state->rt->media_players()]() { mp->flush_video_audio(); });
    // [takess] frame capture — the host snapshots the frame it is
    // about to present (post_frame_capture runs right before present, after
    // this frame's scene drew; the takess event itself fired earlier in the
    // tick, so the captured frame is the pre-save-dialog scene).
    state->rt->set_frame_capture([=](uint32_t* w, uint32_t* h)
        -> std::optional<std::vector<uint8_t>> {
            oa::media::Image img;
            if (!state->oaRender->snapshot_renderer(img) || img.w <= 0 || img.h <= 0) {
                return std::nullopt;
            }
            *w = uint32_t(img.w);
            *h = uint32_t(img.h);
            return std::move(img.rgba);
        });
    state->last = SDL_GetTicks();
    return SDL_APP_CONTINUE;
}

// ---------------------------------------------------------------------------
// Touch -> mouse bridging.
// ---------------------------------------------------------------------------
// SDL's touch→mouse synthesis is off (SDL_HINT_TOUCH_MOUSE_EVENTS=0, same as
// KR2). Finger events must become pointer events here or the screen is dead
// while injected sdlInputMouse (virtual cursor) still works.
//
// Harmony KR2 (krkrsdl_harmony.cpp): press left on first finger-down, release
// on last finger-up. A 12 px "tap vs drag" gate on high-DPI panels swallows
// ordinary taps. Two fingers cancel the left button and tap-right on lift.
#ifdef OA_USE_SDL2
#define OA_FINGER_ID(ev) ((ev)->tfinger.fingerId)
#define OA_EVENT_KEY(ev) ((ev)->key.keysym.sym)
#else
#define OA_FINGER_ID(ev) ((ev)->tfinger.fingerID)
#define OA_EVENT_KEY(ev) ((ev)->key.key)
#endif

enum class TouchPhase { Idle, SingleFinger, MultiFinger };

struct TouchFinger {
    float x = 0.0f;  // latest window coordinates
    float y = 0.0f;
    float start_x = 0.0f;
    float start_y = 0.0f;
    bool moved = false;
};

struct TouchGestureState {
    TouchPhase phase = TouchPhase::Idle;
    std::map<SDL_FingerID, TouchFinger> fingers;
    bool single_left_down = false;
    // Two-finger right tap: defer the button-up one frame so the engine sees
    // the press held across a tick (same as the old one-finger tap path).
    Uint8 pending_button = 0;
    float pending_x = 0.0f;
    float pending_y = 0.0f;
};
namespace {
TouchGestureState g_touch;
}  // namespace

// Normalized 0..1 threshold, same magnitude as KR2 HTS_MOVE_THRESHOLD_SQ
// (0.0001 → 1% of the surface). Used only for two-finger tap vs scroll.
constexpr float kTouchMoveThresholdSq = 0.0001f;

/// Pushes a mouse button event into the SDL queue in window coordinates.
void touch_push_mouse(Uint32 type, Uint8 button, float wx, float wy) {
    SDL_Event e{};
    e.type = type;
    e.button.button = button;
    e.button.clicks = 1;
#ifdef OA_USE_SDL2
    e.button.x = (Sint32)wx;
    e.button.y = (Sint32)wy;
#else
    e.button.x = wx;
    e.button.y = wy;
#endif
    e.button.which = SDL_TOUCH_MOUSEID;
    SDL_PushEvent(&e);
}

void touch_push_motion(float wx, float wy) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
#ifdef OA_USE_SDL2
    e.motion.x = (Sint32)wx;
    e.motion.y = (Sint32)wy;
#else
    e.motion.x = wx;
    e.motion.y = wy;
#endif
    e.motion.which = SDL_TOUCH_MOUSEID;
    SDL_PushEvent(&e);
}

/// Press now, release on the next frame (see TouchGestureState::pending_button).
/// The pointer is moved to the tap point first: every one of this host's pointer
/// consumers hit-tests against the position the runtime holds for the frame
/// (runtime mouse_x_/mouse_y_), and that position is only written by MOTION
/// events. A desktop mouse always moves before it clicks, so a tap does the
/// same — this is what makes a tap land on the button it visually covers.
void touch_tap(AppState* state, Uint8 button, float wx, float wy) {
    if (std::getenv("OA_TOUCH_DIAG")) {
        float rx = wx, ry = wy;
        if (!state->oaRender->get_renderer_coordinates(rx, ry, &rx, &ry)) {
            rx = wx;
            ry = wy;
        }
        std::fprintf(stderr, "[touch] %s tap win=(%.0f,%.0f) stage=(%d,%d)\n",
                     button == SDL_BUTTON_RIGHT ? "two-finger" : "one-finger",
                     wx, wy, int(rx), int(ry));
        std::fflush(stderr);
    }
    touch_push_motion(wx, wy);
    touch_push_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, button, wx, wy);
    g_touch.pending_button = button;
    g_touch.pending_x = wx;
    g_touch.pending_y = wy;
}

/// Called once per frame from SDL_AppIterate: finish the previous frame's tap.
void touch_advance_frame() {
    if (g_touch.pending_button == 0) return;
    const Uint8 b = g_touch.pending_button;
    g_touch.pending_button = 0;
    touch_push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, b, g_touch.pending_x, g_touch.pending_y);
}

void touch_handle(AppState* state, const SDL_Event* ev) {
    // KR2 hts_toPixel: SDL finger coords are 0..1, multiply by the
    // drawable/surface (letterbox output), not the possibly-stale window
    // size from CreateWindow(0,0) / stage.
    int win_w = 0, win_h = 0;
    if (!state->oaRender || !state->oaRender->present_size(&win_w, &win_h) ||
        win_w <= 1 || win_h <= 1) {
        SDL_GetWindowSize(state->window, &win_w, &win_h);
#ifdef OA_USE_SDL2
        int dw = 0, dh = 0;
        SDL_GL_GetDrawableSize(state->window, &dw, &dh);
        if (dw > 1 && dh > 1) {
            win_w = dw;
            win_h = dh;
        }
#endif
    }
    if (win_w <= 1 || win_h <= 1) return;
    const float nx = ev->tfinger.x;
    const float ny = ev->tfinger.y;
    const float wx = nx * float(win_w);
    const float wy = ny * float(win_h);

    if (ev->type == SDL_EVENT_FINGER_DOWN) {
        TouchFinger f;
        f.x = f.start_x = wx;
        f.y = f.start_y = wy;
        g_touch.fingers[OA_FINGER_ID(ev)] = f;
        if (g_touch.fingers.size() == 1) {
            // KR2: left button down on first contact so a VN tap advances
            // even if the finger jitters, and hover/hit-test see the press.
            g_touch.phase = TouchPhase::SingleFinger;
            g_touch.single_left_down = true;
            touch_push_motion(wx, wy);
            touch_push_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, wx, wy);
        } else if (g_touch.fingers.size() == 2) {
            if (g_touch.single_left_down) {
                const TouchFinger& first = g_touch.fingers.begin()->second;
                touch_push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT,
                                 first.x, first.y);
                g_touch.single_left_down = false;
            }
            g_touch.phase = TouchPhase::MultiFinger;
        } else {
            if (g_touch.single_left_down) {
                touch_push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT, wx, wy);
                g_touch.single_left_down = false;
            }
            g_touch.phase = TouchPhase::Idle;
        }
        return;
    }

    if (ev->type == SDL_EVENT_FINGER_MOTION) {
        auto it = g_touch.fingers.find(OA_FINGER_ID(ev));
        if (it == g_touch.fingers.end()) return;
        TouchFinger& f = it->second;
        f.x = wx;
        f.y = wy;
        const float dx = nx - (f.start_x / float(win_w));
        const float dy = ny - (f.start_y / float(win_h));
        if (dx * dx + dy * dy > kTouchMoveThresholdSq) f.moved = true;
        if (g_touch.phase == TouchPhase::SingleFinger) touch_push_motion(f.x, f.y);
        return;
    }

    if (ev->type == SDL_EVENT_FINGER_UP) {
        auto it = g_touch.fingers.find(OA_FINGER_ID(ev));
        if (it == g_touch.fingers.end()) return;
        TouchFinger f = it->second;
        f.x = wx;
        f.y = wy;
        const int remaining = int(g_touch.fingers.size()) - 1;
        g_touch.fingers.erase(it);
        if (remaining > 0) return;

        if (g_touch.phase == TouchPhase::SingleFinger && g_touch.single_left_down) {
            touch_push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT, f.x, f.y);
            g_touch.single_left_down = false;
        } else if (g_touch.phase == TouchPhase::MultiFinger) {
            if (!f.moved) touch_tap(state, SDL_BUTTON_RIGHT, f.x, f.y);
        }
        g_touch.phase = TouchPhase::Idle;
        g_touch.single_left_down = false;
        return;
    }
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* ev)
{
    AppState* state = static_cast<AppState*>(appstate);

    // Key-edge vectors accumulate across the events of one frame and are
    // cleared at the end of SDL_AppIterate (per-event clearing dropped the
    // edges of earlier events in the same batch, e.g. a wheel notch followed
    // by pointer motion).
    if (ev->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;
    if (ev->type == SDL_EVENT_FINGER_DOWN ||
        ev->type == SDL_EVENT_FINGER_MOTION ||
        ev->type == SDL_EVENT_FINGER_UP) {
        touch_handle(state, ev);
        return SDL_APP_CONTINUE;
    }
#ifdef OA_USE_SDL2
    if (ev->type == SDL_WINDOWEVENT) {
        if (ev->window.event == SDL_WINDOWEVENT_RESIZED ||
            ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            ev->window.event == SDL_WINDOWEVENT_EXPOSED) {
            if (ev->window.data1 > 1 && ev->window.data2 > 1)
                state->oaRender->note_window_size(ev->window.data1, ev->window.data2);
            state->force_repaint = true;
        }
    }
#else
    if (ev->type == SDL_EVENT_WINDOW_RESIZED ||
        ev->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        ev->type == SDL_EVENT_WINDOW_EXPOSED) {
        // The static-frame skip keeps the last presented image, so a
        // window resize would otherwise leave the window stale until the next
        // dirty frame. Force one repaint (the letterbox logical presentation
        // re-fits the stage frame to the new window size on that present).
        state->force_repaint = true;
    }
#endif
    if (ev->type == SDL_EVENT_KEY_DOWN) {
        if (OA_EVENT_KEY(ev) == SDLK_ESCAPE)
            return SDL_APP_SUCCESS;
        if (compat_keyboard_enabled(state)) {
            const int vk = sdl_key_to_vk(OA_EVENT_KEY(ev));
            if (vk > 0 && state->kbd_down.insert(vk).second)
                state->input.key_down_edges.push_back(vk);
        }
    }
    if (ev->type == SDL_EVENT_KEY_UP) {
        if (compat_keyboard_enabled(state)) {
            const int vk = sdl_key_to_vk(OA_EVENT_KEY(ev));
            if (vk > 0 && state->kbd_down.erase(vk) > 0)
                state->input.key_up_edges.push_back(vk);
        }
    }
    if (ev->type == SDL_EVENT_MOUSE_WHEEL) {
        if (!compat_wheel_enabled(state)) return SDL_APP_CONTINUE;
        // Artemis maps the mouse wheel onto the HUP/HDW key
        // pair — vk 136 (up) / 137 (down) — the same domain FPM's keyconfig
        // table keys (csv.advkey.def: 136/137 ≡ PageUp/PageDown semantics:
        // adv BACKLOG/CLICK; ui HUP/HDW; vsync.lua also kills edge 136 during
        // transitions). The engine/Lua chain needs nothing else: FPM
        // registers setonpush rows for every def key, so a wheel edge rides
        // the normal push dispatch (wheel-up opens the backlog, wheel-down
        // script-advances the page). Positive y = away from the user.
        //
        // SDL has no wheel "release" — without a paired key-up the vk
        // would read as perpetually held to any isDown/isUpEdge consumer and
        // the OS event queue would keep the "scroll" draining after the user
        // stops. Each notch therefore starts a short press: down edge now and
        // the key stays down for exactly 2 frames, then a synthetic up edge
        // releases it (SDL_AppIterate below). Wheel bursts stay per-notch:
        // the AppEvent edge vectors are per-frame, so multiple notches in one
        // frame coalesce into one action (no free-run accumulation).
        if (ev->wheel.y > 0) {
            state->input.key_down_edges.push_back(136);
            state->wheel_hold_frames[136] = 2;
        } else if (ev->wheel.y < 0) {
            state->input.key_down_edges.push_back(137);
            state->wheel_hold_frames[137] = 2;
        }
    }
    if (ev->type == SDL_EVENT_MOUSE_MOTION ||
        ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        ev->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        // Window/surface pixels → engine stage, including letterbox (KR2
        // hts_windowToDrawablePixel + hts_pixelToLocal).
        float rx = 0, ry = 0;
        bool have = false;
        if (ev->type == SDL_EVENT_MOUSE_MOTION) {
            rx = float(ev->motion.x);
            ry = float(ev->motion.y);
            have = true;
        } else if (ev->button.x != 0 || ev->button.y != 0) {
            // sdlInputMouse's SendMouseButton path has no x/y (0,0); keep
            // the last motion. PushEvent / touch synthesis include coords.
            rx = float(ev->button.x);
            ry = float(ev->button.y);
            have = true;
        }
        if (have) {
            if (state->oaRender->get_renderer_coordinates(rx, ry, &rx, &ry)) {
                state->input.mouse_x = (int)rx;
                state->input.mouse_y = (int)ry;
            } else if (ev->type == SDL_EVENT_MOUSE_MOTION) {
                state->input.mouse_x = (int)ev->motion.x;
                state->input.mouse_y = (int)ev->motion.y;
            }
        }
    }
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        // Artemis vk space: 1 = left, 2 = right, 3 = middle (the engine's push
        // rows and rclick chain key on these; the game wires right-button =
        // EXIT on UI windows). Left keeps its dedicated FrameInput path; the
        // other buttons ride the key edge sets like keyboard keys.
        const Uint8 b = ev->button.button;
        const int vk = b == SDL_BUTTON_LEFT    ? 1
                       : b == SDL_BUTTON_RIGHT ? 2
                       : b == SDL_BUTTON_MIDDLE ? 3
                                                : 0;
        if (vk == 1) {
            if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                state->input.left_down = true;
            else
                state->input.left_down = false;
        } else if (vk == 2 || vk == 3) {
            if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (state->mouse_held.insert(vk).second)
                    state->input.key_down_edges.push_back(vk);
            } else if (state->mouse_held.erase(vk) > 0) {
                state->input.key_up_edges.push_back(vk);
            }
        }
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
    AppState* state = static_cast<AppState*>(appstate);

    // Finish a tap that started in a previous frame: the press was pushed then,
    // the release goes out now so the engine sees the button held for a frame
    // instead of a press/release pair collapsed into one tick.
    touch_advance_frame();

    // Platform lifecycle (oa::plat): android pause and
    // wasm tab-hide block/stall the loop while SDL_GetTicks keeps running —
    // on resume the first tick must not see the whole paused gap as one
    // giant delta (engine clocks, wait timers and media advance by delta,
    // runtime.cpp tick, unclamped), so reset the pacing clock once per
    // background->foreground round trip. Desktop never reports one (the
    // desktop loop is never OS-blocked); the poll is a cheap false-return
    // atomic load on every platform.
    if (oa::plat::take_lifecycle_resume()) {
        state->last = SDL_GetTicks();
        std::printf("[app] platform resumed; tick clock reset\n");
    }

    const Uint64 frame_start = SDL_GetTicks();
#if defined(__OHOS__)
    // Surface size often lands after the first CreateWindow (0x0 / stage).
    // Static-frame skip would freeze that postage-stamp present. Also
    // re-layout when ArkTS pushes TAPIR_PORTRAIT_TOP_OFFSET.
    if (state->window) {
        int dw = 0, dh = 0;
        SDL_GL_GetDrawableSize(state->window, &dw, &dh);
        static int s_prev_dw = -1, s_prev_dh = -1, s_prev_pct = -999;
        int pct = 0;
        if (const char* env = std::getenv("TAPIR_PORTRAIT_TOP_OFFSET"); env && *env)
            pct = std::atoi(env);
        if (dw != s_prev_dw || dh != s_prev_dh || pct != s_prev_pct) {
            s_prev_dw = dw;
            s_prev_dh = dh;
            s_prev_pct = pct;
            if (dw > 1 && dh > 1)
                state->oaRender->note_window_size(dw, dh);
            state->force_repaint = true;
        }
    }
#endif
#if OA_TEST_BUILD
    // auto-drive: apply the actions the state machine armed (keys/click)
    // before the per-frame edge computation consumes them.
    if (!state->ad_flow.empty()) ad_pre_tick(state);
#endif
    // held-key snapshot (Ctrl role-14 etc.); the manifest input gate drops the
    // keyboard half while pointer buttons below stay live.
    if (compat_keyboard_enabled(state)) state->input.keys_down = state->kbd_down;
    else state->input.keys_down.clear();
    for (const int mk : state->mouse_held) state->input.keys_down.insert(mk);
#if OA_TEST_BUILD
    // single synthetic probe click two frames after the plateau metric
    if (state->probe_click_at_ >= 0 && state->frames >= uint64_t(state->probe_click_at_)) {
        state->probe_click_at_ = -1;
        state->input.left_down = true;
        state->mouse_left_prev = false;
        std::printf("[app] P1c probe click at frame %llu (id=%s)\n",
            (unsigned long long)state->frames, state->probe_id.c_str());
    }
#endif
    state->input.left_click_edge = state->input.left_down && !state->mouse_left_prev;
    state->mouse_left_prev = state->input.left_down;
    auto_start_pre_tick(state);
#if OA_TEST_BUILD
    // rollover/rollout + click + drag dispatch now happen inside
    // rt->tick (runtime-owned). The smoke
    // orchestration below only watches the runtime hover set to decide
    // when the title button is reached.
    const bool hovered = state->rt->is_hovered(state->probe_id);
    // text smoke (OA_TXT_SMOKE=1)：尽力走剧情文本（见下方块）
    const bool txt_smoke = std::getenv("OA_TXT_SMOKE") != nullptr;
    {
        // once the title button is actually hovered (probe reached), give
        // the run a settled tail so the end-of-run metric lands on the
        // visible title (hovered button state stays up while parked).
        if (hovered && !state->probe_id.empty() && !state->hover_end_armed && state->opt.frames_target > 0) {
            state->hover_end_armed = true;
            state->settle_frame_ = state->frames;
            std::printf("[app] P1b hover settled at frame %llu (id=%s)\n",
                (unsigned long long)state->frames, state->probe_id.c_str());
        }
        // text smoke (OA_TXT_SMOKE=1): 尽力走 "はじめから" 到达剧情
        // 文本帧。标题停驻后点 bt_start 推进游戏（Stop 停驻排水已在队列第
        // 6 项转正为常开，点击的 estag/jump 链照常执行）；不结束 run（用
        // CLI --frames 决定时长），周期打印文本引擎内容与字形绘制状态。
        // 能到哪算哪（依赖菜单 conf 域）。
        if (txt_smoke && state->hover_end_armed && !state->end_committed_ &&
            state->frames >= state->settle_frame_ + 120 && !state->rt->transition().is_in_progress(
                state->rt->now_ms()) &&
            hovered) {
            state->end_committed_ = true;
            state->probe_click_at_ = state->frames + 2;
            std::printf("[app] TXT smoke: click bt_start at frame %llu\n",
                (unsigned long long)state->frames);
        }
        // after the entrance settles wait for a plateau (no transition in
        // flight, pointer still on the button) and end there; fall back to
        // settle+900 so the run always terminates.
        if (state->hover_end_armed && !state->end_committed_ && state->opt.frames_target > 0 &&
            !txt_smoke) {
            if (state->frames >= state->settle_frame_ + 900) {
                state->end_committed_ = true;
                state->opt.frames_target = state->frames + 60;
                std::printf("[app] P1b fallback end at frame %llu (no plateau after %llu)\n",
                    (unsigned long long)state->opt.frames_target,
                    (unsigned long long)state->settle_frame_);
            }
            else if (state->frames >= state->settle_frame_ + 40 &&
                !state->rt->transition().is_in_progress(state->rt->now_ms()) && hovered) {
                // robust plateau: the documented "stable visible
                // title". The original first-hover-gap trigger fires at
                // the first no-transition gap after the button becomes
                // hoverable, which happens while the title is still
                // animating in (its background layers fade in seconds
                // later) — the metric then samples an all-black frame,
                // both before and after the plateau rule. Sample the
                // presented pixels while hovered and quiet, and commit
                // only on consecutive clearly-visible samples (the FPM
                // title screen is bright, luma ~197; the entrance phase
                // is luma 0).
                if (state->bright_sample_count < 0) state->bright_sample_count = 0;
                if (state->bright_sample_count >= 60) {
                    // stable visible title confirmed (>= 1 s of bright
                    // samples at 60 fps); record the metric from the
                    // presented content right here (a later re-read could
                    // land on a flicker boundary) and end shortly after.
                    state->end_committed_ = true;
                    state->probe_click_at_ = state->frames + 2;
                    const double pl = state->oaRender->frame_luma();
                    state->opt.frames_target = state->frames + 1 + 30;
                    std::printf("[app] P1b plateau at frame %llu; metric snapshot + end "
                        "%llu\n",
                        (unsigned long long)state->frames,
                        (unsigned long long)state->opt.frames_target);
                    std::printf("[app] title-metric frame=%llu luma=%.1f layers=%zu "
                        "drawn=%zu hover=%s\n",
                        (unsigned long long)(state->frames + 1), pl,
                        state->rt->scene().size(), state->s_last_drawn, state->probe_id.c_str());
                }
                else {
                    const double pl = state->oaRender->frame_luma();
                    state->bright_sample_count =
                        pl > 120.0 ? state->bright_sample_count + 1 : 0;
                }
            }
        }
        if (std::getenv("OA_P1B_DEBUG") && state->hover_end_armed && state->frames % 30 == 0) {
            const std::set<std::string>& hs = state->rt->hovered_layers();
            std::string top = hs.empty() ? "" : *hs.begin();
            std::printf("[app] diary f=%llu hover=%s wait=%s trans=%d layers=%zu\n",
                (unsigned long long)state->frames,
                top.empty() ? "-" : top.c_str(),
                wait_desc(state->rt->current_wait()).c_str(),
                (int)state->rt->transition().is_in_progress(state->rt->now_ms()),
                state->rt->scene().size());
        }
    }
#endif // OA_TEST_BUILD

#if OA_TEST_BUILD
    if (txt_smoke && state->hover_end_armed && state->end_committed_ && state->frames % 120 == 0) {
        const auto layers = state->rt->text().visible_content_layers();
        std::string sample;
        bool body = false;
        const std::string act = state->rt->text().active_layer_id();
        if (!layers.empty()) {
            if (const oa::render::MessageLayer* ml0 = state->rt->text().layer(layers[0])) {
                for (const auto& u : ml0->page) {
                    if (u.data.empty()) continue;
                    sample = u.data.substr(0, 24);
                    break;
                }
            }
            for (const auto& id : layers) {
                if (state->rt->text().page_has_visible_text(id)) {
                    body = true;
                    break;
                }
            }
        }
        static bool body_verdict_printed = false;
        if (body && !body_verdict_printed) {
            body_verdict_printed = true;
            std::printf("[txt] BODY-TEXT-REACHED f=%llu layer=%s sample='%s'\n",
                (unsigned long long)state->frames, act.c_str(), sample.c_str());
        }
        std::printf("[txt] f=%llu wait=%s text_layers=%zu glyphs=%zu active=%s "
            "body=%d sample='%s'\n",
            (unsigned long long)state->frames, wait_desc(state->rt->current_wait()).c_str(),
            layers.size(), state->s_last_text_glyphs, act.c_str(), (int)body,
            sample.c_str());
    }
#endif // OA_TEST_BUILD
    // decide injection (overrideKey 124) intentionally omitted: it drives
    // [stop]-released flows into save/menu contexts that need M8 domains

    state->input.keys_down.insert(state->input.key_down_edges.begin(), state->input.key_down_edges.end());
    // wheel pulse tail: hold the wheel vk down for the pulsed frames,
    // then emit the synthetic key-up edge exactly once (see the wheel event
    // handler). The pulse window keeps e:isDown(vk) true for ~2 frames and
    // then restores the clean released state for every later frame.
    for (auto it = state->wheel_hold_frames.begin(); it != state->wheel_hold_frames.end();) {
        const int vk = it->first;
        if (it->second > 0) {
            state->input.keys_down.insert(vk);
            --it->second;
            if (it->second == 0) state->input.key_up_edges.push_back(vk);
            ++it;
        } else {
            it = state->wheel_hold_frames.erase(it);
        }
    }
#if OA_TEST_BUILD
    // OA_VIDEO_DEMO starts after the boot phase (loader is wired at
    // open_project). Frame 300 sits past FPM's pre-title boot movie request
    // (~1 s in), so the demo channel is not clobbered by the boot chain's
    // own [video] (movie/logo.dat is absent: logical fallback, instant).
    if (state->frames >= 300 && !state->video_demo_file.empty())
        video_demo_start(state);
#endif
    const Uint64 now = SDL_GetTicks();
    const uint64_t delta = now - state->last;
    state->last = now;
    try {
        if (state->frames == 0) {
            std::printf("[app] first tick...\n");
            std::fflush(stdout);
        }
        crash_note(state, "tick-enter");
        state->rt->tick(delta > 0 ? delta : 1, state->input);
        crash_note(state, "tick-ok");
        if (state->frames == 0) {
            std::printf("[app] first tick done\n");
            std::fflush(stdout);
        }
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "[app] tick error: %s\n", e.what());
        return SDL_APP_FAILURE;
    }
    // per-frame edge lifecycle: everything this frame appended (SDL events
    // and synthetic wheel-pulse up edges) is consumed by the tick above and
    // must not leak into the next frame when no new events arrive.
    state->input.key_down_edges.clear();
    state->input.key_up_edges.clear();
    if (state->rt->exit_requested()) state->quit = true;
    const std::vector<oa::runtime::Event> drained_events = state->rt->drain_events();
    crash_note(state, "events");
    for (const auto& e : drained_events) state->oaRender->process_event(e);
    crash_note(state, "pump");
    // upload newly decoded video frames (before the draw pass so bound
    // layer textures resolve; forces redraws while a channel is active).
    // static emote textures before the draw pass. A fresh emote pose revision
    // must force a repaint exactly like a video frame does: otherwise the
    // static-frame skip freezes a parked portrait (no text reveal / tween /
    // transition) even though the player keeps breathing underneath.
    const bool emote_active = emote_pump_frames(state);
    const bool video_active = video_pump_frames(state);
#if OA_TEST_BUILD
    // OA_STATE_LOG=1: fine-grained headless-style state heartbeat (every 120
    // frames) for external drivers (Xvfb UX journeys): wait kind, layer count
    // and drawn glyph count — coarse 5 s statuses are too slow to drive
    // multi-phase UI flows (Xvfb evidence).
    if (std::getenv("OA_STATE_LOG") && state->frames % 120 == 0) {
        std::printf("[app] st frames=%llu wait=%s layers=%zu glyphs=%zu\n",
                    (unsigned long long)state->frames,
                    wait_desc(state->rt->current_wait()).c_str(), state->rt->scene().size(),
                    state->s_last_text_glyphs);
    }
#endif

    // static-frame skip + intermediate-composite cache: track whether
    // anything that could change the picture happened this tick. Scene
    // mutations (layer events/tweens/handlers) are consumed inside tick,
    // so compare the runtime counters with the previous frame; pointer
    // dispatches may flip visuals through Lua (which lands in the counters
    // on a later tick — keep the conservative delta here too). An active
    // tween/[anime]/transition also forces per-frame redraws.
    static uint64_t s_prev_layer_ev = 0, s_prev_tween_ev = 0;
    static uint64_t s_prev_tween_calls = 0, s_prev_pointer = 0;
    static uint64_t s_prev_text_rev = 0; // 文本内容版本（跳帧不吞文本）
    const bool text_changed = state->rt->text_revision() != s_prev_text_rev;
    s_prev_text_rev = state->rt->text_revision();
    // layer-model: 现状判据的四个计数项先分项成布尔
    // （合并式不变;分项只供 OA_DIRTY_AB 归因打印）。
    const bool layer_ev_delta =
        state->rt->scene_layer_events() != s_prev_layer_ev;
    const bool tween_ev_delta =
        state->rt->scene_tween_events() != s_prev_tween_ev;
    const bool completion_delta =
        state->rt->tween_completion_calls() != s_prev_tween_calls;
    const bool pointer_delta =
        state->rt->pointer_dispatch_count() != s_prev_pointer;
    const bool scene_mutated =
        layer_ev_delta || tween_ev_delta || completion_delta || pointer_delta;
    s_prev_layer_ev = state->rt->scene_layer_events();
    s_prev_tween_ev = state->rt->scene_tween_events();
    s_prev_tween_calls = state->rt->tween_completion_calls();
    s_prev_pointer = state->rt->pointer_dispatch_count();
    const bool animated_now =
        state->rt->scene().has_tweens() || state->rt->scene().has_anime();
    // The transition overlay changes every frame while active; its capture
    // texture also needs dropping exactly when it stops being drawable.
    const bool trans_now = state->rt->transition().is_in_progress(state->rt->now_ms()) ||
        state->rt->transition().active() || state->oaRender->is_transition_active();
    // [alldelete]: global fade-out multiplier; active fades force
    // per-frame redraws and drop cached group bakes (they would freeze at
    // their bake-time alpha).
    const double global_fade = state->rt->all_delete_fade();
    const bool fade_active = global_fade < 1.0;
    static bool s_prev_animated = false;
    const bool frame_dirty =
        scene_mutated || animated_now || s_prev_animated || trans_now ||
        !drained_events.empty() || text_changed || video_active ||
        emote_active || fade_active; // alldelete fade redraws every frame
    s_prev_animated = animated_now;

    // layer-model: 统一 invalidate 簿记 —— 每帧决策点消费
    // （本帧 tick 内派发/媒体帧末的 Compositor 写 + 本帧宿主视频泵上传）。
    // 同进程 A/B 见 OA_TEST_BUILD 段(测试版)。两种构建都必须消费(有界簿记)。
    const std::map<std::string, uint16_t> dirty_report =
        state->rt->scene().consume_dirty_report();
    (void)dirty_report; // 用户构建只消费(有界簿记),A/B 见测试段
#if OA_TEST_BUILD
    const bool ledger_dirty = !dirty_report.empty();
    // OA_DIRTY_AB 累计统计(函数级 static —— 退出点在帧函数尾部汇总)
    static uint64_t s_ab_frames = 0, s_ab_set_mm = 0, s_ab_redraw_mm = 0,
                    s_ab_bake_mm = 0;
    static bool s_ab_aborted = false, s_ab_summarized = false;
    // OA_DIRTY_AB: 同进程 A/B —— 同一帧分别按"现状判据"
    // （旧计数项 + 活动查询）与"簿记集合"（ledger_dirty + 同款活动查询）产出
    // 去重集合，逐帧比对。不一致打帧并累计类别，退出时汇总。=1 只报；=2 额外
    // 在首个会翻转消费者等价布尔（重绘门/烘焙失效）的帧 abort。渲染门本身
    // 不受影响。
    if (const char* ab = std::getenv("OA_DIRTY_AB"); ab && *ab) {
        const bool ab_strict = ab[0] == '2';
        // 类别位（两边同词汇）:scene=1 text=2 anim=4 anim-prev=8 trans=16
        // drain=32 video=64 fade=128 emote=256（现状四计数项合并为 scene ——
        // 与消费者合并式一致；分项见打印归因）。重绘门等价 = 集合空性等价
        // （!s_rendered_any / repaint 两边同源，不参与差异）。
        const uint16_t old_set = uint16_t(
            (scene_mutated ? 1 : 0) | (text_changed ? 2 : 0) |
            (animated_now ? 4 : 0) | (s_prev_animated ? 8 : 0) |
            (trans_now ? 16 : 0) | (!drained_events.empty() ? 32 : 0) |
            (video_active ? 64 : 0) | (fade_active ? 128 : 0) |
            (emote_active ? 256 : 0));
        const uint16_t new_set = uint16_t(
            (ledger_dirty ? 1 : 0) | (text_changed ? 2 : 0) |
            (animated_now ? 4 : 0) | (s_prev_animated ? 8 : 0) |
            (trans_now ? 16 : 0) | (!drained_events.empty() ? 32 : 0) |
            (video_active ? 64 : 0) | (fade_active ? 128 : 0) |
            (emote_active ? 256 : 0));
        const bool bake_old = scene_mutated || animated_now || s_prev_animated ||
                              text_changed || fade_active || video_active ||
                              emote_active;
        const bool bake_new = ledger_dirty || animated_now || s_prev_animated ||
                              text_changed || fade_active || video_active ||
                              emote_active;
        const bool set_mismatch = old_set != new_set;
        const bool redraw_mismatch = (old_set == 0) != (new_set == 0);
        const bool bake_mismatch = bake_old != bake_new;
        ++s_ab_frames;
        if (set_mismatch) ++s_ab_set_mm;
        if (redraw_mismatch) ++s_ab_redraw_mm;
        if (bake_mismatch) ++s_ab_bake_mm;
        const uint64_t mm_total = s_ab_set_mm;
        if (set_mismatch || bake_mismatch) {
        if (mm_total <= 1000) {
            std::printf("[dirtyab] f=%llu old=%04x new=%04x bake_old=%d "
                        "bake_new=%d scene(ly=%d tw=%d done=%d ptr=%d "
                        "ledger=%zu)%s%s\n",
                        (unsigned long long)state->frames, old_set, new_set,
                        (int)bake_old, (int)bake_new, (int)layer_ev_delta,
                        (int)tween_ev_delta, (int)completion_delta,
                        (int)pointer_delta, dirty_report.size(),
                        redraw_mismatch ? " REDRAW-FLIP" : "",
                        bake_mismatch && !redraw_mismatch ? " BAKE-FLIP" : "");
            if (redraw_mismatch || bake_mismatch) {
                // 归因:会翻门的簿记条目(最多 6 条)
                size_t shown = 0;
                for (const auto& [id, bits] : dirty_report) {
                    if (shown++ >= 6) break;
                    std::printf("[dirtyab]   ledger id='%s' bits=%04x\n",
                                id.c_str(), bits);
                }
            }
        }
        }
        if (ab_strict && !s_ab_aborted &&
            (redraw_mismatch || bake_mismatch)) {
            s_ab_aborted = true;
            std::fprintf(stderr,
                         "[dirtyab] ABORT (strict): f=%llu old=%04x new=%04x "
                         "bake %d/%d\n",
                         (unsigned long long)state->frames, old_set, new_set,
                         (int)bake_old, (int)bake_new);
            std::exit(2);
        }
        // 旅程/运行结束(或每 4096 帧心跳)出汇总
        const bool run_end = state->quit;
        if ((run_end || (state->frames > 0 && state->frames % 4096 == 0)) &&
            !s_ab_summarized) {
            if (run_end) s_ab_summarized = true;
            std::printf("[dirtyab] summary frames=%llu set_mismatch=%llu "
                        "redraw_mismatch=%llu bake_mismatch=%llu dropped=%zu\n",
                        (unsigned long long)s_ab_frames,
                        (unsigned long long)s_ab_set_mm,
                        (unsigned long long)s_ab_redraw_mm,
                        (unsigned long long)s_ab_bake_mm,
                        state->rt->scene().dirty_ledger_dropped());
        }
    }
#endif // OA_TEST_BUILD

    // text content changes (reveal/page/flush) invalidate cached
    // offscreen group bakes too — glyphs now render inside their node's
    // subtree, so a bake that contains text must be rebuilt when the text
    // changed without any scene mutation.
    // Active video also invalidates group bakes: a video layer inside a
    // baked group refreshes its texture every frame and would otherwise
    // keep drawing the stale bake. An animated emote canvas is the same
    // kind of live host texture.
    if (scene_mutated || animated_now || s_prev_animated || text_changed ||
        fade_active || video_active || emote_active) {
        state->oaRender->render_clear_tex_cache();
    }

    // ---- render (static-frame skip: identical frames keep the last
    // presented image —  last_submitted_frame logic) --
    static bool s_rendered_any = false;
    // window resize/expose events force one repaint at the new size.
    const bool repaint_requested = state->force_repaint;
    state->force_repaint = false;
    if (frame_dirty || !s_rendered_any || repaint_requested) {
        crash_note(state, "draw");

        state->oaRender->render_beigin();
        // L2 M1: ONE recursive scene traversal — dotted-id tree
        // pre-order == draw order. Each node draws its image content AND the
        // glyphs bound to it at its own slot (later nodes cover them;
        // intermediate_render groups composite offscreen only when needed);
        // global_fade ([alldelete]) multiplies the whole chain. The old
        // flat-vs-slow dual paths are gone (they duplicated the semantics and
        // the slow path re-copied the tree map at every recursion level —
        // background-freeze hotspot).
        const oa::render::Compositor& sc = state->rt->scene();
        const size_t drawn = state->oaRender->draw_scene(sc, global_fade);

        // L2 M3: glyphs painted at their node slots inside draw_scene (the
        // old scene-wide trailing text pass is gone — every drawable message
        // owns a scene node materialized by the runtime).
        const size_t text_glyphs = state->oaRender->last_frame_glyphs();
        state->s_last_text_glyphs = text_glyphs;
#if OA_TEST_BUILD
        // smoke print: gated (post-M3 the count covers inline glyphs, so
        // a plain windowed run would otherwise spam every 20 frames).
        if (text_glyphs && std::getenv("OA_GLYPH_LOG") && state->frames % 20 == 0)
            std::printf("[app] text glyphs drawn: %zu\n", text_glyphs);
#endif
        state->s_last_drawn = drawn;
        state->oaRender->progress_transition();
        // OA_RENDER_DIAG (temporary diagnostic): low-frequency
        // stage-luma/drawn/backend-error canary for backend line comparisons.
        // the error surface is the backend's (last_error) — the
        // sdl line reports SDL_GetError-sourced text, the gles line its own
        // GLES error text; SDL_ClearError is gone (it used to clear SDL's
        // error state every 30 frames; backend errors are sticky instead).
        if (std::getenv("OA_RENDER_DIAG") && state->frames % 30 == 0) {
            oa::media::Image diag;
            double dl = -1;
            if (state->oaRender->snapshot_renderer(diag) && diag.w > 0)
                dl = oa::media::band_luma(diag, 0, 720);
            const char* err = state->oaRender->renderer_error();
            std::printf("[diag] f=%llu drawn=%zu stage_luma=%.1f "
                        "backend_err=%s\n",
                (unsigned long long)state->frames, drawn, dl,
                err && err[0] ? err : "(none)");
        }
        // the overlay video is composited as a topmost virtual scene
        // layer bound to the uploaded overlay video texture; it draws in
        // the normal scene pass above every script layer (no host blit).
        // deferred [takess] capture — snapshot this frame's
        // content (pre-present) when a takess request is pending.
        state->rt->post_frame_capture();
        crash_note(state, "present");
        state->oaRender->render_end();
#if OA_TEST_BUILD
        // auto-drive: sample the just-presented pixels (tail stage) and
        // note that a fresh frame exists for the driver's residue counting.
        if (!state->ad_flow.empty()) {
            if (state->ad_stage >= 4 && state->ad_ppm_count < 10)
                ad_sample(state, true);
            else if (state->ad_stage >= 4)
                ad_sample(state, false);
            state->ad_rendered = true;
        }
#endif
        s_rendered_any = true;
    }
#if defined(__OHOS__)
    else if (s_rendered_any) {
        // KR2 SwapWindow every loop iteration so vsync blocks even on a
        // static scene. Re-blit the last stage so the back buffer is defined.
        crash_note(state, "present");
        state->oaRender->render_end();
    }
#endif
    ++state->frames;
    if (state->frames == 1) {
        std::printf("[app] first-frame layers=%zu\n", state->rt->scene().size());
        std::fflush(stdout);
    }
#if OA_TEST_BUILD
    // auto-drive state machine (runs every frame; pixel metrics only
    // count on frames that actually rendered).
    if (!state->ad_flow.empty() && ad_drive(state)) state->quit = true;
#endif
    // default continuous mode (no --frames): ~5 s status heartbeat so it
    // is easy to see where the game is parked (frame / wait / layers).
    // OA_PROFILE beats fire on the same 5 s cadence in BOTH modes (a bounded
    // --frames profiling run is the common case).
    if (state->opt.frames_target != 0) {
        const Uint64 now_pf = SDL_GetTicks();
        if (state->last_profile_ms == 0) state->last_profile_ms = now_pf;
        if (now_pf - state->last_profile_ms >= 5000) {
            state->last_profile_ms = now_pf;
            profile_beat(state);
        }
    }
    if (state->opt.frames_target == 0) {
        const Uint64 now_hb = SDL_GetTicks();
        if (state->last_status_ms == 0) state->last_status_ms = now_hb;
        if (now_hb - state->last_status_ms >= 5000) {
            state->last_status_ms = now_hb;
            print_status(state);
            profile_beat(state);
            // the audio latency heartbeat. The queued level of
            // a bound device stream IS the audible delay, so this samples it
            // every status beat — the real-machine number for "is the sound
            // late right now", printable without waiting for AppQuit (the
            // OA_AUDIO_DIAG summary below stays the end-of-run census).
            if (std::getenv("OA_AUDIO_DIAG") && state->rt) {
                auto& mp = state->rt->media_players();
                const size_t mix = mp.audio_level_frames(0);
                const size_t mov = mp.audio_level_frames(1);
                // the mix stream's STANDING level is what a
                // sound started now would suffer; `under` counts the device
                // callbacks that had to pad a live stream with silence (a hole
                // in the middle of the sound = the click this revision fixes).
                std::printf("[audiolat] f=%llu mix=%zu (%.0fms) movie=%zu "
                            "(%.0fms) target=%zu paced_out=%llu refill=%llu "
                            "cb=%llu under_mix=%llu under_movie=%llu\n",
                            (unsigned long long)state->frames, mix,
                            double(mix) / 44.1, mov, double(mov) / 44.1,
                            mp.mix_target_frames(),
                            (unsigned long long)mp.paced_out_ticks(),
                            (unsigned long long)mp.refill_frames(),
                            (unsigned long long)mp.device_callbacks(),
                            (unsigned long long)mp.device_underruns(0),
                            (unsigned long long)mp.device_underruns(1));
            }
        }
    }
#if OA_TEST_BUILD
    // title-button probe (windowed smoke, mouse-only): once the title
    // registers its button handlers, park the virtual pointer on the
    // bt_start child's world clip center every frame (tracking it while
    // entrance tweens slide the group) so the hover/rollover chain prints
    // the expected id + Lua fn. Handler rows now live on the scene layers
    // (the handler rows live on the scene layers); helper below reads the
    // FPM naming (row param "click"/"over"
    // or the dispatched "function").
    auto handler_param = [](const oa::render::LayerEventHandler* h,
        const char* k) -> std::string {
            if (!h) return std::string();
            const auto it = h->params.find(k);
            return it == h->params.end() ? std::string() : it->second;
        };
    auto click_label = [=](const std::string& id) -> std::string {
        const auto* h = state->rt->scene().find_event_handler(id, "click");
        std::string v = handler_param(h, "click");
        if (v.empty()) v = handler_param(h, "function");
        return v;
        };
    auto over_label = [=](const std::string& id) -> std::string {
        const auto* h = state->rt->scene().find_event_handler(id, "rollover");
        std::string v = handler_param(h, "over");
        if (v.empty()) v = handler_param(h, "function");
        return v;
        };
    if (state->probe_id.empty() && state->frames >= 200) {
        for (const oa::render::Layer* l : state->rt->scene().draw_order()) {
            const auto* h = state->rt->scene().find_event_handler(l->id, "click");
            if (!h) continue;
            const auto k = h->params.find("key");
            if (k == h->params.end() || k->second != "bt_start") continue;
            const auto n = h->params.find("name");
            if (n != h->params.end() && n->second == "ttl1") { // title group
                state->probe_id = l->id;
                std::printf("[app] P1b probe frame=%llu target id=%s (click=%s over=%s "
                    "key=%s)\n",
                    (unsigned long long)state->frames, state->probe_id.c_str(),
                    click_label(state->probe_id).c_str(), over_label(state->probe_id).c_str(),
                    k->second.c_str());
                break;
            }
        }
        if (state->probe_id.empty()) { // fallback: any bt_start keyed layer
            for (const oa::render::Layer* l : state->rt->scene().draw_order()) {
                const auto* h = state->rt->scene().find_event_handler(l->id, "click");
                if (!h) continue;
                const auto k = h->params.find("key");
                if (k == h->params.end() || k->second != "bt_start") continue;
                state->probe_id = l->id;
                std::printf("[app] P1b probe frame=%llu fallback target id=%s\n",
                    (unsigned long long)state->frames, state->probe_id.c_str());
                break;
            }
        }
        if (!state->probe_id.empty()) {
            // world geometry of the title button chain at probe time:
            // group origins + child world rects prove the parent+child
            // composition lands (early-era l/t examples are superseded by
            // world rects)
            for (const oa::render::Layer* l : state->rt->scene().draw_order()) {
                if (l->id.rfind("500.", 0) != 0) continue;
                oa::render::Affine2 t;
                if (!state->rt->scene().world_transform(l->id, &t)) continue;
                double ox = 0, oy = 0;
                t.transform_point(0, 0, &ox, &oy);
                const auto wrect = state->oaRender->layer_world_rect(*l);
                const std::string cl = click_label(l->id);
                if (wrect)
                    std::printf("[app] wr id=%-14s origin=(%.0f,%.0f) world=(%.0f,%.0f "
                        "%.0fx%.0f) cl=%s\n",
                        l->id.c_str(), ox, oy, (*wrect)[0], (*wrect)[1],
                        (*wrect)[2], (*wrect)[3],
                        cl.empty() ? "-" : cl.c_str());
                else
                    std::printf("[app] wr id=%-14s origin=(%.0f,%.0f) group(no quad)\n",
                        l->id.c_str(), ox, oy);
            }
        }
    }
    if (!state->probe_id.empty() && state->frames >= 200 && state->opt.frames_target > state->frames + 2) {
        const oa::render::Layer* pl = state->rt->scene().find(state->probe_id);
        if (pl) {
            if (const auto r = state->oaRender->layer_world_rect(*pl)) {
                const int cx = int((*r)[0] + (*r)[2] / 2);
                const int cy = int((*r)[1] + (*r)[3] / 2);
                if (cx != state->input.mouse_x || cy != state->input.mouse_y) {
                    state->input.mouse_x = cx;
                    state->input.mouse_y = cy;
                }
                if (!state->probe_rect_printed) {
                    state->probe_rect_printed = true;
                    std::printf("[app] P1b %s world=(%.0f,%.0f %.0fx%.0f) probe=(%d,%d)\n",
                        state->probe_id.c_str(), (*r)[0], (*r)[1], (*r)[2], (*r)[3], cx,
                        cy);
                }
            }
        }
    }
    // Deterministic smoke end: once the title has run, keep the windowed
    // run alive so the entrance sequence finishes and the end-of-run
    // luma/draw snapshot (or the stable-rect plateau) lands on the stable
    // title screen. 520 frames was too short for the FPM entrance (the
    // title content appears only ~500 frames after the [stop] park);
    // the longer tail keeps the run alive (the static-frame skip makes those
    // frames cheap when the scene is at rest).
    if (state->saw_title_init && state->opt.frames_target > 0) {
        static bool window_armed = false;
        if (!window_armed) {
            window_armed = true;
            state->opt.frames_target = state->frames + 1500;
            std::printf("[app] title reached; extending finite run to frame %llu\n",
                (unsigned long long)state->opt.frames_target);
        }
    }
    if (state->frames == 330) {
        // world geometry of the 500.b.N title-button chain: parent groups
        // pass their offset down, the .0 children carry the btn.png clip
        // quad — world rect = full ancestor composition.
        for (const oa::render::Layer* l : state->rt->scene().draw_order()) {
            if (l->id.rfind("500.b.", 0) != 0 && l->id.rfind("500.lo", 0) != 0) continue;
            oa::render::Affine2 t;
            if (state->rt->scene().world_transform(l->id, &t)) {
                double ox = 0, oy = 0;
                t.transform_point(0, 0, &ox, &oy);
                const auto wrect = state->oaRender->layer_world_rect(*l);
                const std::string cl = click_label(l->id);
                if (wrect)
                    std::printf("[app] wr id=%-14s origin=(%.0f,%.0f) world=(%.0f,%.0f "
                        "%.0fx%.0f) cl=%s\n",
                        l->id.c_str(), ox, oy, (*wrect)[0], (*wrect)[1],
                        (*wrect)[2], (*wrect)[3],
                        cl.empty() ? "-" : cl.c_str());
                else
                    std::printf("[app] wr id=%-14s origin=(%.0f,%.0f) group(no quad)\n",
                        l->id.c_str(), ox, oy);
            }
        }
        for (const oa::render::Layer* l : state->rt->scene().draw_order()) {
            const auto* h = state->rt->scene().find_event_handler(l->id, "click");
            if (!h) continue;
            std::string nm = handler_param(h, "name");
            std::string ky = handler_param(h, "key");
            std::string cl = click_label(l->id);
            if (!cl.empty() && !nm.empty())
                std::printf("[app] h id=%-16s name=%-8s key=%-8s over=%s\n",
                    l->id.c_str(), nm.c_str(), ky.c_str(),
                    over_label(l->id).c_str());
        }
    }
    if (state->frames == 200) {
        const oa::runtime::WaitReason* w = state->rt->current_wait();
        std::printf("[app] frame200 wait=%d id='%s' pos=%s:%zu\n",
            w ? (int)w->kind : -1,
            w ? w->id.c_str() : "", state->rt->interpreter().current_script()
            ? state->rt->interpreter().current_script()->c_str()
            : "(none)",
            state->rt->interpreter().current_line());
    }
    if (state->frames == 150 || state->frames == 400) {
        const auto order = state->rt->scene().draw_order();
        std::printf("[app] frame %llu draw-tail (topmost 6):\n",
            (unsigned long long)state->frames);
        for (size_t i = order.size() > 6 ? order.size() - 6 : 0; i < order.size(); ++i) {
            const oa::render::Layer* l = order[i];
            std::printf("    id=%-16s vis=%d alpha=%.2f file=%s left=%.0f top=%.0f\n",
                l->id.c_str(), (int)l->visible, l->alpha, l->file.c_str(), l->left,
                l->top);
        }
    }
    if (state->frames == 40 && std::getenv("OA_DUMP_FRAME")) {
        // early snapshot to catch the caution/brand frame content
    }
    if (state->metric_at_frame_ > 0 && state->frames == state->metric_at_frame_) {
        // plateau metric: read the presented pixels right after the
        // stable title+button frame (no transition in flight).
        const double luma = state->oaRender->frame_luma();
        std::printf("[app] title-metric frame=%llu luma=%.1f layers=%zu drawn=%zu "
            "hover=%s\n",
            (unsigned long long)state->frames, luma, state->rt->scene().size(), state->s_last_drawn,
            state->probe_id.c_str());
    }
#endif // OA_TEST_BUILD
    if (state->opt.frames_target > 0 && state->frames >= state->opt.frames_target) {
        oa::media::Image frame;
        if (state->oaRender->snapshot_renderer(frame)) {
            const double luma = oa::media::band_luma(frame, 0, 720);
            std::printf("[app] frame luma=%.1f layers=%zu drawn=%zu layer_ev=%zu "
                "text_ev=%zu switch_ev=%zu msg_lines=%zu\n",
                luma, state->rt->scene().size(), state->s_last_drawn, state->rt->scene_layer_events(),
                state->rt->text_events(), state->rt->message_switch_events(),
                state->rt->text().visible_content_layers().size());
            std::printf("[app] trans_ev=%zu tween_ev=%zu tween_calls=%zu "
                "trans_in_progress=%d trans_progress=%.2f trans_cap=%d "
                "active_tweens=%d\n",
                state->oaRender->transition_events_size(), state->rt->scene_tween_events(),
                state->rt->tween_completion_calls(),
                (int)state->rt->transition().is_in_progress(state->rt->now_ms()),
                state->rt->transition().progress(state->rt->now_ms()),
                (int)(state->oaRender->is_transition_active()),
                (int)state->rt->scene().has_tweens());
            size_t handler_layers = 0;
            for (const oa::render::Layer* ll : state->rt->scene().draw_order())
                if (!ll->event_handlers.empty()) ++handler_layers;
            std::printf("[app] handlers=%zu\n", handler_layers);
            const size_t total_chars = state->rt->text().visible_content_layers().size();
            std::printf("[app] text layers=%zu glyphs_last_frame=%zu\n", total_chars,
                state->s_last_text_glyphs);
            if (state->opt.dump_on_end) write_ppm(state->opt.dump, frame);
            state->quit = true;
        }
        else {
            std::fprintf(stderr, "[app] readpixels failed: %s\n", SDL_GetError());
            state->quit = true;
        }
    }
#if OA_TEST_BUILD
    // demo: leave once the overlay video reached EOF (or the fallback
    // fired with no decodable file — fs_video_rev stays 0 and the summary
    // reports the failure).
    video_demo_end(state, &state->quit);
#endif
    pace_windowed_frame(frame_pace_hz(state->opt), frame_start);
#if OA_TEST_BUILD
    // OA_DIRTY_AB: 旅程/运行结束汇总(退出帧的 A/B 已在上面跑完)
    if (state->quit && !s_ab_summarized && std::getenv("OA_DIRTY_AB")) {
        s_ab_summarized = true;
        std::printf("[dirtyab] summary frames=%llu set_mismatch=%llu "
                    "redraw_mismatch=%llu bake_mismatch=%llu dropped=%zu\n",
                    (unsigned long long)s_ab_frames,
                    (unsigned long long)s_ab_set_mm,
                    (unsigned long long)s_ab_redraw_mm,
                    (unsigned long long)s_ab_bake_mm,
                    state->rt->scene().dirty_ledger_dropped());
    }
#endif
    if (state->quit) return SDL_APP_SUCCESS;
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_Fail()
{
    SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "Error %s", SDL_GetError());
    return SDL_APP_FAILURE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
    (void)result;
    AppState* state = static_cast<AppState*>(appstate);

    if (state) {
        // Early SDL_AppInit returns (--help / usage errors / no project) leave
        // rt and oaRender null — SDL still calls AppQuit, so guard them.
        if (state->oaRender) state->oaRender->release_all();
        // detach + close the audio sink before tearing SDL down.
        // Drop the movie-audio hook first — it captures the
        // MediaPlayers instance that release() is about to destroy.
        if (state->rt) {
            state->rt->video().set_audio_output(nullptr);
            state->rt->video().set_audio_flush(nullptr);
            // OA_AUDIO_DIAG=1 — dump the per-source device-stream
            // delivery summary (water levels / dry hits / push gaps over the
            // run; no-op unless the diagnostics were enabled).
            state->rt->media_players().print_audio_diag("run");
            state->rt->media_players().release();
        }
        SDL_DestroyWindow(state->window);
        std::printf("[app] bye (frames=%llu)\n", (unsigned long long)state->frames);
        delete state;
    }
    oa::plat::shutdown(); // final platform teardown (wasm: last IDBFS persist attempt)
#ifndef OA_HARMONY_LIB
    // Host VintagePomelo owns the process-wide SDL instance. Calling
    // SDL_Quit here would tear down the XComponent window while the app
    // is still alive.
    SDL_Quit();
#endif
}

#ifdef OA_USE_SDL2
#ifndef OA_HARMONY_LIB
int main(int argc, char* argv[]) {
    SDL_SetMainReady();
    void* appstate = nullptr;
    SDL_AppResult r = SDL_AppInit(&appstate, argc, argv);
    if (r != SDL_APP_CONTINUE) {
        SDL_AppQuit(appstate, r);
        return r == SDL_APP_SUCCESS ? 0 : 1;
    }
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            r = SDL_AppEvent(appstate, &ev);
            if (r != SDL_APP_CONTINUE) goto oa_done;
        }
        r = SDL_AppIterate(appstate);
        if (r != SDL_APP_CONTINUE) break;
    }
oa_done:
    SDL_AppQuit(appstate, r);
    return r == SDL_APP_SUCCESS ? 0 : 1;
}
#endif
#endif



