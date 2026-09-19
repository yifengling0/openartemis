#pragma once
// Per-game compatibility manifest (the engine side of the host round-tripping
// art3m1s-style manifests).
//
// Read from the project root at load time, from the project filesystem, in
// this order: `oa_compat.json` first, then `art3m1s.json` (the ecosystem name
// used by the Flutter host). A manifest is a flat JSON object; unknown keys
// are ignored, so a host manifest written for another host still loads.
//
// Keys the ENGINE acts on (snake_case and the art3m1s camelCase spelling are
// both accepted):
//   platform / reportedOs / reported_os   system.ini section + script `os`
//   charset / scriptCharset               script decoding override
//   font_override / fontOverride          game-relative TTF/OTF used for
//                                         every script font (missing-glyph
//                                         and 汉化 font cases)
//   inputGate.keyboard                    false = keyboard/pad input ignored
//   inputGate.wheelToKeys                 false = wheel->HUP/HDW mapping off
//
// Keys a host may write that this engine accepts and IGNORES today
// (translation patch path, Eluna switch, environment patch, VNDB metadata):
// they do not change engine behaviour, and the manifest reader never fails on
// them. Precedence for every engine-visible field is CLI/env > manifest >
// project default, and an absent manifest keeps today's behaviour exactly.
#include <string>

namespace oa::fs {

class IFileSystem;

struct CompatConfig {
    /// Manifest file the values came from ("" = none found).
    std::string source;

    std::string platform;       // reported OS / system.ini section
    std::string charset;        // script charset override
    std::string font_override;  // game-relative font path
    bool gate_keyboard = true;  // false: drop keyboard input
    bool gate_wheel = true;     // false: drop the wheel->key mapping

    bool has_platform = false;
    bool has_charset = false;
    bool has_font_override = false;
    bool has_input_gate = false;

    bool any() const {
        return has_platform || has_charset || has_font_override || has_input_gate;
    }
};

/// Parse the per-game manifest out of `fs` ("" when the project ships none).
CompatConfig load_compat_config(const IFileSystem& fs);

/// Parse one manifest text (exposed for tests; `name` only feeds `source`).
CompatConfig parse_compat_config(const std::string& text, const std::string& name);

} // namespace oa::fs
