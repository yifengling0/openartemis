// Queued-call barrier: Lua batches queued with e:enqueueTag run the callee
// FIRST and hold the caller's continuation until that frame returns.
//
// This is the boot shape of ハミダシ系 枫笛 games: `system/first.iet`'s
// initLua2() enqueues `[call file=system/msg.iet]` (plus more calls) and then
// `[jump label=game_start]`; game_start lives in the CALLER. Before the
// barrier the drain switched the current script to the callee and the jump
// failed with "jump: label not found: game_start", so the title never came up.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/runtime/runtime_iet.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using oa::runtime::CallbackResult;
using oa::runtime::Event;
using oa::runtime::ExecutionResult;
using oa::runtime::Interpreter;
using oa::runtime::Value;

const char* kCaller = R"(
*top
[stop]
*after_calllua
[var name="t.continuation" data="1"]
[stop]
*game_start
[var name="t.game_start" data="1"]
[stop]
*tail
[stop]
)";

const char* kCallee = R"(
*last
[var name="t.callee" data="1"]
[return]
)";

bool var_is(Interpreter& it, const char* name, const char* want) {
    const auto v = it.variables().get(name);
    if (!v) return false;
    return v->as_int().value_or(-1) == std::atoi(want);
}

void test_queued_call_defers_caller_continuation() {
    Interpreter it;
    it.hooks().file_loader = [](const std::string& name)
        -> std::optional<std::vector<uint8_t>> {
        const char* text = nullptr;
        if (name == "sys/callee.iet") text = kCallee;
        if (!text) return std::nullopt;
        return std::vector<uint8_t>(text, text + std::char_traits<char>::length(text));
    };
    it.set_callback([](const Event& e) {
        return e.kind == Event::Kind::Wait_ ? CallbackResult::Pause
                                            : CallbackResult::Continue;
    });
    it.load_script("sys/caller.iet", kCaller);
    it.start("sys/caller.iet", "top");

    // The Lua batch: callee first, then the caller-relative jump.
    it.enqueue_tag("call", {{"file", "sys/callee.iet"}, {"label", "last"}});
    it.enqueue_tag("jump", {{"label", "game_start"}});

    const ExecutionResult r = it.run();
    check(r == ExecutionResult::Wait, "drain parks on the caller's [stop]");
    check(var_is(it, "t.callee", "1"), "callee body ran");
    check(var_is(it, "t.game_start", "1"), "deferred jump resolved in the caller");
    check(!var_is(it, "t.continuation", "1"),
          "continuation row after the call was not executed before the jump");
}

void test_immediate_rows_keep_their_prefix() {
    // e:tag (immediate) rows queued BEFORE a queued call still run first: the
    // callee barrier only holds the rows behind them.
    Interpreter it;
    it.hooks().file_loader = [](const std::string& name)
        -> std::optional<std::vector<uint8_t>> {
        const char* text = nullptr;
        if (name == "sys/callee.iet") text = kCallee;
        if (!text) return std::nullopt;
        return std::vector<uint8_t>(text, text + std::char_traits<char>::length(text));
    };
    it.set_callback([](const Event& e) {
        return e.kind == Event::Kind::Wait_ ? CallbackResult::Pause
                                            : CallbackResult::Continue;
    });
    it.load_script("sys/caller.iet", kCaller);
    it.start("sys/caller.iet", "top");

    it.enqueue_tag_immediate("var", {{"name", "t.immediate"}, {"data", "1"}});
    it.enqueue_tag("call", {{"file", "sys/callee.iet"}, {"label", "last"}});
    it.enqueue_tag("jump", {{"label", "game_start"}});

    const ExecutionResult r = it.run();
    check(r == ExecutionResult::Wait, "drain parks on the caller's [stop]");
    check(var_is(it, "t.immediate", "1"), "immediate row ran before the call");
    check(var_is(it, "t.callee", "1"), "callee body ran");
    check(var_is(it, "t.game_start", "1"), "deferred jump resolved in the caller");
}

void test_immediate_drain_keeps_click_continuation_parked() {
    Interpreter it;
    it.set_callback([](const Event& e) {
        return e.kind == Event::Kind::Wait_ ? CallbackResult::Pause
                                           : CallbackResult::Continue;
    });
    it.load_script("main.iet", kCaller);
    it.start("main.iet", "top");
    it.enqueue_tag("@", {});
    it.enqueue_tag("var", {{"name","t.continuation"},{"data","1"}});
    check(it.run_queued()==ExecutionResult::Wait,"queued click parks the continuation");
    it.enqueue_tag_immediate("var", {{"name","t.ui"},{"data","1"}});
    check(it.run_queued(true)==ExecutionResult::Completed,"fresh immediate UI command runs while parked");
    check(var_is(it,"t.ui","1"),"the immediate UI command took effect");
    check(!var_is(it,"t.continuation","1"),"immediate drain does not run the post-click continuation");
    check(it.last_wait_from_queue(),"immediate drain preserves queue wait bookkeeping");
    check(it.has_queued_tags()&&!it.has_immediate_tags(),"only the deferred continuation remains");
    it.run_queued();
    check(var_is(it,"t.continuation","1"),"normal drain still runs the deferred continuation");
}

}  // namespace

int main() {
    test_queued_call_defers_caller_continuation();
    test_immediate_rows_keep_their_prefix();
    test_immediate_drain_keeps_click_continuation_parked();
    if (failures) {
        std::fprintf(stderr, "tag_queue_barrier_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("tag_queue_barrier_test: ok\n");
    return 0;
}
