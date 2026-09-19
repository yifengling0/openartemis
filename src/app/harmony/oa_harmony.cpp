/*
 * HarmonyOS engine_loader entry for OpenArtemis.
 * Exports runner_main / requestShutdown / cleanupSDL like krkrsdl_harmony.cpp.
 */

#include <SDL3/SDL.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#ifdef __OHOS__
#include <hilog/log.h>
#define OA_ERR(fmt, ...) \
    OH_LOG_Print(LOG_APP, LOG_ERROR, 0xFF00, "openartemis", fmt, ##__VA_ARGS__)
#define OA_INFO(fmt, ...) \
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "openartemis", fmt, ##__VA_ARGS__)
#else
#define OA_ERR(fmt, ...) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[openartemis] " fmt, ##__VA_ARGS__)
#define OA_INFO(fmt, ...) SDL_Log("[openartemis] " fmt, ##__VA_ARGS__)
#endif

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]);
SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event);
SDL_AppResult SDL_AppIterate(void* appstate);
void SDL_AppQuit(void* appstate, SDL_AppResult result);

static std::atomic<int> g_oa_shutdown{0};
static std::atomic<int> g_oa_running{0};
static char g_oa_last_error[2048] = "";

extern "C" {

__attribute__((visibility("default")))
int runner_main(int argc, char** argv)
{
    g_oa_shutdown.store(0);
    g_oa_running.store(1);
    g_oa_last_error[0] = '\0';

    if (argc < 2 || !argv[1]) {
        OA_ERR("Usage: openartemis <game_path>");
        snprintf(g_oa_last_error, sizeof(g_oa_last_error), "No game path provided");
        g_oa_running.store(0);
        return -1;
    }

    std::string game = argv[1];
    std::string pfs = game;
    const std::string root_pfs = game + "/root.pfs";
    FILE* probe = fopen(root_pfs.c_str(), "rb");
    if (probe) {
        fclose(probe);
        pfs = root_pfs;
    }
    OA_INFO("runner_main game=%{public}s pfs=%{public}s", game.c_str(), pfs.c_str());

    std::vector<std::string> args_store = {
        "openartemis", pfs, "--renderer", "gles", "--platform", "android"
    };
    std::vector<char*> args;
    for (auto& s : args_store) args.push_back(s.data());

    SDL_SetMainReady();
    void* appstate = nullptr;
    SDL_AppResult r = SDL_APP_FAILURE;
    try {
        r = SDL_AppInit(&appstate, (int)args.size(), args.data());
    } catch (const std::exception& e) {
        OA_ERR("SDL_AppInit exception: %{public}s", e.what());
        snprintf(g_oa_last_error, sizeof(g_oa_last_error), "SDL_AppInit exception: %s", e.what());
        SDL_AppQuit(appstate, SDL_APP_FAILURE);
        g_oa_running.store(0);
        return -1;
    } catch (...) {
        OA_ERR("SDL_AppInit unknown exception");
        snprintf(g_oa_last_error, sizeof(g_oa_last_error), "SDL_AppInit unknown exception");
        SDL_AppQuit(appstate, SDL_APP_FAILURE);
        g_oa_running.store(0);
        return -1;
    }
    if (r != SDL_APP_CONTINUE) {
        const char* se = SDL_GetError();
        if (se && *se) {
            snprintf(g_oa_last_error, sizeof(g_oa_last_error), "%s", se);
        } else if (!g_oa_last_error[0]) {
            snprintf(g_oa_last_error, sizeof(g_oa_last_error), "SDL_AppInit failed");
        }
        OA_ERR("SDL_AppInit failed: %{public}s", g_oa_last_error);
        SDL_AppQuit(appstate, r);
        g_oa_running.store(0);
        return r == SDL_APP_SUCCESS ? 0 : -1;
    }

    try {
        while (!g_oa_shutdown.load()) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                r = SDL_AppEvent(appstate, &ev);
                if (r != SDL_APP_CONTINUE) goto done;
            }
            r = SDL_AppIterate(appstate);
            if (r != SDL_APP_CONTINUE) break;
            // KR2 krkrsdl_harmony.cpp: yield only. SwapInterval(1) +
            // SwapWindow inside iterate waits one vblank (panel Hz).
            SDL_Delay(1);
        }
    } catch (const std::exception& e) {
        OA_ERR("runtime exception: %{public}s", e.what());
        snprintf(g_oa_last_error, sizeof(g_oa_last_error), "%s", e.what());
        r = SDL_APP_FAILURE;
    } catch (...) {
        OA_ERR("runtime unknown exception");
        snprintf(g_oa_last_error, sizeof(g_oa_last_error), "OpenArtemis runtime exception");
        r = SDL_APP_FAILURE;
    }
done:
    SDL_AppQuit(appstate, r);
    g_oa_running.store(0);
    return r == SDL_APP_FAILURE ? -1 : 0;
}

__attribute__((visibility("default")))
void requestShutdown(void)
{
    g_oa_shutdown.store(1);
}

__attribute__((visibility("default")))
void cleanupSDL(void)
{
    g_oa_shutdown.store(1);
}

__attribute__((visibility("default")))
const char* getLastRubyError(void)
{
    return g_oa_last_error[0] ? g_oa_last_error : nullptr;
}

__attribute__((visibility("default")))
const char* get_last_ruby_error(void)
{
    return getLastRubyError();
}

} // extern "C"
