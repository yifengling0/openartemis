// Asset-free playback contracts. Synthetic variable tracks isolate timing
// and crossfades from PSB parsing and the rasterizer.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "core/emote/emote_player.h"

using namespace oa::emote;
namespace {
int failures = 0;
void check(bool ok, const char* msg) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", msg); ++failures; }
}
bool near(double a, double b) { return std::fabs(a-b) < 1e-8; }
void env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}
void track(EmotePlayer& player, const char* name, const char* var, double value) {
    auto& model = const_cast<EmoteFile&>(player.file());
    const_cast<std::set<std::string>&>(player.known_labels()).insert(var);
    EmoteTimeline tl;
    tl.label = name; tl.diff = 1; tl.loopBegin = 0; tl.loopEnd = 600;
    tl.variables.push_back({var, {{0, 2, true, 0, value}}});
    model.timelines.push_back(std::move(tl));
}
}

int main() {
    for (const char* fps : {"2", "30", "60"}) {
        env("OA_EMOTE_FPS", fps);
        EmotePlayer p;
        p.set_external_pose(true);
        track(p, "clock", "drift", 0);
        p.play_timeline("clock"); p.advance_ms(1000);
        check(near(p.foreground_slots().front().t, 60), "pose cadence cannot slow authored clock");
        p.progress(0.5);
        check(near(p.foreground_slots().front().t, 60.5), "Lua progress uses authored frames");
    }
    env("OA_EMOTE_FPS", nullptr);
    EmotePlayer p; p.set_external_pose(true);
    p.set_variable("arm_type", 1, 15, 0);
    check(near(p.get_variable("arm_type"), 0), "arm change starts from displayed value");
    p.progress(7.5);
    check(near(p.get_variable("arm_type"), 0.5), "15-frame arm transition has a midpoint");
    p.progress(7.5);
    check(near(p.get_variable("arm_type"), 1), "arm transition reaches target");
    p.set_variable("arm_type", 0, 20);
    p.progress(5);
    p.set_variable("arm_type", 1, 10);
    p.progress(5);
    check(near(p.get_variable("arm_type"), 0.875), "retarget starts from current interpolated value");
    p.step();
    check(near(p.get_variable("arm_type"), 1), "step settles variable tween without foreground");
    p.set_variable("arm_type", 0, 20); p.progress(3);
    p.set_variable("arm_type", 0.25); p.progress(30);
    check(near(p.get_variable("arm_type"), 0.25), "immediate write cancels previous tween");
    p.progress(std::numeric_limits<double>::quiet_NaN());
    check(near(p.get_variable("arm_type"), 0.25), "nonfinite clock is rejected");

    track(p, "old_idle", "drift", 10);
    track(p, "new_idle", "drift", -10);
    p.fade_in_timeline("old_idle");
    p.fade_out_timeline("old_idle", 15);
    p.fade_in_timeline("new_idle", 15);
    check(p.is_timeline_playing("old_idle"), "fade out keeps old idle alive");
    p.progress(7.5);
    check(near(p.get_variable("drift"), 0), "old and new idle overlap at half weight");
    p.progress(0); p.progress(0);
    check(near(p.get_variable("drift"), 0), "compose does not accumulate idle deltas");
    p.progress(7.5);
    check(near(p.get_variable("drift"), -10), "crossfade reaches new idle");
    check(!p.is_timeline_playing("old_idle") && p.is_timeline_playing("new_idle"),
          "old idle stops only after fade completion");
    p.stop_timeline("");
    check(!p.is_timeline_playing(""), "empty timeline label stops all timelines");

    EmotePlayer fading; fading.set_external_pose(true);
    track(fading,"base","axis",2);track(fading,"action","axis",10);
    fading.play_timeline("base");fading.pass();
    fading.play_timeline("action");fading.fade_out_timeline("action",10);
    fading.progress(5);
    check(near(fading.get_variable("axis"),6),"absolute timeline fades toward its underlying pose");
    fading.progress(0);
    check(near(fading.get_variable("axis"),6),"absolute fade does not accumulate on repeated compose");
    fading.progress(5);
    check(near(fading.get_variable("axis"),2)&&!fading.is_timeline_playing("action"),
          "absolute fade applies the endpoint before stopping");

    EmotePlayer blink; blink.set_external_pose(true);
    auto& model = const_cast<EmoteFile&>(blink.file());
    model.eyeControls.push_back({"eye", 0, 1, 4, 10, 10});
    track(blink, "neutral", "eye", 0);
    blink.play_timeline("neutral");
    blink.progress(12);
    check(near(blink.get_variable("eye"), 1), "eye controller closes at blink midpoint");
    blink.progress(0.25);
    check(near(blink.get_variable("eye"), 1), "eye controller holds the fully closed pose");
    blink.progress(1.75);
    check(near(blink.get_variable("eye"), 0), "eye controller restores underlying pose");
    blink.set_variable("eye", 0.4); blink.progress(12);
    check(near(blink.get_variable("eye"), 0.4), "explicit eye expression owns the variable");
    std::printf("emote semantics: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
