// Compare the actual mixed PCM against independently indexed Vorbis samples.
// Exercises worker starvation, ring wrap, resampling, A/B loops and EOF.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>
#include "SDL3/SDL.h"
#include "core/media/audio.h"
#include "core/media/decode_pool.h"
#include "ogg_fixture.h"

namespace {
int failures=0;
void check(bool ok,const char* text) {
    if(!ok){std::fprintf(stderr,"FAIL: %s\n",text);++failures;}
}
std::vector<float> decode(const std::vector<uint8_t>& bytes,int channels) {
    auto src=oa::media::VorbisSource::open(bytes);
    std::vector<float> result,buf(size_t(channels)*1024);
    for(long n;(n=src->read(buf.data(),1024))>0;) {
        for(long f=0;f<n;++f)for(int c=0;c<channels;++c)
            result.push_back(buf[size_t(c)*1024+size_t(f)]);
    }
    return result;
}
}
int main() {
    const auto out=std::filesystem::temp_directory_path()/
        ("oa_audio_window_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(out);
    const auto prefix=(out/"samples").string();
    SDL_setenv("OA_AUDIO_REC",prefix.c_str(),1);
    std::vector<float> expected;
    oa::media::DecodePool pool(1);
    oa::media::MediaPlayers mp;
    for(int scenario=0;scenario<4;++scenario) {
        const int channels=scenario==0?1:2;
        const long rate=scenario==0?32000:scenario==1?48000:22050;
        const bool ab=scenario==2,loop=scenario!=3;
        std::map<std::string,std::vector<uint8_t>> assets;
        assets["a.ogg"]=oafix::tone_ogg(0.193,channels,rate,317,0.4f);
        if(ab)assets["b.ogg"]=oafix::tone_ogg(0.137,channels,rate,631,0.25f);
        const auto a=decode(assets.at("a.ogg"),channels);
        const auto b=ab?decode(assets.at("b.ogg"),channels):a;
        const size_t a_frames=a.size()/size_t(channels),b_frames=b.size()/size_t(channels);
        std::map<std::string,int> reads;
        std::mutex reads_mu;
        mp.set_loader([&](const std::string& name)->std::optional<std::vector<uint8_t>> {
            std::lock_guard<std::mutex> lk(reads_mu);
            ++reads[name];
            const auto it=assets.find(name);
            return it==assets.end()?std::nullopt:std::optional(it->second);
        });
        for(bool workers:{false,true}) {
            reads.clear();
            mp.set_decode_pool(workers?&pool:nullptr);
            oa::media::AudioEngine ae;
            oa::media::BgmConfig config;config.loop_play=loop;
            ae.play_bgm("a.ogg",config);
            if(ab)ae.state().bgm_channel->loop_file="b.ogg";
            size_t frames=0;
            const auto start=std::chrono::steady_clock::now();
            int finished=0;
            // Large irregular blocks force fallback and wrap past the 16K ring.
            for(uint64_t ms:{16u,33u,700u,1u,400u}) {
                ae.update(ms);
                mp.update(ms,ae,[&](auto,const auto&){++finished;});
                frames+=size_t(ms*44100/1000); // recompute exact total below
            }
            frames=size_t(1150*44100/1000);
            mp.stop_all(); // joins the worker before the loader's captures die
            check(reads["a.ogg"]==1,"each channel opens its source only once");
            if(ab)check(reads["b.ogg"]==1,"A/B switch opens the loop segment once");
            check(finished==(loop?0:1),"natural EOF fires once, looping channels do not finish");
            double position=0;
            auto sample=[&](size_t f,int c) {
                if(f<a_frames)return a[f*size_t(channels)+size_t(c)];
                if(!loop)return 0.0f;
                const auto& samples=ab?b:a;
                return samples[((f-a_frames)%b_frames)*size_t(channels)+size_t(c)];
            };
            // Match per-block position accumulation, while indexing an
            // independently decoded source (no engine interpolation helpers).
            double frac=0;
            for(uint64_t ms:{16u,33u,700u,1u,400u}) {
                frac+=double(ms)*44.1;
                const size_t n=size_t(frac);frac-=double(n);
                for(size_t f=0;f<n;++f) {
                    const double p=position+double(f)*double(rate)/44100.0;
                    const size_t k=size_t(std::floor(p));const float t=float(p-double(k));
                    for(int c=0;c<2;++c) {
                        const int src_c=channels==1?0:c;
                        const float x=sample(k,src_c),y=sample(k+1,src_c);
                        expected.push_back(x+(y-x)*t);
                    }
                }
                position+=double(n)*(double(rate)/44100.0);
            }
            std::printf("scenario=%d workers=%d frames=%zu wall_ms=%.2f reads=%d\n",scenario,
                int(workers),frames,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),reads["a.ogg"]);
        }
    }
    mp.release();
    std::ifstream wav(prefix+".mix.wav",std::ios::binary|std::ios::ate);
    const auto bytes=wav.tellg();
    check(bytes==std::streamoff(56+expected.size()*sizeof(float)),"mixed frame count matches the fractional output clock");
    std::vector<float> actual(expected.size());wav.seekg(56);
    wav.read(reinterpret_cast<char*>(actual.data()),std::streamsize(actual.size()*sizeof(float)));
    float max_error=0;
    for(size_t i=0;i<actual.size();++i)max_error=std::max(max_error,std::fabs(actual[i]-expected[i]));
    check(max_error<1e-6f,"sync and prefetched PCM match the reference across loops, resampling and EOF");
    std::printf("audio_window: samples=%zu max_error=%.9g failures=%d\n",actual.size(),max_error,failures);
    wav.close();
    if(!failures)std::filesystem::remove_all(out);
    else std::printf("capture=%s\n",prefix.c_str());
    return failures?1:0;
}
