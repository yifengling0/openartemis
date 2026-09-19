#include "platform/Platform.h"

#include <filesystem>
#include <string>

#ifdef __OHOS__
#include <atomic>
#include <SDL.h>
#endif

namespace oa::plat {

std::string default_save_root(const std::string& data_path, bool data_is_dir) {
    std::string save_root = (data_is_dir ? std::filesystem::path(data_path)
                                         : std::filesystem::path(data_path).parent_path())
                                .string();
    if (save_root.empty()) save_root = ".";
    std::filesystem::path p(save_root);
    p /= "savedata";
    return p.string();
}

#ifdef __OHOS__
namespace {
std::atomic<bool> s_resume{false};

int lifecycle_watch(void*, SDL_Event* ev) {
    if (ev->type == SDL_APP_DIDENTERBACKGROUND ||
        ev->type == SDL_APP_DIDENTERFOREGROUND) {
        s_resume.store(true);
    }
    return 1;
}
}

void init() {
    SDL_AddEventWatch(lifecycle_watch, nullptr);
}

void shutdown() {
    SDL_DelEventWatch(lifecycle_watch, nullptr);
}

bool take_lifecycle_resume() {
    return s_resume.exchange(false);
}
#else
void init() {}
void shutdown() {}
bool take_lifecycle_resume() { return false; }
#endif

} // namespace oa::plat
