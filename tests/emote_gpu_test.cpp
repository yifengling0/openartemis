#ifdef OA_USE_SDL2
#include <SDL2/SDL.h>
#else
#include <SDL3/SDL.h>
#endif
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <new>
#include <thread>
#include "core/render/renderer.h"
#include "core/emote/emote_file.h"

namespace {
// Driver shader/compiler workers have independent allocation lifetimes.
// Count the submitting thread so their startup cannot dominate this check.
thread_local bool counting = false;
thread_local size_t allocations = 0;
int failures = 0;
void check(bool ok, const char* msg) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", msg); ++failures; }
}
struct MemFs : oa::fs::IFileSystem {
    std::map<std::string, std::string> files;
    std::optional<std::vector<uint8_t>> read(std::string_view path) const override {
        auto it=files.find(std::string(path));
        if(it==files.end())return std::nullopt;
        return std::vector<uint8_t>(it->second.begin(),it->second.end());
    }
    const char* kind() const override { return "emote-test"; }
};
void env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if(value)setenv(name,value,1);else unsetenv(name);
#endif
}
}
void* operator new(std::size_t n) {
    if(counting) ++allocations;
    if(auto* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }

int main() {
#ifdef OA_USE_SDL2
    if(SDL_Init(SDL_INIT_VIDEO)!=0) return 77;
#else
    if(!SDL_Init(SDL_INIT_VIDEO)) return 77;
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef OA_USE_SDL2
    auto* win=SDL_CreateWindow("e-mote GPU regression",0,0,64,64,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
#else
    auto* win=SDL_CreateWindow("e-mote GPU regression",64,64,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
#endif
    if(!win){SDL_Quit();return 77;}
    auto fs=std::make_shared<MemFs>();
    fs->files["system.ini"]="[WINDOWS]\nWIDTH=64\nHEIGHT=64\nFPS=60\nCHARSET=UTF-8\nBOOT=main.iet\n";
    fs->files["main.iet"]="*main\n[stop]\n";
    oa::runtime::GameRuntime rt(fs); rt.open_project("windows"); rt.boot_project();
    {
        oa::render::RenderEngine renderer(fs.get(),&rt);
        if(!renderer.create_renderer(win,"gles")){SDL_DestroyWindow(win);SDL_Quit();return 77;}
        oa::emote::EmoteFile model;
        auto src=std::make_unique<oa::emote::EmoteSource>();
        src->textureWidth=src->textureHeight=2;src->rgbaDecoded=true;src->rgba.assign(16,255);
        oa::emote::EmoteIcon icon;icon.width=icon.height=2;src->icons.push_back(icon);
        model.sources.push_back(std::move(src));
        oa::emote::EmoteDrawPart part;part.source=part.icon=0;part.alpha=0.5;
        part.verts={{0,0,0,0},{64,0,1,0},{64,64,1,1},{0,0,0,0},{64,64,1,1},{0,64,0,1}};
        const auto key=oa::render::EmoteContent::canvas_key("em");
        auto& scene=const_cast<oa::render::Compositor&>(rt.scene());
        scene.create("em",{{"width","64"},{"height","64"}});scene.bind_emote_layer("em");
        auto sample=[&](double fade) {
            renderer.render_beigin();renderer.draw_scene(scene,fade);
            oa::media::Image img;
            check(renderer.snapshot_renderer(img),"stage readback");
            return img.rgba.empty()? -1000 : int(img.rgba[(size_t(img.h/2)*img.w+img.w/2)*4]);
        };
        check(renderer.emote_render_parts(key,model,{part},64,64),"GPU canvas submission");
        const int half=sample(1),quarter=sample(0.5);
        std::printf("alpha white=0.5: rgb=%d; parent=0.5: rgb=%d\n",half,quarter);
        check(std::abs(half-128)<=2,"part alpha applied once");
        check(std::abs(quarter-64)<=2,"parent opacity modulates premultiplied RGB and alpha");
        check(renderer.emote_render_parts(key,model,{part,part},64,64),"overlapping parts submission");
        check(std::abs(sample(1)-191)<=2,"overlapping half-alpha parts compose correctly");
        auto mesh=part;mesh.verts.clear();
        for(int i=0;i<64;++i)mesh.verts.insert(mesh.verts.end(),part.verts.begin(),part.verts.end());
        const std::vector<oa::emote::EmoteDrawPart> parts(64,mesh);
        for(int i=0;i<3;++i)renderer.emote_render_parts(key,model,parts,64,64);
        allocations=0;counting=true;
        for(int i=0;i<10;++i)renderer.emote_render_parts(key,model,parts,64,64);
        counting=false;
        std::printf("warm GPU: 64 parts x 384 vertices x 10 poses, submit-thread allocations=%zu\n",allocations);
        check(allocations<128,"warm submissions avoid per-part vertex and atlas allocations");
        check(renderer.emote_render_parts(key,model,{part},128,96),"resized canvas submission");
        const auto size=static_cast<const oa::render::ContentSizeSource&>(renderer).uploaded_size(key);
        check(size && size->first==128 && size->second==96,"canvas follows new dimensions");
        part.source=99;
        check(!renderer.emote_render_parts(key,model,{part},64,64),"invalid parts require fallback");
        const auto created=renderer.render_stats().textures_created;
        scene.remove("em");renderer.render_beigin();renderer.draw_scene(scene,1);
        check(renderer.texture_for_key(key)==nullptr,"deleted layer releases canvas");
        part.source=0;
        renderer.emote_render_parts(key,model,{part},64,64);
        check(renderer.render_stats().textures_created>=created+2,"deleted layer also releases atlas");
        scene.create("em",{{"width","64"},{"height","64"}});scene.bind_emote_layer("em");
        std::vector<uint8_t> cpu(64*64*4,255);
        check(renderer.upload_host_frame(key,64,64,cpu.data()),"GPU to CPU fallback upload");
        scene.remove("em");renderer.render_beigin();renderer.draw_scene(scene,1);
        check(renderer.texture_for_key(key)==nullptr,"deleted layer also releases CPU fallback texture");
        renderer.release_all();
    }
    env("OA_ASYNC_DECODE","1");
    {
        const auto png=oa::media::encode_png(64,64,std::vector<uint8_t>(64*64*4,255));
        fs->files["asset.png"]=std::string(png.begin(),png.end());
        oa::render::RenderEngine renderer(fs.get(),&rt);
        check(renderer.create_renderer(win,"gles"),"async renderer");
        auto& scene=const_cast<oa::render::Compositor&>(rt.scene());
        scene.create("g",{{"width","64"},{"height","64"},{"intermediate_render","1"}});
        scene.create("g.image",{{"file","asset.png"},{"width","64"},{"height","64"}});
        renderer.render_beigin();renderer.draw_scene(scene,1);
        bool completed=false;
        for(int i=0;i<100&&!completed;++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            completed=renderer.consume_async_completion();
        }
        check(completed,"async completion wakes a static scene without forced render_begin");
        check(renderer.async_decode_stats().completed==1,"async result drained at host tick");
        renderer.render_beigin();renderer.draw_scene(scene,1);
        oa::media::Image img;
        check(renderer.snapshot_renderer(img)&&!img.rgba.empty()&&
              img.rgba[(size_t(img.h/2)*img.w+img.w/2)*4]==255,
              "async arrival rebuilds a group baked before its image was ready");
        renderer.release_all();
    }
    env("OA_ASYNC_DECODE",nullptr);
    SDL_DestroyWindow(win);SDL_Quit();
    return failures?1:0;
}
