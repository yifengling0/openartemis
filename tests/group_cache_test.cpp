// Asset-free pixel regressions for partial group invalidation and target reuse.
#ifdef OA_USE_SDL2
#include <SDL2/SDL.h>
#else
#include <SDL3/SDL.h>
#endif
#include <cstdio>
#include <map>
#include "core/render/renderer.h"
#include "core/render/render_internal.h"
namespace {
int failures=0;
void check(bool ok,const char* msg){if(!ok){std::fprintf(stderr,"FAIL: %s\n",msg);++failures;}}
struct MemFs : oa::fs::IFileSystem {
    std::map<std::string,std::string> files;
    std::optional<std::vector<uint8_t>> read(std::string_view p) const override {
        auto i=files.find(std::string(p));if(i==files.end())return std::nullopt;
        return std::vector<uint8_t>(i->second.begin(),i->second.end());
    }
    const char* kind() const override{return "group-test";}
};
}
int main(int argc,char** argv){
#ifdef OA_USE_SDL2
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 77;
#else
    if(!SDL_Init(SDL_INIT_VIDEO))return 77;
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef OA_USE_SDL2
    auto* win=SDL_CreateWindow("group cache regression",0,0,64,64,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
#else
    auto* win=SDL_CreateWindow("group cache regression",64,64,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
#endif
    if(!win){SDL_Quit();return 77;}
    auto fs=std::make_shared<MemFs>();
    fs->files["system.ini"]="[WINDOWS]\nWIDTH=64\nHEIGHT=64\nCHARSET=UTF-8\nBOOT=main.iet\n";
    fs->files["main.iet"]="*main\n[stop]\n";
    oa::runtime::GameRuntime rt(fs);rt.open_project("windows");rt.boot_project();
    {
        oa::render::RenderEngine renderer(fs.get(),&rt);renderer.stage_w_=renderer.stage_h_=64;
        if(!renderer.create_renderer(win,argc>1?argv[1]:"gles")){SDL_DestroyWindow(win);SDL_Quit();return 77;}
        auto& sc=const_cast<oa::render::Compositor&>(rt.scene());
        sc.create("a",{});
        sc.create("a.g",{{"intermediate_render","1"},{"alpha","128"}});
        sc.create("a.g.c",{{"color","#ffff0000"},{"width","20"},{"height","20"}});
        sc.create("b",{{"left","32"},{"intermediate_render","1"},{"alpha","128"}});
        sc.create("b.c",{{"color","#ff00ff00"},{"width","20"},{"height","20"}});
        auto draw=[&] {
            renderer.render_clear_frame_cache();renderer.render_beigin();renderer.draw_scene(sc,1);
            oa::media::Image image;check(renderer.snapshot_renderer(image),"readback");return image.rgba;
        };
        int step=0;
        auto equal_full=[&] {
            ++step;
            const auto cached=draw();renderer.render_clear_tex_cache();
            const auto full=draw();
            if(cached!=full) {
                size_t n=0; for(size_t i=0;i<cached.size()&&i<full.size();++i)if(cached[i]!=full[i]){
                    if(n++<8)std::fprintf(stderr,"step=%d pixel=(%zu,%zu) c=%zu cached=%u full=%u\n",step,(i/4)%64,(i/4)/64,i%4,cached[i],full[i]);
                }
                std::fprintf(stderr,"mismatch step=%d bytes=%zu\n",step,n);
            }
            check(cached==full,"partial invalidation equals complete rebuild pixel for pixel");
        };
        const auto first=draw();
        std::fprintf(stderr,"fixture rgb=%d,%d,%d\n",first[(5*64+5)*4],first[(5*64+5)*4+1],first[(5*64+5)*4+2]);
        check(first.size()==64*64*4 && first[(5*64+5)*4]>100,"fixture draws translucent red group");
        auto before=renderer.group_stats().premul;draw();
        check(renderer.group_stats().premul==before,"static groups reuse bakes");
        sc.apply_lytween({{"id","a.g.c"},{"param","left"},{"from","0"},{"to","10"},{"time","100"}},0);
        sc.advance_tweens(50);before=renderer.group_stats().premul;draw();
        check(renderer.group_stats().premul==before+1,"child tween rebuilds only its group");equal_full();
        // Changes above a cached group are outside its subtree revision.
        sc.set_props("a",{{"left","3"},{"alpha","160"}});equal_full();
        sc.set_props("a.g",{{"clip","0,0,12,12"}});equal_full();
        sc.set_props("a.g",{{"grayscale","1"}});equal_full();
        sc.set_props("a.g",{{"grayscale","0"}});equal_full();
        sc.create("a.g.n",{{"intermediate_render","1"},{"alpha","128"}});
        sc.create("a.g.n.c",{{"color","#ff0000ff"},{"width","8"},{"height","8"}});equal_full();
        sc.remove("a.g.n.c");equal_full();
        sc.create("a.g.live",{{"width","8"},{"height","8"}});sc.bind_video_layer("a.g.live");
        const auto key=oa::render::VideoContent::frame_key("a.g.live");
        std::vector<uint8_t> px(8*8*4,255);
        check(renderer.upload_host_frame(key,8,8,px.data()),"host video upload");
        sc.mark_dirty("a.g.live",oa::render::DirtyAspect::HostFrame);equal_full();
        for(size_t i=0;i<px.size();i+=4)px[i]=0;
        renderer.upload_host_frame(key,8,8,px.data());sc.mark_dirty("a.g.live",oa::render::DirtyAspect::HostFrame);equal_full();
        // Warm invalidations reuse targets, resetting alpha/color modulation.
        const auto created=renderer.render_stats().textures_created;
        for(int i=0;i<20;++i){renderer.render_clear_tex_cache();draw();}
        check(renderer.render_stats().textures_created==created,"warm group invalidation creates no new targets");
        // A nested crop depends on an earlier backdrop outside its parent
        // group's subtree. Changing that backdrop must invalidate the parent.
        sc.create("0",{{"width","8"},{"height","8"}});sc.bind_video_layer("0");
        renderer.upload_host_frame(oa::render::VideoContent::frame_key("0"),8,8,px.data());
        sc.create("c",{{"intermediate_render","1"},{"alpha","128"}});
        sc.create("c.crop",{{"intermediate_render","1"},{"clip","0,0,32,32"}});
        sc.create("c.crop.blue",{{"color","#ff0000ff"},{"width","32"},{"height","32"}});
        draw();sc.set_props("0",{{"xscale","50"}});equal_full();
        // Resizing must never reuse a target with the old dimensions.
        renderer.stage_w_=renderer.stage_h_=80;equal_full();
        renderer.release_all();
    }
    SDL_DestroyWindow(win);SDL_Quit();return failures?1:0;
}
