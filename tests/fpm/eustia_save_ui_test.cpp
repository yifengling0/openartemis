// Real legacy Artemis save/overwrite UI; all writes use an isolated store.
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <memory>
#include <map>
#include <string>
#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_save.h"

namespace {
int failures=0;
void check(bool ok,const char* text) {
    if(!ok){std::fprintf(stderr,"FAIL: %s\n",text);++failures;}
}
struct CountingStore : oa::runtime::SaveStore {
    oa::runtime::DirSaveStore disk;
    std::map<std::string,int> writes;
    explicit CountingStore(const std::string& root):disk(root){}
    bool write(const std::string& file,const std::vector<uint8_t>& bytes) override {
        if(!disk.write(file,bytes))return false;
        ++writes[file];return true;
    }
    std::optional<std::vector<uint8_t>> read(const std::string& file) const override {return disk.read(file);}
    bool remove(const std::string& file) override {return disk.remove(file);}
    bool exists(const std::string& file) const override {return disk.exists(file);}
};
void pump(oa::runtime::GameRuntime& rt, int frames) {
    for (int i=0;i<frames;++i) rt.tick(16,{});
}
void click(oa::runtime::GameRuntime& rt, int x, int y) {
    oa::runtime::FrameInput in;in.mouse_x=x-1;in.mouse_y=y;rt.tick(16,in);
    in.mouse_x=x;rt.tick(16,in);
    in.left_click_edge=true;in.left_down=true;rt.tick(16,in);
    in.left_click_edge=false;in.left_down=false;rt.tick(16,in);
}
void open_save(oa::runtime::GameRuntime& rt) {
    oa::runtime::FrameInput in;in.key_down_edges={113};in.keys_down={113};
    rt.tick(16,in); // The game's F2 binding invokes adv_save through input dispatch.
    pump(rt,300);
}
void dump(oa::runtime::GameRuntime& rt,const char* label) {
    auto& lua=rt.interpreter().lua_bridge();
    lua.call_function("probe_state",{});
    const auto v=rt.interpreter().variables().get("t.probe");
    std::printf("[%s] %s script=%s:%zu layers=%zu\n",label,
        v?v->to_string().c_str():"?",
        rt.interpreter().current_script()?rt.interpreter().current_script()->c_str():"-",
        rt.interpreter().current_line(),rt.scene().size());
    for(const auto& frame:rt.interpreter().call_stack())
        std::printf("  stack %s:%zu\n",frame.script.c_str(),frame.return_line);
}
std::string var(oa::runtime::GameRuntime& rt,const char* name) {
    const auto v=rt.interpreter().variables().get(name);
    return v?v->to_string():"";
}
bool dialog(oa::runtime::GameRuntime& rt,const char* name) {
    rt.interpreter().lua_bridge().call_function("probe_state",{});
    return var(rt,"t.dialog")==name && rt.scene().find("600.1.1.3.0") &&
        rt.scene().find("600.1.1.4.0") && rt.scene().is_effectively_visible("600.1.1.3.0") &&
        rt.scene().is_effectively_visible("600.1.1.4.0");
}
}
int main() {
    setvbuf(stdout,nullptr,_IONBF,0);
    const char* archive=std::getenv("OA_TEST_EUSTIA_PFS");
    if(!archive||!*archive)return 77;
    const auto tmp=std::filesystem::temp_directory_path()/
        ("oa_eustia_overwrite_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tmp);
    auto fs=std::make_shared<oa::fs::PhysFileSystem>(archive,false);
    auto store=std::make_shared<CountingStore>(tmp.string());
    oa::runtime::GameRuntime rt(fs);rt.set_save_store(store);
    std::map<std::string,std::pair<double,double>> sizes;
    rt.set_hit_providers([&](void*,const oa::render::Layer& l)->std::optional<std::pair<double,double>> {
        if(l.file.empty())return std::nullopt;
        if(sizes.count(l.file))return sizes.at(l.file);
        auto bytes=fs->read(l.file+".png");if(!bytes)bytes=fs->read(l.file);
        if(!bytes)bytes=store->read(l.file+".png");
        if(!bytes||bytes->size()<24)return std::nullopt;
        auto be=[&](size_t n) {return uint32_t((*bytes)[n])<<24|uint32_t((*bytes)[n+1])<<16|uint32_t((*bytes)[n+2])<<8|uint32_t((*bytes)[n+3]);};
        if((*bytes)[0]!=137)return std::nullopt;
        return sizes[l.file]={double(be(16)),double(be(20))};
    },{},nullptr);
    rt.open_project("windows");rt.boot_project();
    pump(rt,2000);
    auto& lua=rt.interpreter().lua_bridge();
    lua.run_code(R"(
function probe_state(e)
    e:tag{'var',name='t.dialog',data=tostring(dlg and dlg.name or '')}
    e:tag{'var',name='t.probe',data='btn='..tostring(btn and btn.name)..
        ', cursor='..tostring(btn_cursor)..', dlg='..tostring(dlg and dlg.name)..
        ', save='..tostring(sys.save and sys.save.com)..', name='..e:var('t.name')..
        ', yesno='..e:var('t.yesno')..', exists='..e:var('t.file')..
        ', slot='..tostring(sys.saveslot[1] and sys.saveslot[1].file)..
        ', conf='..tostring(sys.conf.dlg_save)..'/'..tostring(sys.conf.dlg_save2)}
end
function probe_start(e)
    sys.startflag=1
    title_start()
end
function probe_ready(e)
    e:tag{'var',name='t.ready',data=menu_check() and '0' or '1'}
end
)","save-ui-probe");
    dump(rt,"boot");lua.call_function("probe_start",{});
    // Finish the opening's timed presentation and wait at a genuine text
    // click. Calling adv_save before this point is correctly rejected by
    // the game's menu_check(). Do not bypass that game state in the probe.
    for(int i=0;i<10000;++i) {
        rt.tick(100,{});lua.call_function("probe_ready",{});
        if(var(rt,"t.ready")=="1")break;
    }
    check(var(rt,"t.ready")=="1","story reaches a menu-enabled click wait");
    dump(rt,"story");
    open_save(rt);dump(rt,"save");
    click(rt,290,118);pump(rt,300);dump(rt,"slot-new");
    check(dialog(rt,"save"),"empty slot opens the normal save dialog with both buttons");
    click(rt,530,340);pump(rt,300);dump(rt,"new-yes");
    const std::string path="savedata/AiyokunoEustia0001.dat";
    const auto original=store->read(path);
    check(original.has_value()&&store->writes[path]==1,"confirming a new slot writes one numbered save");
    open_save(rt);dump(rt,"reopen-save");
    click(rt,290,118);pump(rt,300);dump(rt,"overwrite-click");
    check(dialog(rt,"save2"),"stationary thumbnail click opens the overwrite dialog");
    check(store->writes[path]==1,"opening overwrite confirmation does not write the save");
    click(rt,750,340);pump(rt,300);dump(rt,"overwrite-no");
    check(store->writes[path]==1&&store->read(path)==original,"cancel preserves the existing save bytes");
    check(!rt.scene().find("600.1.1.3.0"),"cancel closes the dialog");
    click(rt,290,118);pump(rt,300);
    check(dialog(rt,"save2"),"the same slot can reopen overwrite confirmation after cancel");
    click(rt,530,340);pump(rt,300);dump(rt,"overwrite-yes");
    check(store->writes[path]==2&&store->exists(path),"confirm overwrites the same numbered save exactly once");
    check(!rt.scene().find("600.1.1.3.0"),"confirmed overwrite closes the dialog");
    // A moving drag must retain its original slot-move behavior.
    open_save(rt);
    oa::runtime::FrameInput in;in.mouse_x=289;in.mouse_y=118;rt.tick(16,in);
    in.mouse_x=290;rt.tick(16,in);
    in.left_click_edge=true;in.left_down=true;rt.tick(16,in);
    in.left_click_edge=false;in.mouse_x=440;rt.tick(16,in);
    in.left_down=false;rt.tick(16,in);pump(rt,300);
    lua.run_code("assert(sys.saveslot[1]==nil and sys.saveslot[2].file=='AiyokunoEustia0001')","thumbnail-move");
    check(!rt.scene().find("600.1.1.3.0")&&store->writes[path]==2,"moving a thumbnail does not confirm an overwrite");
    std::printf("eustia_save_ui: failures=%d\n",failures);
    std::filesystem::remove_all(tmp);
    return failures?1:0;
}
