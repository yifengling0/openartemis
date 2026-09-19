#include "platform/Platform.h"

#include "core/util/path_utf8.h"

#include <filesystem>
#include <string>

#ifdef __OHOS__
#include <atomic>
#include <SDL.h>
#endif

namespace oa::plat {

std::string default_save_root(const std::string& data_path, bool data_is_dir) {
    // UTF-8 -> native (see core/util/path_utf8.h): the OHOS data path can
    // carry CJK in the app's sandbox name.
    const std::filesystem::path native = oa::util::native_path_from_utf8(data_path);
    std::string save_root =
        oa::util::path_to_utf8(data_is_dir ? native : native.parent_path());
    if (save_root.empty()) save_root = ".";
    std::filesystem::path p(oa::util::native_path_from_utf8(save_root));
    p /= "savedata";
    return oa::util::path_to_utf8(p);
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
