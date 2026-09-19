// Per-game compatibility manifest (core/fs/compat_config.h): flat JSON with
// both engine snake_case and art3m1s camelCase spellings, nested inputGate,
// unknown-key tolerance, and never-throwing malformed input.
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/fs/compat_config.h"
#include "core/fs/fs.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using oa::fs::CompatConfig;

// In-memory IFileSystem for the load_compat_config candidate order.
struct MemFs : oa::fs::IFileSystem {
    std::map<std::string, std::string> files;
    std::optional<std::vector<uint8_t>> read(std::string_view path) const override {
        const auto it = files.find(std::string(path));
        if (it == files.end()) return std::nullopt;
        return std::vector<uint8_t>(it->second.begin(), it->second.end());
    }
    const char* kind() const override { return "mem"; }
};

void test_art3m1s_spelling() {
    const char* text = R"({
  "name": "Magical Charming!",
  "engine": "art3m1s",
  "reportedOs": "ps4",
  "fontOverride": "font/sourcehansans-regular.otf",
  "inputGate": { "keyboard": false, "wheelToKeys": false }
})";
    const CompatConfig c = oa::fs::parse_compat_config(text, "art3m1s.json");
    check(c.source == "art3m1s.json", "source name kept");
    check(c.has_platform && c.platform == "ps4", "reportedOs -> platform");
    check(c.has_font_override && c.font_override == "font/sourcehansans-regular.otf",
          "fontOverride");
    check(c.has_input_gate, "inputGate seen");
    check(!c.gate_keyboard, "inputGate.keyboard=false");
    check(!c.gate_wheel, "inputGate.wheelToKeys=false");
    check(!c.has_charset, "charset not present");
}

void test_engine_spelling_and_unknown_keys() {
    const char* text = R"({
  "platform": "windows",
  "charset": "Shift_JIS",
  "font_override": "font/custom.ttf",
  "translationPatchPath": "patch/zh.json",
  "vndbId": "v12851",
  "unknownNested": { "a": { "b": [1, 2, 3] } },
  "list": ["x", "y"]
})";
    const CompatConfig c = oa::fs::parse_compat_config(text, "oa_compat.json");
    check(c.has_platform && c.platform == "windows", "platform");
    check(c.has_charset && c.charset == "Shift_JIS", "charset");
    check(c.font_override == "font/custom.ttf", "font_override");
    check(c.gate_keyboard && c.gate_wheel, "no inputGate -> inputs stay enabled");
    check(!c.has_input_gate, "has_input_gate false without inputGate");

    const CompatConfig alias = oa::fs::parse_compat_config(
        "{\"reported_os\": \"android\"}", "alias.json");
    check(alias.has_platform && alias.platform == "android", "reported_os alias");
}

void test_malformed_never_throws() {
    for (const char* text : {"", "{", "{\"a\"", "}{", "[1,2]", "not json at all",
                             "{\"platform\": }"}) {
        const CompatConfig c = oa::fs::parse_compat_config(text, "broken.json");
        check(!c.has_platform || !c.platform.empty(), "malformed never yields a value");
    }
    const CompatConfig partial = oa::fs::parse_compat_config(
        "{\"platform\": \"android\"", "truncated.json");
    check(partial.has_platform && partial.platform == "android",
          "truncated-but-readable key still applies");
}

void test_candidate_order() {
    MemFs fs;
    fs.files["art3m1s.json"] = "{\"reportedOs\":\"ps4\"}";
    const CompatConfig only_host = oa::fs::load_compat_config(fs);
    check(only_host.source == "art3m1s.json" && only_host.platform == "ps4",
          "host manifest read when no engine manifest");

    fs.files["oa_compat.json"] = "{\"platform\":\"android\"}";
    const CompatConfig engine_wins = oa::fs::load_compat_config(fs);
    check(engine_wins.source == "oa_compat.json" && engine_wins.platform == "android",
          "oa_compat.json wins over art3m1s.json");

    MemFs empty;
    const CompatConfig none = oa::fs::load_compat_config(empty);
    check(none.source.empty() && !none.any(), "no manifest -> empty config");
}

}  // namespace

int main() {
    test_art3m1s_spelling();
    test_engine_spelling_and_unknown_keys();
    test_malformed_never_throws();
    test_candidate_order();
    if (failures) {
        std::fprintf(stderr, "compat_config_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("compat_config_test: ok\n");
    return 0;
}
