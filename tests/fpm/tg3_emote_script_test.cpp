// Executes the supplied game's unmodified e-mote.lua against the real Lua
// bridge/runtime and synthetic variable tracks. This verifies script/API
// semantics even when the optional PSB volume is unavailable, not model pixels.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime.h"

using namespace oa::emote;
namespace {
int failures=0;
void check(bool ok,const char* msg) {
    if(!ok){std::fprintf(stderr,"FAIL: %s\n",msg);++failures;}
}
bool near(double a,double b){return std::fabs(a-b)<1e-8;}
void track(EmotePlayer& p,const char* name,const char* var,double value) {
    auto& file=const_cast<EmoteFile&>(p.file());
    const_cast<std::set<std::string>&>(p.known_labels()).insert(var);
    EmoteTimeline tl;tl.label=name;tl.diff=1;tl.loopBegin=0;tl.loopEnd=600;
    tl.variables.push_back({var,{{0,2,true,0,value}}});
    file.timelines.push_back(std::move(tl));
}
}
int main() {
    const char* archive=std::getenv("OA_TEST_TG3_SCRIPT_PFS");
    if(!archive||!*archive)return 77;
    auto fs=std::make_shared<oa::fs::PhysFileSystem>(archive,false);
    const auto bytes=fs->read("system/image/e-mote.lua");
    if(!bytes){std::fprintf(stderr,"missing real e-mote.lua\n");return 1;}
    std::string code(bytes->begin(),bytes->end());
    const auto parser=fs->read("system/adv/parse.lua");
    const auto helpers=fs->read("system/adv/func.lua");
    if(!parser||!helpers)return 1;
    const std::string functions(helpers->begin(),helpers->end());
    const auto explode_begin=functions.find("function explode(delim, str)");
    const auto explode_end=functions.find("----------------------------------------",explode_begin);
    if(explode_begin==std::string::npos||explode_end==std::string::npos)return 1;
    code+='\n'+functions.substr(explode_begin,explode_end-explode_begin);
    code+='\n'+std::string(parser->begin(),parser->end());
    // The idle-switch branch is used verbatim, including its frame conversion.
    const auto begin=code.find("local alw = p.always or init.emote_autoexec");
    const auto end=code.find("-- その他",begin);
    if(begin==std::string::npos||end==std::string::npos)return 1;
    const std::string idle_branch=code.substr(begin,end-begin);
    oa::runtime::GameRuntime rt(fs);rt.open_project("windows");rt.boot_project();
    auto player=std::make_shared<EmotePlayer>();player->set_external_pose(true);
    track(*player,"old_idle","drift",10);
    track(*player,"new_idle","drift",-10);
    track(*player,"gesture","gesture",1);
    auto& layers=const_cast<std::map<std::string,oa::runtime::GameRuntime::EmoteLayerState>&>(rt.emote_layers());
    layers["fixture"].player=player;
    auto& lua=rt.interpreter().lua_bridge();
    if(const auto config=fs->read("system/table/list_windows.tbl")) {
        lua.run_code(std::string(config->begin(),config->end()),"real-tg3-config");
        lua.run_code("assert(init.emote_shakeoverwt ~= 'on')","tg3-optional-shake-config");
    } else { std::fprintf(stderr,"missing TG3 Windows config\n"); return 1; }
    code+=R"(
function tg3_prepare(e)
    _G.e=e
    init={fg_fade=250,emote_lipsync='on',emote_autoexec='old_idle'}
    game={os='windows',fgext='.psb'}
    flg={} scr={img={fg={}}}
    csv={emote={base={demo={'say_0'}}}}
    tn=tonumber
    getSkip=function() return false end
    em=e:getEmoteLayer{id='fixture'}
    ex.emsave('demo',em)
    assert(type(em:isTimelinePlaying('absent'))=='boolean')
    assert(not em:isTimelinePlaying('absent'))
    ex.param(em,{arm_type=1,head_lr=1,frame=250})
end
function tg3_half(e) em:progress(7.5) end
function tg3_reset(e) ex.reset(em,{frame=250}) end
function tg3_action(e) emote.action{ch='demo',act='gesture'} end
function tg3_idle(e)
    local p={always='new_idle',time=250}
    local d={always='old_idle'}
)";
    code+=idle_branch;
    code+=R"(
end
function tg3_lip(e)
    ex.lipsave('fg',em,{ch='demo'})
    flg.lipsync={say={time=e:now(),p={[0]=0.75}}}
    emote.vsync()
end
function tg3_lipstop(e) emote.lipstop{ch='say'} end
function tg3_stop(e) ex.stop(em,'new_idle') end
function tg3_csv_lip(e)
    game.soundext='.ogg'
    btn={name='test'}
    isFile=function(path) return e:isFileExists(path) end
    ex.lipsave('fg',em,{ch='demo'})
    local path='sound/vo/jsa/fem_jsa_30001.ogg'
    emote.readlipcsv(path,'say')
    assert(flg.lipsync and flg.lipsync.say,'real voice CSV must be mounted')
    local p=flg.lipsync.say
    assert(tonumber(p.p[9])==0.3,'real CSV parser preserves fractional lip amplitude')
    p.time=e:now()-8.5*ex.frametime
    emote.vsync()
end
)";
    try {lua.run_code(code,"real-tg3-e-mote");}
    catch(const std::exception& e){std::fprintf(stderr,"Lua load: %s\n",e.what());return 1;}
    auto call=[&](const char* name){check(lua.call_function(name,{}),name);};
    call("tg3_prepare");
    check(near(player->get_variable("arm_type"),0),"real ex.param respects arm fade duration");
    call("tg3_half");
    check(near(player->get_variable("arm_type"),0.5),"real 250ms parameter conversion = 15 authored frames");
    check(near(player->get_variable("head_LR"),0.5),"real ex.param normalizes lowercase axis name");
    call("tg3_half");call("tg3_reset");call("tg3_half");
    check(near(player->get_variable("head_LR"),0.5),"real ex.reset interpolates head reset");
    player->fade_in_timeline("old_idle");call("tg3_idle");call("tg3_half");
    check(near(player->get_variable("drift"),0),"real fg always branch crossfades both loops");
    call("tg3_half");
    check(!player->is_timeline_playing("old_idle")&&player->is_timeline_playing("new_idle"),
          "real fg always branch stops old loop after fade");
    call("tg3_action");
    check(player->foreground_slots().size()==1&&player->foreground_slots()[0].flags==1,
          "real action path passes PARALLEL flag");
    call("tg3_lip");
    check(near(player->get_variable("face_talk"),0.75),"real vsync applies CSV lip value");
    check(layers["fixture"].host_clock,"real vsync selects game-driven clock");
    call("tg3_lipstop");
    check(near(player->get_variable("face_talk"),0),"real lipstop closes mouth");
    call("tg3_csv_lip");
    check(near(player->get_variable("face_talk"),0.3),"real voice CSV drives real vsync at authored sample index");
    call("tg3_stop");
    check(!player->is_timeline_playing("new_idle"),"real ex.stop queries and stops named timeline");
    std::printf("TG3 actual Lua framework + synthetic tracks: %s (%zu-byte script)\n",
                failures?"FAIL":"PASS",bytes->size());
    return failures?1:0;
}
