#pragma once
// Lua-side file IO + host-service stubs (implementation: runtime_lua_file.cpp).
//
// The Lua host surface has ONE contract header (runtime_lua.h). This is a
// packaging split inside the same domain: the file-IO arm is a self-contained
// unit (its own userdata + metatable + io/os openers) that runtime_lua.cpp
// installs right after the standard-library block list, so the io/os policy
// and the interpreter wiring stay readable in separate TUs.
//
// Why it exists: Artemis frameworks persist their own setting/system files
// through plain Lua file IO (e.g. ハミダシ系 `system/system/system.lua`:
// `io.open(e:var("s.savepath").."/"..init.save_system, "wb")`). The original
// engine runs with the game directory as the process working directory, so
// those relative names land in a real writable directory. openartemis never
// has that: the project is a PFS archive and the process CWD is the host's.
// The unit below gives the script the SAME logical namespace the rest of the
// engine uses — game-relative path, save root as the writable half, project
// filesystem as the read-only half — so boot scripts that save-then-read
// work unchanged on every host.
struct lua_State;

namespace oa::runtime {

class LuaBridge;

/// Replace io.open/io.close/io.lines/io.type with the save-root-aware
/// implementation and install the host-service stubs (os.execute family,
/// io.popen/tmpfile/input/output) whose behaviour is documented in
/// runtime_lua.cpp's standard-library block-list section. No-op when
/// OA_LUA_STDLIB=stock (the A/B arm keeps upstream Lua io/os behaviour).
void install_lua_file_io(lua_State* L, LuaBridge* bridge);

} // namespace oa::runtime
