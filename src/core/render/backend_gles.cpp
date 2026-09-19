#include "core/render/backend_gles.h"

#include <SDL3/SDL_video.h> // SDL_Window / SDL_GL_*（只含视频面，无 SDL_Render）

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__OHOS__)
// KR2 Harmony path (krkrsdl_harmony.cpp + krkrsdl_gl.cpp): link libGLESv3.so
// and call GLES via <GLES3/gl3.h>. SDL_GL_GetProcAddress on OHOS looks up
// libGLESv2.so, so GLES3 entry points (VAO, etc.) come back NULL.
#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <dlfcn.h>
#include <hilog/log.h>
#endif

// 原生 OpenGL ES 后端：GLES 全部原语手写（无 SDL_Render）。
// 渲染数学对照本机 sdl 线实际生效的 SDL3 "opengl" 渲染驱动逐项复刻，目标 =
// 同一批真实旅程上 GLES↔sdl 帧逐字节一致（或 ≤ 已知取整级差的量化差异）。
// Desktop: GL 入口点经 SDL_GL_GetProcAddress 加载。Harmony: 与 KR2 一样走
// 已链接的 libGLESv3.so。只使用 GLES 3.0 核心函数。

// ---------------------------------------------------------------------------
// GLES 3.0 最小 API 面（类型 + 常量 + 函数指针表，create() 时加载）
// ---------------------------------------------------------------------------
namespace {

typedef unsigned int GLenum_;
typedef unsigned int GLuint_;
typedef int GLint_;
typedef int GLsizei_;
typedef intptr_t GLsizeiptr_;
typedef unsigned char GLubyte_;
typedef char GLchar_;
typedef float GLfloat_;
typedef unsigned char GLboolean_;
typedef unsigned int GLbitfield_;

// 只用到的 GL 常量（GLES3 官方数值）
enum : unsigned int {
    GL_FALSE_ = 0, GL_TRUE_ = 1, GL_NO_ERROR_ = 0,
    GL_ZERO_ = 0, GL_ONE_ = 1,
    GL_SRC_COLOR_ = 0x0300, GL_SRC_ALPHA_ = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA_ = 0x0303,
    GL_BLEND_ = 0x0BE2, GL_SCISSOR_TEST_ = 0x0C11, GL_DEPTH_TEST_ = 0x0B71,
    GL_CULL_FACE_ = 0x0B44,
    GL_FUNC_ADD_ = 0x8006,
    GL_ARRAY_BUFFER_ = 0x8892, GL_DYNAMIC_DRAW_ = 0x88E8,
    GL_FRAGMENT_SHADER_ = 0x8B30, GL_VERTEX_SHADER_ = 0x8B31,
    GL_COMPILE_STATUS_ = 0x8B81, GL_LINK_STATUS_ = 0x8B82,
    GL_INFO_LOG_LENGTH_ = 0x8B84,
    GL_TEXTURE0_ = 0x84C0, GL_TEXTURE1_ = 0x84C1,
    GL_TEXTURE_2D_ = 0x0DE1, GL_TEXTURE_MIN_FILTER_ = 0x2801,
    GL_TEXTURE_MAG_FILTER_ = 0x2800, GL_TEXTURE_WRAP_S_ = 0x2802,
    GL_TEXTURE_WRAP_T_ = 0x2803, GL_CLAMP_TO_EDGE_ = 0x812F,
    GL_LINEAR_ = 0x2601,
    GL_UNPACK_ALIGNMENT_ = 0x0CF5, GL_UNPACK_ROW_LENGTH_ = 0x0CF2,
    GL_PACK_ALIGNMENT_ = 0x0D05,
    GL_RGBA_ = 0x1908, GL_RGBA8_ = 0x8058, GL_UNSIGNED_BYTE_ = 0x1401,
    GL_FLOAT_ = 0x1406, GL_TRIANGLES_ = 0x0004,
    GL_COLOR_BUFFER_BIT_ = 0x00004000,
    GL_FRAMEBUFFER_ = 0x8D40, GL_COLOR_ATTACHMENT0_ = 0x8CE0,
    GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5,
    GL_VERSION_ = 0x1F02, GL_RENDERER_ = 0x1F01,
};

struct GlProcs {
#define OA_GLP(type, name, params) type (*name) params = nullptr;
    OA_GLP(void, ActiveTexture, (GLenum_))
    OA_GLP(void, AttachShader, (GLuint_, GLuint_))
    OA_GLP(void, BindBuffer, (GLenum_, GLuint_))
    OA_GLP(void, BindFramebuffer, (GLenum_, GLuint_))
    OA_GLP(void, BindTexture, (GLenum_, GLuint_))
    OA_GLP(void, BindVertexArray, (GLuint_))
    OA_GLP(void, BlendEquation, (GLenum_))
    OA_GLP(void, BlendFuncSeparate, (GLenum_, GLenum_, GLenum_, GLenum_))
    OA_GLP(void, BufferData, (GLenum_, GLsizeiptr_, const void*, GLenum_))
    OA_GLP(void, BufferSubData, (GLenum_, GLsizeiptr_, GLsizeiptr_, const void*))
    OA_GLP(GLenum_, CheckFramebufferStatus, (GLenum_))
    OA_GLP(void, Clear, (GLbitfield_))
    OA_GLP(void, ClearColor, (GLfloat_, GLfloat_, GLfloat_, GLfloat_))
    OA_GLP(void, CompileShader, (GLuint_))
    OA_GLP(GLuint_, CreateProgram, ())
    OA_GLP(GLuint_, CreateShader, (GLenum_))
    OA_GLP(void, DeleteBuffers, (GLsizei_, const GLuint_*))
    OA_GLP(void, DeleteFramebuffers, (GLsizei_, const GLuint_*))
    OA_GLP(void, DeleteProgram, (GLuint_))
    OA_GLP(void, DeleteShader, (GLuint_))
    OA_GLP(void, DeleteTextures, (GLsizei_, const GLuint_*))
    OA_GLP(void, DeleteVertexArrays, (GLsizei_, const GLuint_*))
    OA_GLP(void, Disable, (GLenum_))
    OA_GLP(void, DisableVertexAttribArray, (GLuint_))
    OA_GLP(void, DrawArrays, (GLenum_, GLint_, GLsizei_))
    OA_GLP(void, Enable, (GLenum_))
    OA_GLP(void, EnableVertexAttribArray, (GLuint_))
    OA_GLP(void, FramebufferTexture2D,
           (GLenum_, GLenum_, GLenum_, GLuint_, GLint_))
    OA_GLP(void, GenBuffers, (GLsizei_, GLuint_*))
    OA_GLP(void, GenFramebuffers, (GLsizei_, GLuint_*))
    OA_GLP(void, GenTextures, (GLsizei_, GLuint_*))
    OA_GLP(void, GenVertexArrays, (GLsizei_, GLuint_*))
    OA_GLP(GLenum_, GetError, ())
    OA_GLP(void, GetIntegerv, (GLenum_, GLint_*))
    OA_GLP(const GLubyte_*, GetString, (GLenum_))
    OA_GLP(void, GetProgramiv, (GLuint_, GLenum_, GLint_*))
    OA_GLP(void, GetShaderiv, (GLuint_, GLenum_, GLint_*))
    OA_GLP(void, GetShaderInfoLog, (GLuint_, GLsizei_, GLsizei_*, GLchar_*))
    OA_GLP(void, GetProgramInfoLog, (GLuint_, GLsizei_, GLsizei_*, GLchar_*))
    OA_GLP(GLint_, GetUniformLocation, (GLuint_, const GLchar_*))
    OA_GLP(void, LinkProgram, (GLuint_))
    OA_GLP(void, PixelStorei, (GLenum_, GLint_))
    OA_GLP(void, ReadPixels, (GLint_, GLint_, GLsizei_, GLsizei_, GLenum_,
                              GLenum_, void*))
    OA_GLP(void, Scissor, (GLint_, GLint_, GLsizei_, GLsizei_))
    OA_GLP(void, ShaderSource, (GLuint_, GLsizei_, const GLchar_* const*,
                                const GLint_*))
    OA_GLP(void, TexImage2D, (GLenum_, GLint_, GLint_, GLsizei_, GLsizei_,
                              GLint_, GLenum_, GLenum_, const void*))
    OA_GLP(void, TexParameteri, (GLenum_, GLenum_, GLint_))
    OA_GLP(void, TexSubImage2D, (GLenum_, GLint_, GLint_, GLint_, GLsizei_,
                                 GLsizei_, GLenum_, GLenum_, const void*))
    OA_GLP(void, Uniform1i, (GLint_, GLint_))
    OA_GLP(void, Uniform1f, (GLint_, GLfloat_))
    OA_GLP(void, Uniform2f, (GLint_, GLfloat_, GLfloat_))
    OA_GLP(void, UseProgram, (GLuint_))
    OA_GLP(void, VertexAttribPointer,
           (GLuint_, GLint_, GLenum_, GLboolean_, GLsizei_, const void*))
    OA_GLP(void, Viewport, (GLint_, GLint_, GLsizei_, GLsizei_))
#undef OA_GLP
    /// 加载全部入口点；失败返回 false 并写明缺哪个。
    bool load(const char** missing) {
#if defined(__OHOS__)
        // Same as KR2: the GLES3 symbols are already in this DSO's link map.
#define OA_GLL(name)                                                        \
    name = reinterpret_cast<decltype(name)>(                                \
        reinterpret_cast<void(*)()>(::gl##name));                           \
    if (!name)                                                              \
        name = (decltype(name))SDL_GL_GetProcAddress("gl" #name);           \
    if (!name)                                                              \
        name = (decltype(name))dlsym(RTLD_DEFAULT, "gl" #name);             \
    if (!name) { *missing = #name; return false; }
#else
#define OA_GLL(name)                                                        \
    name = (decltype(name))SDL_GL_GetProcAddress("gl" #name);               \
    if (!name) { *missing = #name; return false; }
#endif
        OA_GLL(ActiveTexture) OA_GLL(AttachShader) OA_GLL(BindBuffer)
        OA_GLL(BindFramebuffer) OA_GLL(BindTexture) OA_GLL(BindVertexArray)
        OA_GLL(BlendEquation) OA_GLL(BlendFuncSeparate) OA_GLL(BufferData)
        OA_GLL(BufferSubData) OA_GLL(CheckFramebufferStatus) OA_GLL(Clear)
        OA_GLL(ClearColor) OA_GLL(CompileShader) OA_GLL(CreateProgram)
        OA_GLL(CreateShader) OA_GLL(DeleteBuffers) OA_GLL(DeleteFramebuffers)
        OA_GLL(DeleteProgram) OA_GLL(DeleteShader) OA_GLL(DeleteTextures)
        OA_GLL(DeleteVertexArrays) OA_GLL(Disable)
        OA_GLL(DisableVertexAttribArray) OA_GLL(DrawArrays) OA_GLL(Enable)
        OA_GLL(EnableVertexAttribArray) OA_GLL(FramebufferTexture2D)
        OA_GLL(GenBuffers) OA_GLL(GenFramebuffers) OA_GLL(GenTextures)
        OA_GLL(GenVertexArrays) OA_GLL(GetError) OA_GLL(GetIntegerv)
        OA_GLL(GetString) OA_GLL(GetProgramiv) OA_GLL(GetShaderiv)
        OA_GLL(GetShaderInfoLog) OA_GLL(GetProgramInfoLog)
        OA_GLL(GetUniformLocation) OA_GLL(LinkProgram) OA_GLL(PixelStorei)
        OA_GLL(ReadPixels) OA_GLL(Scissor) OA_GLL(ShaderSource)
        OA_GLL(TexImage2D)
        OA_GLL(TexParameteri) OA_GLL(TexSubImage2D) OA_GLL(Uniform1i)
        OA_GLL(Uniform1f) OA_GLL(Uniform2f) OA_GLL(UseProgram)
        OA_GLL(VertexAttribPointer)
        OA_GLL(Viewport)
#undef OA_GLL
        return true;
    }
};

#if !defined(__OHOS__)
GlProcs g;
#endif

// Desktop: all gl* calls go through the GetProcAddress table.
// OHOS: do not remap — call the libGLESv3 prototypes from <GLES3/gl3.h>
// exactly like KR2 (krkrsdl_gl.cpp). A function-pointer table is how GLES3
// symbols went NULL on this platform (SDL looks up libGLESv2).
#if !defined(__OHOS__)
#define glActiveTexture g.ActiveTexture
#define glAttachShader g.AttachShader
#define glBindBuffer g.BindBuffer
#define glBindFramebuffer g.BindFramebuffer
#define glBindTexture g.BindTexture
#define glBindVertexArray g.BindVertexArray
#define glBlendEquation g.BlendEquation
#define glBlendFuncSeparate g.BlendFuncSeparate
#define glBufferData g.BufferData
#define glBufferSubData g.BufferSubData
#define glCheckFramebufferStatus g.CheckFramebufferStatus
#define glClear g.Clear
#define glClearColor g.ClearColor
#define glCompileShader g.CompileShader
#define glCreateProgram g.CreateProgram
#define glCreateShader g.CreateShader
#define glDeleteBuffers g.DeleteBuffers
#define glDeleteFramebuffers g.DeleteFramebuffers
#define glDeleteProgram g.DeleteProgram
#define glDeleteShader g.DeleteShader
#define glDeleteTextures g.DeleteTextures
#define glDeleteVertexArrays g.DeleteVertexArrays
#define glDisable g.Disable
#define glDisableVertexAttribArray g.DisableVertexAttribArray
#define glDrawArrays g.DrawArrays
#define glEnable g.Enable
#define glEnableVertexAttribArray g.EnableVertexAttribArray
#define glFramebufferTexture2D g.FramebufferTexture2D
#define glGenBuffers g.GenBuffers
#define glGenFramebuffers g.GenFramebuffers
#define glGenTextures g.GenTextures
#define glGenVertexArrays g.GenVertexArrays
#define glGetError g.GetError
#define glGetIntegerv g.GetIntegerv
#define glGetString g.GetString
#define glGetProgramiv g.GetProgramiv
#define glGetShaderiv g.GetShaderiv
#define glGetShaderInfoLog g.GetShaderInfoLog
#define glGetProgramInfoLog g.GetProgramInfoLog
#define glGetUniformLocation g.GetUniformLocation
#define glLinkProgram g.LinkProgram
#define glPixelStorei g.PixelStorei
#define glReadPixels g.ReadPixels
#define glScissor g.Scissor
#define glShaderSource g.ShaderSource
#define glTexImage2D g.TexImage2D
#define glTexParameteri g.TexParameteri
#define glTexSubImage2D g.TexSubImage2D
#define glUniform1i g.Uniform1i
#define glUniform1f g.Uniform1f
#define glUniform2f g.Uniform2f
#define glUseProgram g.UseProgram
#define glVertexAttribPointer g.VertexAttribPointer
#define glViewport g.Viewport
#endif

// GLSL ES 3.00. Keep shader text ASCII-only: Harmony GPU compilers (Mali /
// Maleoon / Adreno) reject non-ASCII even in comments. KR2 puts #version as
// the first byte of the raw string — a leading newline is enough to fail
// compile on some drivers ("#version must be the first directive").
const char* kVertexShader = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;
uniform vec2 uScale;
uniform vec2 uOff;
out vec4 vColor;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos.x * uScale.x + uOff.x,
                       aPos.y * uScale.y + uOff.y, 0.0, 1.0);
    vColor = aColor;
    vUV = aUV;
}
)";

const char* kFragmentShader = R"(#version 300 es
precision mediump float;
precision highp int;
in vec4 vColor;
in vec2 vUV;
uniform sampler2D uTex;
uniform highp int uUseTex;
out vec4 fragColor;
void main() {
    if (uUseTex != 0)
        fragColor = texture(uTex, vUV) * vColor;
    else
        fragColor = vColor;
}
)";

// [trans type=2] rule dissolve. keep = smoothstep(t, t+band, rule.r) with
// t = progress*(1+band)-band. Capture RGB, alpha = cap.a*keep.
const char* kRuleFragmentShader = R"(#version 300 es
precision mediump float;
in vec2 vUV;
uniform sampler2D uTex;
uniform sampler2D uRule;
uniform float uProgress;
uniform float uBand;
out vec4 fragColor;
void main() {
    vec4 cap = texture(uTex, vUV);
    float t = uProgress * (1.0 + uBand) - uBand;
    float r = texture(uRule, vUV).r;
    float keep = smoothstep(t, t + uBand, r);
    fragColor = vec4(cap.rgb, cap.a * keep);
}
)";

unsigned int compile_shader(unsigned int type, const char* src, char* log,
                            size_t logsz) {
    while (src && (*src == '\n' || *src == '\r' || *src == ' ' || *src == '\t'))
        ++src;
#ifdef OA_USE_SDL2
    std::string rewritten;
#if defined(__OHOS__) || defined(__ANDROID__)
    const bool rewrite_desktop = false;
#else
    const bool rewrite_desktop = true;
#endif
    const char* es = "#version 300 es";
    if (rewrite_desktop && src && std::strncmp(src, es, std::strlen(es)) == 0) {
        rewritten = src;
        const auto pos = rewritten.find(es);
        if (pos != std::string::npos)
            rewritten.replace(pos, std::strlen(es), "#version 330");
        // Desktop GL rejects ES-only precision statements (highp/mediump/int).
        for (;;) {
            auto p = rewritten.find("precision ");
            if (p == std::string::npos) break;
            auto e = rewritten.find(';', p);
            if (e == std::string::npos) break;
            rewritten.erase(p, e - p + 1);
        }
        src = rewritten.c_str();
    }
#endif
    const unsigned int sh = glCreateShader(type);
    if (!sh) {
        if (log && logsz) {
            std::snprintf(log, logsz, "glCreateShader returned 0 (err=0x%x)",
                          (unsigned)glGetError());
        }
        return 0;
    }
    const GLchar_* s = src;
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint_ ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS_, &ok);
    if (!ok && log && logsz) {
        GLsizei_ len = 0;
        glGetShaderInfoLog(sh, (GLsizei_)logsz, &len, log);
    }
    return ok ? sh : 0;
}

int portrait_top_offset_pct()
{
    // Same contract as KR2 / RPGRunner: TAPIR_PORTRAIT_TOP_OFFSET is 0=top,
    // 50=center, 100=bottom. Unset → 0 (top), matching those engines.
    const char* env = std::getenv("TAPIR_PORTRAIT_TOP_OFFSET");
    if (!env || !*env) return 0;
    int pct = std::atoi(env);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

#if defined(__OHOS__)
bool query_egl_surface_size(int* w, int* h)
{
    const EGLDisplay dpy = eglGetCurrentDisplay();
    const EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
    if (dpy == EGL_NO_DISPLAY || surf == EGL_NO_SURFACE) return false;
    EGLint ew = 0, eh = 0;
    if (!eglQuerySurface(dpy, surf, EGL_WIDTH, &ew)) return false;
    if (!eglQuerySurface(dpy, surf, EGL_HEIGHT, &eh)) return false;
    if (ew <= 1 || eh <= 1) return false;
    if (w) *w = int(ew);
    if (h) *h = int(eh);
    return true;
}
#endif

void query_output_size(SDL_Window* window, int* ow, int* oh, int* ws, int* hs,
                       int noted_w, int noted_h)
{
    int w = 0, h = 0, pw = 0, ph = 0;
    if (window) SDL_GetWindowSize(window, &w, &h);
#ifdef OA_USE_SDL2
    // Drawable size is the EGL/XComponent buffer. Window size can stay at
    // the 0x0 / stage size used at CreateWindow while the surface is already
    // full-screen — letterboxing against that leaves a postage-stamp stage.
    if (window) SDL_GL_GetDrawableSize(window, &pw, &ph);
#else
    if (window) SDL_GetWindowSizeInPixels(window, &pw, &ph);
#endif
#if defined(__OHOS__)
    int ew = 0, eh = 0;
    if (query_egl_surface_size(&ew, &eh)) {
        pw = ew;
        ph = eh;
        // The EGL buffer is the only pixels we can draw into. Keep window
        // metrics identical so dpi stays 1 (ArkTS/touch are already in
        // surface pixels) and letterbox uses the full XComponent, not the
        // 1×1 / stage size left over from CreateWindow.
        w = ew;
        h = eh;
    }
    if ((pw <= 1 || ph <= 1) && noted_w > 1 && noted_h > 1) {
        pw = noted_w;
        ph = noted_h;
    }
    if ((w <= 1 || h <= 1) && noted_w > 1 && noted_h > 1) {
        w = noted_w;
        h = noted_h;
    }
    if (pw > 1 && ph > 1 && (w != pw || h != ph)) {
        w = pw;
        h = ph;
    }
#endif
    if (pw <= 0 || ph <= 0) {
        pw = w;
        ph = h;
    }
    if (ow) *ow = pw;
    if (oh) *oh = ph;
    if (ws) *ws = w;
    if (hs) *hs = h;
}

void compute_letterbox(int ow, int oh, int lw, int lh,
                       float* dst_x, float* dst_y, float* dst_w, float* dst_h)
{
    if (ow <= 0 || oh <= 0 || lw <= 0 || lh <= 0) {
        *dst_x = 0;
        *dst_y = 0;
        *dst_w = float(std::max(0, ow));
        *dst_h = float(std::max(0, oh));
        return;
    }
    const float fow = float(ow), foh = float(oh);
    const float flw = float(lw), flh = float(lh);
    const float want = flw / flh, real = fow / foh;
    if (std::fabs(want - real) < 0.0001f) {
        *dst_x = 0;
        *dst_y = 0;
        *dst_w = fow;
        *dst_h = foh;
        return;
    }
    if (want > real) {
        // Stage wider than the screen: fit width, letterbox top/bottom.
        const float s = fow / flw;
        *dst_x = 0;
        *dst_w = fow;
        *dst_h = std::floor(flh * s);
        const float remaining = foh - *dst_h;
        if (remaining > 1.0f && oh > ow) {
#if defined(__OHOS__) || defined(__ANDROID__)
            *dst_y = remaining * float(portrait_top_offset_pct()) / 100.0f;
#else
            *dst_y = remaining / 2.0f;
#endif
        } else {
            *dst_y = remaining / 2.0f;
        }
    } else {
        const float s = foh / flh;
        *dst_y = 0;
        *dst_h = foh;
        *dst_w = std::floor(flw * s);
        *dst_x = (fow - *dst_w) / 2.0f;
    }
}

} // namespace

namespace oa::render {

GlesRenderBackend::~GlesRenderBackend() { shutdown(); }

void GlesRenderBackend::fail(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    err_ = buf;
    SDL_SetError("%s", buf);
#ifdef __OHOS__
    OH_LOG_Print(LOG_APP, LOG_ERROR, 0xFF00, "openartemis", "%{public}s", buf);
#endif
}

bool GlesRenderBackend::ensure_program() {
    if (program_) return true;
    char log[512] = {0};
    const unsigned int vs = compile_shader(GL_VERTEX_SHADER_, kVertexShader,
                                           log, sizeof(log));
    if (!vs) {
        fail("gles: vertex shader compile failed: %s", log);
        return false;
    }
    const unsigned int fs = compile_shader(GL_FRAGMENT_SHADER_, kFragmentShader,
                                           log, sizeof(log));
    if (!fs) {
        fail("gles: fragment shader compile failed: %s", log);
        glDeleteShader(vs);
        return false;
    }
    const unsigned int prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint_ ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS_, &ok);
    if (!ok) {
        GLsizei_ len = 0;
        glGetProgramInfoLog(prog, (GLsizei_)sizeof(log), &len, log);
        fail("gles: program link failed: %s", log);
        glDeleteProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    loc_scale_ = glGetUniformLocation(prog, "uScale");
    loc_off_ = glGetUniformLocation(prog, "uOff");
    loc_usetex_ = glGetUniformLocation(prog, "uUseTex");
    const GLint_ utex = glGetUniformLocation(prog, "uTex");
    program_ = prog;
    glUseProgram(program_);
    gl_cur_program_ = program_;
    if (utex >= 0) glUniform1i(utex, 0);
    // VAO/VBO：交错 8 float/顶点（pos.xy + color.rgba + uv.xy）
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER_, vbo_);
    glVertexAttribPointer(0, 2, GL_FLOAT_, GL_FALSE_, 8 * sizeof(GLfloat_),
                          (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT_, GL_FALSE_, 8 * sizeof(GLfloat_),
                          (const void*)(2 * sizeof(GLfloat_)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT_, GL_FALSE_, 8 * sizeof(GLfloat_),
                          (const void*)(6 * sizeof(GLfloat_)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER_, 0);
    return true;
}

bool GlesRenderBackend::ensure_rule_program() {
    if (rule_program_) return true;
    char log[512] = {0};
    const unsigned int vs = compile_shader(GL_VERTEX_SHADER_, kVertexShader,
                                           log, sizeof(log));
    if (!vs) {
        fail("gles: rule vertex shader compile failed: %s", log);
        return false;
    }
    const unsigned int fs = compile_shader(GL_FRAGMENT_SHADER_, kRuleFragmentShader,
                                           log, sizeof(log));
    if (!fs) {
        fail("gles: rule fragment shader compile failed: %s", log);
        glDeleteShader(vs);
        return false;
    }
    const unsigned int prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint_ ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS_, &ok);
    if (!ok) {
        GLsizei_ len = 0;
        glGetProgramInfoLog(prog, (GLsizei_)sizeof(log), &len, log);
        fail("gles: rule program link failed: %s", log);
        glDeleteProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    rule_loc_scale_ = glGetUniformLocation(prog, "uScale");
    rule_loc_off_ = glGetUniformLocation(prog, "uOff");
    rule_loc_progress_ = glGetUniformLocation(prog, "uProgress");
    rule_loc_band_ = glGetUniformLocation(prog, "uBand");
    rule_loc_utex_ = glGetUniformLocation(prog, "uTex");
    rule_loc_urule_ = glGetUniformLocation(prog, "uRule");
    rule_program_ = prog;
    glUseProgram(rule_program_);
    if (rule_loc_utex_ >= 0) glUniform1i(rule_loc_utex_, 0);
    if (rule_loc_urule_ >= 0) glUniform1i(rule_loc_urule_, 1);
    glUseProgram(program_); // 状态回到主 program
    gl_cur_program_ = program_;
    return true;
}

bool GlesRenderBackend::create(SDL_Window* window, int stage_w, int stage_h,
                               BackendInfo* info)
{
    err_.clear();
    window_ = window;
    logical_w_ = stage_w;
    logical_h_ = stage_h;
    if (!gl_context_) {
#if defined(__OHOS__)
        // Host already created the context the KR2 way. Do not CreateContext
        // again (OHOS is a single XComponent/EGL surface) and do not treat a
        // second MakeCurrent failure as fatal — CreateContext already made it
        // current, and KR2 ignores MakeCurrent's return.
        gl_context_ = SDL_GL_GetCurrentContext();
        if (!gl_context_) {
            gl_context_ = SDL_GL_CreateContext(window);
            if (!gl_context_) {
                SDL_ClearError();
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
                gl_context_ = SDL_GL_CreateContext(window);
            }
        }
        if (!gl_context_) {
            fail("[oa-kr2gl] GLES context creation failed: %s", SDL_GetError());
            return false;
        }
        // CreateContext already made the context current (SDL_EGL_CreateContext
        // calls eglMakeCurrent). A second OHOS_GLES_MakeCurrent can wait on /
        // recreate the XComponent surface and unbind a working context. Only
        // rebind if TLS current is empty.
        if (SDL_GL_GetCurrentContext() != gl_context_)
            (void)oa_sdl2_GL_MakeCurrent()(window, (SDL_GLContext)gl_context_);
#else
        // GLES 上下文由后端 create() 内创建
        // （SDL_GL ES profile；本机 Xvfb/radeonsi 实测 ES 3.2 可建）。
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
#if defined(OA_USE_SDL2) && !defined(__ANDROID__)
                            SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#else
                            SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
        gl_context_ = SDL_GL_CreateContext(window);
#if defined(OA_USE_SDL2)
        if (!gl_context_) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                                SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
            gl_context_ = SDL_GL_CreateContext(window);
        }
#endif
        if (!gl_context_) {
            fail("gles: GLES context creation failed: %s", SDL_GetError());
            return false;
        }
        if (!SDL_GL_MakeCurrent(window, (SDL_GLContext)gl_context_)) {
            fail("gles: SDL_GL_MakeCurrent failed: %s", SDL_GetError());
            SDL_GL_DestroyContext((SDL_GLContext)gl_context_);
            gl_context_ = nullptr;
            return false;
        }
#endif
#if defined(__OHOS__)
        // KR2 (krkrsdl_harmony.cpp / krkrsdl_android.cpp / krkrsdl.cpp):
        // SwapInterval(1) waits one compositor vblank. That is the frame
        // clock — 60, 90, 120, 144 Hz, whatever the panel actually runs.
        // Do not force interval N to fake 60 Hz, and do not pair this with
        // a software 60 Hz SDL_Delay (the two waits stacked to ~30 fps).
        if (SDL_GL_SetSwapInterval(1) != 0) {
            SDL_Log("[gles] SetSwapInterval(1) failed: %s", SDL_GetError());
            OH_LOG_Print(LOG_APP, LOG_WARN, 0xFF00, "openartemis",
                         "[gles] SetSwapInterval(1) failed: %{public}s",
                         SDL_GetError());
        }
#else
        // Desktop: SwapBuffers blocks on vblank. On 120/180/240 Hz panels
        // pick interval N so present itself is ~60 Hz (windowed power).
        // 90/144 Hz are not integer multiples of 60 — interval stays 1 and
        // the host remainder-sleeps so Lua does not free-run.
        {
            int refresh = 0;
#ifdef OA_USE_SDL2
            const int idx = SDL_GetWindowDisplayIndex(window);
            SDL_DisplayMode mode{};
            // Prefer desktop mode: GetCurrentDisplayMode can report junk on
            // windowed / VRR setups (this machine logged 32 Hz).
            if (idx >= 0 && SDL_GetDesktopDisplayMode(idx, &mode) == 0)
                refresh = mode.refresh_rate;
            SDL_DisplayMode cur{};
            if (idx >= 0 && SDL_GetCurrentDisplayMode(idx, &cur) == 0 &&
                cur.refresh_rate >= 50)
                refresh = cur.refresh_rate;
#else
            const SDL_DisplayID did = SDL_GetDisplayForWindow(window);
            const SDL_DisplayMode* cur =
                did ? SDL_GetCurrentDisplayMode(did) : nullptr;
            if (cur) refresh = cur->refresh_rate;
#endif
            int interval = 1;
            if (refresh >= 50) {
                const int n = refresh / 60;
                if (n > 1) {
                    const int effective = refresh / n;
                    if (effective >= 54 && effective <= 66) interval = n;
                }
            }
#ifdef OA_USE_SDL2
            if (SDL_GL_SetSwapInterval(interval) != 0) {
                std::fprintf(stderr, "[gles] vsync interval %d failed: %s; trying 1\n",
                             interval, SDL_GetError());
                SDL_GL_SetSwapInterval(1);
                interval = 1;
            }
            const int got = SDL_GL_GetSwapInterval();
#else
            if (!SDL_GL_SetSwapInterval(interval)) {
                std::fprintf(stderr, "[gles] vsync interval %d failed: %s; trying 1\n",
                             interval, SDL_GetError());
                SDL_GL_SetSwapInterval(1);
                interval = 1;
            }
            int got = interval;
            SDL_GL_GetSwapInterval(&got);
#endif
            char hzbuf[32];
            if (refresh >= 50)
                std::snprintf(hzbuf, sizeof(hzbuf), "%d Hz", refresh);
            else
                std::snprintf(hzbuf, sizeof(hzbuf), "unknown");
            std::printf("[gles] vsync interval=%d (requested %d, display %s)\n",
                        got, interval, hzbuf);
        }
        const char* missing = nullptr;
        if (!g.load(&missing)) {
            fail("gles: GL entry point '%s' unavailable", missing);
            SDL_GL_DestroyContext((SDL_GLContext)gl_context_);
            gl_context_ = nullptr;
            return false;
        }
#endif
        const GLubyte_* ver = glGetString(GL_VERSION_);
        const GLubyte_* ren = glGetString(GL_RENDERER_);
        std::printf("[gles] context: %s (%s)\n", ver ? (const char*)ver : "?",
                    ren ? (const char*)ren : "?");
#ifdef __OHOS__
        SDL_Log("[gles] context: %s (%s)", ver ? (const char*)ver : "?",
                ren ? (const char*)ren : "?");
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "openartemis",
                     "[gles] context: %{public}s (%{public}s)",
                     ver ? (const char*)ver : "?",
                     ren ? (const char*)ren : "?");
        if (!ver) {
            fail("[oa-kr2gl] glGetString(GL_VERSION) is NULL (no current GLES context, err=0x%x)",
                 (unsigned)glGetError());
            return false;
        }
#endif
        glDisable(GL_DEPTH_TEST_);
        glDisable(GL_CULL_FACE_);
        glDisable(GL_SCISSOR_TEST_);
    }
    if (!ensure_program()) return false;
    int ws = 0, hs = 0;
    query_output_size(window, &out_w_, &out_h_, &ws, &hs, noted_w_, noted_h_);
    dpi_x_ = (ws > 1 && out_w_ > 1) ? float(out_w_) / float(ws) : 1.0f;
    dpi_y_ = (hs > 1 && out_h_ > 1) ? float(out_h_) / float(hs) : 1.0f;
#if defined(__OHOS__)
    if (ws == out_w_ && hs == out_h_) {
        dpi_x_ = 1.0f;
        dpi_y_ = 1.0f;
    }
#endif
    compute_letterbox(out_w_, out_h_, logical_w_, logical_h_,
                      &dst_x_, &dst_y_, &dst_w_, &dst_h_);
    cur_scale_x_ = (logical_w_ > 0 && dst_w_ > 0) ? dst_w_ / float(logical_w_) : 1.0f;
    cur_scale_y_ = (logical_h_ > 0 && dst_h_ > 0) ? dst_h_ / float(logical_h_) : 1.0f;
#ifdef __OHOS__
    {
        int refresh = 0;
#ifdef OA_USE_SDL2
        const int idx = SDL_GetWindowDisplayIndex(window);
        SDL_DisplayMode cur{};
        if (idx >= 0 && SDL_GetCurrentDisplayMode(idx, &cur) == 0)
            refresh = cur.refresh_rate;
        const int got = SDL_GL_GetSwapInterval();
#else
        const SDL_DisplayID did = SDL_GetDisplayForWindow(window);
        const SDL_DisplayMode* cur =
            did ? SDL_GetCurrentDisplayMode(did) : nullptr;
        if (cur) refresh = cur->refresh_rate;
        int got = 1;
        SDL_GL_GetSwapInterval(&got);
#endif
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "openartemis",
                     "[gles] layout win=%{public}dx%{public}d drawable=%{public}dx%{public}d "
                     "stage=%{public}dx%{public}d dst=%{public}d,%{public}d %{public}dx%{public}d offset=%{public}d",
                     ws, hs, out_w_, out_h_, logical_w_, logical_h_,
                     (int)dst_x_, (int)dst_y_, (int)dst_w_, (int)dst_h_,
                     portrait_top_offset_pct());
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "openartemis",
                     "[gles] vsync interval=%{public}d display=%{public}d Hz "
                     "(interval 1 = one vblank, not a 60 Hz lock)",
                     got, refresh);
        SDL_Log("[gles] vsync interval=%d display=%d Hz", got, refresh);
    }
#endif
    if (info) {
        info->output_w = out_w_;
        info->output_h = out_h_;
        info->logical_w = stage_w;
        info->logical_h = stage_h;
        info->logical_mode = 2; // SDL_LOGICAL_PRESENTATION_LETTERBOX
    }
    created_ = true;
    return true;
}

void GlesRenderBackend::shutdown()
{
    batch_.clear();
    batch_tex_ = nullptr;
    batch_key_valid_ = false;
    gl_cur_program_ = 0;
    gl_blend_valid_ = false;
    gl_scissor_valid_ = false;
    gl_xform_valid_ = false;
    gl_usetex_ = -1;
    gl_bound_tex_ = 0;
    gl_viewport_[2] = gl_viewport_[3] = -1;
    if (!created_) {
        // 半成品上下文也要收掉（create 失败路径已自行清理，这里兜底）
        if (gl_context_) {
            SDL_GL_DestroyContext((SDL_GLContext)gl_context_);
            gl_context_ = nullptr;
        }
        return;
    }
    if (program_) glDeleteProgram(program_);
    if (rule_program_) glDeleteProgram(rule_program_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    program_ = 0; rule_program_ = 0; vao_ = 0; vbo_ = 0;
    if (gl_context_) {
        SDL_GL_DestroyContext((SDL_GLContext)gl_context_);
        gl_context_ = nullptr;
    }
    cur_target_ = nullptr;
    created_ = false;
}

const char* GlesRenderBackend::last_error()
{
    return err_.empty() ? "" : err_.c_str();
}

// ---------------------------------------------------------------------------
// target / clip 状态
// ---------------------------------------------------------------------------

TextureRef GlesRenderBackend::current_target() { return cur_target_; }

void GlesRenderBackend::set_target(TextureRef t)
{
    GlesTexture* gt = gles_tex(t);
    if (!gt && t) return; // 无效纹理
    if (!gl_context_) return;
    if (gt && !gt->fbo) return; // 非 target 纹理不能绑
    if (gt == cur_target_) return; // 无切换：批次继续累积
    flush_batch(); // 排队绘制落在旧 target 上
    if (gt) {
        cur_target_ = gt;
        glBindFramebuffer(GL_FRAMEBUFFER_, gt->fbo);
        if (gl_viewport_[0] != 0 || gl_viewport_[1] != 0 ||
            gl_viewport_[2] != gt->w || gl_viewport_[3] != gt->h) {
            glViewport(0, 0, gt->w, gt->h);
            gl_viewport_[0] = 0; gl_viewport_[1] = 0;
            gl_viewport_[2] = gt->w; gl_viewport_[3] = gt->h;
        }
        gl_xform_valid_ = false; // scale/off 随 target 变
    } else {
        cur_target_ = nullptr;
        glBindFramebuffer(GL_FRAMEBUFFER_, 0);
        ensure_window_ready();
        gl_xform_valid_ = false;
    }
}

void GlesRenderBackend::ensure_window_ready()
{
    // 目标=窗口：刷新 drawable 尺寸与竖屏偏移；变了就重算 letterbox。
    int ow = 0, oh = 0, ws = 0, hs = 0;
    query_output_size(window_, &ow, &oh, &ws, &hs, noted_w_, noted_h_);
    const int pct = portrait_top_offset_pct();
    static int s_applied_pct = -1;
    if (ow == out_w_ && oh == out_h_ && pct == s_applied_pct) return;
    s_applied_pct = pct;
    out_w_ = ow;
    out_h_ = oh;
    dpi_x_ = (ws > 1 && ow > 1) ? float(ow) / float(ws) : 1.0f;
    dpi_y_ = (hs > 1 && oh > 1) ? float(oh) / float(hs) : 1.0f;
#if defined(__OHOS__)
    if (ws == ow && hs == oh) {
        dpi_x_ = 1.0f;
        dpi_y_ = 1.0f;
    }
#endif
    compute_letterbox(ow, oh, logical_w_, logical_h_,
                      &dst_x_, &dst_y_, &dst_w_, &dst_h_);
    cur_scale_x_ = (logical_w_ > 0 && dst_w_ > 0) ? dst_w_ / float(logical_w_) : 1.0f;
    cur_scale_y_ = (logical_h_ > 0 && dst_h_ > 0) ? dst_h_ / float(logical_h_) : 1.0f;
#ifdef __OHOS__
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "openartemis",
                 "[gles] letterbox win=%{public}dx%{public}d drawable=%{public}dx%{public}d "
                 "stage=%{public}dx%{public}d dst=%{public}d,%{public}d %{public}dx%{public}d",
                 ws, hs, ow, oh, logical_w_, logical_h_,
                 (int)dst_x_, (int)dst_y_, (int)dst_w_, (int)dst_h_);
#endif
}

bool GlesRenderBackend::clip_enabled() { return clip_enabled_; }

IRect GlesRenderBackend::clip_rect() { return clip_; }

void GlesRenderBackend::set_clip(const IRect& r)
{
    clip_enabled_ = true;
    clip_ = r;
}

void GlesRenderBackend::clear_clip()
{
    clip_enabled_ = false;
}

void GlesRenderBackend::set_draw_blend(BlendMode m) { draw_blend_ = m; }

void GlesRenderBackend::set_draw_color(uint8_t cr, uint8_t cg, uint8_t cb,
                                       uint8_t ca)
{
    draw_color_[0] = cr;
    draw_color_[1] = cg;
    draw_color_[2] = cb;
    draw_color_[3] = ca;
}

/// 每绘制的混合 + clip 状态（目标空间像素坐标）。SDL 驱动语义：
/// clip 对 target = 直接；对窗口 = 逻辑坐标缩放后翻转（画到 dst 区）。
/// GL 调用去重：与上一次实际应用的状态相同则跳过对应调用（合批后
/// 连续 flush 之间状态几乎不变；逐调用比对也比驱动校验便宜）。
void GlesRenderBackend::apply_draw_state(const GlesTexture* tex, BlendMode blend,
                                         bool clip_on, const IRect& clip)
{
    (void)tex;
    if (gl_cur_program_ != program_) {
        glUseProgram(program_);
        gl_cur_program_ = program_;
    }
    if (!gl_blend_valid_ || blend != gl_blend_) {
        if (blend == BlendMode::None) {
            glDisable(GL_BLEND_);
        } else {
            glEnable(GL_BLEND_);
            switch (blend) {
                case BlendMode::Blend:
                    glBlendFuncSeparate(GL_SRC_ALPHA_, GL_ONE_MINUS_SRC_ALPHA_,
                                        GL_ONE_, GL_ONE_MINUS_SRC_ALPHA_);
                    break;
                case BlendMode::Add:
                    glBlendFuncSeparate(GL_SRC_ALPHA_, GL_ONE_, GL_ZERO_, GL_ONE_);
                    break;
                case BlendMode::Mod:
                    glBlendFuncSeparate(GL_ZERO_, GL_SRC_COLOR_, GL_ZERO_, GL_ONE_);
                    break;
                case BlendMode::Premul:
                    // 源已是预乘 alpha：rgb 直接相加，dst 按 1-src.a 衰减
                    // （组 target 的像素由离屏 pass 的预乘混合写出）。
                    glBlendFuncSeparate(GL_ONE_, GL_ONE_MINUS_SRC_ALPHA_,
                                        GL_ONE_, GL_ONE_MINUS_SRC_ALPHA_);
                    break;
                case BlendMode::None: break;
            }
            glBlendEquation(GL_FUNC_ADD_);
        }
        gl_blend_ = blend;
        gl_blend_valid_ = true;
    }
    // scissor：先算目标空间矩形，再与已应用值比对。
    bool want_scissor = false;
    IRect sr{};
    float sx = 0, sy = 0, ox = 0, oy = 0;
    if (cur_target_) {
        // FBO：clip = 内容坐标（SDL target 分支：不翻行）
        want_scissor = clip_on && clip.w > 0 && clip.h > 0;
        sr = clip;
        sx = 2.0f / float(cur_target_->w);
        sy = 2.0f / float(cur_target_->h);
        ox = -1.0f;
        oy = -1.0f;
    } else {
        // 窗口：clip 按逻辑坐标缩放（floor/ceil，SDL UpdatePixelClipRect），
        // 落在 letterbox dst 区内（y 翻转到窗口底原点）
        if (clip_on && clip.w > 0 && clip.h > 0) {
            const float cx = std::floor(clip.x * cur_scale_x_);
            const float cy = std::floor(clip.y * cur_scale_y_);
            const float cw = std::ceil(float(clip.w) * cur_scale_x_);
            const float ch = std::ceil(float(clip.h) * cur_scale_y_);
            want_scissor = true;
            sr.x = (int)(std::floor(dst_x_) + cx);
            sr.y = (int)(float(out_h_) - (std::floor(dst_y_) + cy) - ch);
            sr.w = (int)cw;
            sr.h = (int)ch;
        }
        // Viewport is the letterbox dest in window pixels (KR2
        // SDL_GL_DrawTexture / RPGRunner recalculate_viewport). Draw
        // coordinates stay in stage/logical space (SDL logical presentation:
        // glOrtho(0, stage_w, stage_h, 0)). Using dest pixels here mapped a
        // 1920x1080 stage into the corner of a 2560x1440 dest.
        const float vw = dst_w_ > 0 ? dst_w_ : 1.0f;
        const float vh = dst_h_ > 0 ? dst_h_ : 1.0f;
        const float lw = logical_w_ > 0 ? float(logical_w_) : vw;
        const float lh = logical_h_ > 0 ? float(logical_h_) : vh;
        const int vx = (int)std::floor(dst_x_);
        const int vy = (int)(float(out_h_) - std::floor(dst_y_) - dst_h_);
        const int vw_i = (int)std::ceil(vw);
        const int vh_i = (int)std::ceil(vh);
        if (gl_viewport_[0] != vx || gl_viewport_[1] != vy ||
            gl_viewport_[2] != vw_i || gl_viewport_[3] != vh_i) {
            glViewport((GLint_)vx, (GLint_)vy, (GLsizei_)vw_i, (GLsizei_)vh_i);
            gl_viewport_[0] = vx; gl_viewport_[1] = vy;
            gl_viewport_[2] = vw_i; gl_viewport_[3] = vh_i;
        }
        sx = 2.0f / lw;
        sy = -2.0f / lh;
        ox = -1.0f;
        oy = 1.0f;
    }
    if (!gl_scissor_valid_ || want_scissor != gl_scissor_on_ ||
        (want_scissor &&
         (sr.x != gl_scissor_rect_.x || sr.y != gl_scissor_rect_.y ||
          sr.w != gl_scissor_rect_.w || sr.h != gl_scissor_rect_.h))) {
        if (want_scissor) {
            glEnable(GL_SCISSOR_TEST_);
            glScissor((GLint_)sr.x, (GLint_)sr.y, (GLsizei_)sr.w, (GLsizei_)sr.h);
        } else {
            glDisable(GL_SCISSOR_TEST_);
        }
        gl_scissor_on_ = want_scissor;
        gl_scissor_rect_ = sr;
        gl_scissor_valid_ = true;
    }
    if (!gl_xform_valid_ || sx != gl_scale_[0] || sy != gl_scale_[1] ||
        ox != gl_off_[0] || oy != gl_off_[1]) {
        glUniform2f(loc_scale_, sx, sy);
        glUniform2f(loc_off_, ox, oy);
        gl_scale_[0] = sx; gl_scale_[1] = sy;
        gl_off_[0] = ox; gl_off_[1] = oy;
        gl_xform_valid_ = true;
    }
    // 记住本次的像素→NDC 数学（rule program 复用同一坐标约定）
    last_scale_[0] = sx;
    last_scale_[1] = sy;
    last_off_[0] = ox;
    last_off_[1] = oy;
}

// ---------------------------------------------------------------------------
// draw 合批
// ---------------------------------------------------------------------------

void GlesRenderBackend::batch_append(const GlesTexture* tex, BlendMode blend,
                                     const GLfloat_* verts, int nverts)
{
    ++stats_.draw_calls;  // one submitted primitive (batched or not)
    const bool key_match = batch_key_valid_ && batch_tex_ == tex &&
        batch_blend_ == blend && batch_clip_on_ == clip_enabled_ &&
        (!clip_enabled_ ||
         (batch_clip_.x == clip_.x && batch_clip_.y == clip_.y &&
          batch_clip_.w == clip_.w && batch_clip_.h == clip_.h));
    if (!key_match) {
        flush_batch();
        batch_tex_ = tex;
        batch_blend_ = blend;
        batch_clip_on_ = clip_enabled_;
        batch_clip_ = clip_;
        batch_key_valid_ = true;
    }
    batch_.insert(batch_.end(), verts, verts + size_t(nverts) * 8);
    // 上限防无限增长（长 emote 网格链）：≈2730 quads 即 512KB 顶点。
    if (batch_.size() >= (size_t(1) << 17)) flush_batch();
}

void GlesRenderBackend::flush_batch()
{
    if (batch_.empty()) return;
    ++stats_.batches;
    if (!gl_context_) { // shutdown 后的尾调用：丢弃即可
        batch_.clear();
        batch_key_valid_ = false;
        return;
    }
    apply_draw_state(batch_tex_, batch_blend_, batch_clip_on_, batch_clip_);
    if (batch_tex_) {
        glActiveTexture(0x84C0); // GL_TEXTURE0
        if (gl_bound_tex_ != batch_tex_->tex) {
            glBindTexture(GL_TEXTURE_2D_, batch_tex_->tex);
            gl_bound_tex_ = batch_tex_->tex;
            ++stats_.texture_binds;
        }
        if (gl_usetex_ != 1) {
            glUniform1i(loc_usetex_, 1);
            gl_usetex_ = 1;
        }
    } else if (gl_usetex_ != 0) {
        glUniform1i(loc_usetex_, 0);
        gl_usetex_ = 0;
    }
    glBindBuffer(GL_ARRAY_BUFFER_, vbo_);
    glBufferData(GL_ARRAY_BUFFER_, GLsizeiptr_(batch_.size() * sizeof(GLfloat_)),
                 batch_.data(), GL_DYNAMIC_DRAW_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES_, 0, (GLsizei_)(batch_.size() / 8));
    glBindVertexArray(0);
    batch_.clear();
}

void GlesRenderBackend::clear()
{
    if (!gl_context_) return;
    flush_batch(); // 先画完已排队内容——clear 语义在先前绘制之后
    // glClear 受 scissor 影响：SDL 驱动在 clear 时先关 scissor（全目标清，
    // SDL CLEAR 命令同语义）；后续绘制的 apply_draw_state 按需重开。
    if (gl_scissor_on_) {
        glDisable(GL_SCISSOR_TEST_);
        gl_scissor_on_ = false;
        gl_scissor_valid_ = true;
    }
    // Window target: leftover FBO viewport is the stage (e.g. 1280x720).
    // Clear the full drawable so letterbox bars are black and the next
    // letterbox viewport is not a postage stamp in the corner.
    if (!cur_target_ && out_w_ > 0 && out_h_ > 0 &&
        (gl_viewport_[2] != out_w_ || gl_viewport_[3] != out_h_ ||
         gl_viewport_[0] != 0 || gl_viewport_[1] != 0)) {
        glViewport(0, 0, out_w_, out_h_);
        gl_viewport_[0] = 0; gl_viewport_[1] = 0;
        gl_viewport_[2] = out_w_; gl_viewport_[3] = out_h_;
    }
    glClearColor(draw_color_[0] / 255.0f, draw_color_[1] / 255.0f,
                 draw_color_[2] / 255.0f, draw_color_[3] / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT_);
}

void GlesRenderBackend::emit_quad(const float x0, const float y0,
                                  const float x1, const float y1,
                                  const float u0, const float v0,
                                  const float u1, const float v1,
                                  const float col[4], GLfloat_* verts)
{
    // 6 顶点（SDL rect_index_order {0,1,2,0,2,3} 的两三角形布局，
    // 顶点序 (minx,miny) (maxx,miny) (maxx,maxy) (minx,maxy)）
    const float xy[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    const float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) {
        const int k = order[i];
        GLfloat_* v = verts + i * 8;
        v[0] = xy[k][0];
        v[1] = xy[k][1];
        v[2] = col[0];
        v[3] = col[1];
        v[4] = col[2];
        v[5] = col[3];
        v[6] = uv[k][0];
        v[7] = uv[k][1];
    }
}

void GlesRenderBackend::fill_rect(const FRect& dst)
{
    if (!gl_context_) return;
    const float col[4] = {draw_color_[0] / 255.0f, draw_color_[1] / 255.0f,
                          draw_color_[2] / 255.0f, draw_color_[3] / 255.0f};
    GLfloat_ verts[6 * 8];
    emit_quad(dst.x, dst.y, dst.x + dst.w, dst.y + dst.h, 0, 0, 0, 0, col,
              verts);
    batch_append(nullptr, draw_blend_, verts, 6);
}

void GlesRenderBackend::draw_texture(TextureRef t, const FRect* src,
                                     const FRect* dst)
{
    if (!gl_context_) return;
    GlesTexture* gt = gles_tex(t);
    if (!gt || !gt->tex || !dst) return;
    const float tw = float(gt->w), th = float(gt->h);
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (src && src->w > 0 && src->h > 0) {
        u0 = src->x / tw; // SDL 共享几何：src rect / 纹理尺寸（float 除法）
        v0 = src->y / th;
        u1 = (src->x + src->w) / tw;
        v1 = (src->y + src->h) / th;
    }
    const float col[4] = {gt->color_mod[0] / 255.0f, gt->color_mod[1] / 255.0f,
                          gt->color_mod[2] / 255.0f,
                          gt->alpha_mod / 255.0f};
    GLfloat_ verts[6 * 8];
    emit_quad(dst->x, dst->y, dst->x + dst->w, dst->y + dst->h, u0, v0, u1, v1,
              col, verts);
    batch_append(gt, gt->blend, verts, 6);
}

void GlesRenderBackend::draw_texture_affine(TextureRef t, const FRect* src,
                                            const FPoint& o, const FPoint& r,
                                            const FPoint& d)
{
    if (!gl_context_) return;
    GlesTexture* gt = gles_tex(t);
    if (!gt || !gt->tex) return;
    const float tw = float(gt->w), th = float(gt->h);
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (src && src->w > 0 && src->h > 0) {
        u0 = src->x / tw;
        v0 = src->y / th;
        u1 = (src->x + src->w) / tw;
        v1 = (src->y + src->h) / th;
    }
    const float col[4] = {gt->color_mod[0] / 255.0f, gt->color_mod[1] / 255.0f,
                          gt->color_mod[2] / 255.0f,
                          gt->alpha_mod / 255.0f};
    // SDL_RenderTextureAffine：o=origin r=right d=down；第 4 角 = r+d-o
    const float qx[4] = {o.x, r.x, r.x + d.x - o.x, d.x};
    const float qy[4] = {o.y, r.y, r.y + d.y - o.y, d.y};
    const float qu[4] = {u0, u1, u1, u0};
    const float qv[4] = {v0, v0, v1, v1};
    GLfloat_ verts[6 * 8];
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) {
        const int k = order[i];
        GLfloat_* v = verts + i * 8;
        v[0] = qx[k]; v[1] = qy[k];
        v[2] = col[0]; v[3] = col[1]; v[4] = col[2]; v[5] = col[3];
        v[6] = qu[k]; v[7] = qv[k];
    }
    batch_append(gt, gt->blend, verts, 6);
}

void GlesRenderBackend::draw_geometry(TextureRef t, const Vertex* verts,
                                      int nverts, const int* indices,
                                      int nindices)
{
    if (!gl_context_) return;
    GlesTexture* gt = gles_tex(t);
    if (!gt || !gt->tex || nverts <= 0 || !verts) return;
    const int count = (indices && nindices > 0) ? nindices : nverts;
    if (count <= 0) return;
    std::vector<GLfloat_> buf(size_t(count) * 8);
    for (int i = 0; i < count; ++i) {
        const int j = indices ? indices[i] : i;
        if (j < 0 || j >= nverts) continue;
        const Vertex& sv = verts[j];
        GLfloat_* v = buf.data() + size_t(i) * 8;
        v[0] = sv.pos.x;
        v[1] = sv.pos.y;
        // 顶点色（emote：白×部件 alpha）；SDL GL 几何路径逐顶点同语义
        v[2] = sv.color.r; v[3] = sv.color.g;
        v[4] = sv.color.b; v[5] = sv.color.a;
        v[6] = sv.uv.x;    v[7] = sv.uv.y;
    }
    batch_append(gt, gt->blend, buf.data(), count);
}

bool GlesRenderBackend::draw_rule_transition(TextureRef capture,
                                             TextureRef rule,
                                             float progress, float band) {
    // type-2 rule 溶解：整幅旧帧以逐像素 keep 为 alpha 叠到当前 target。
    // 只在 FBO（stage 目标）上做——与主转场 overlay 同目标；窗口目标返回
    // false（引擎回退交叉淡化）。坐标数学（scale/off、blend、scissor）经
    // apply_draw_state 复用主 program 路径，随后切到 rule program 仅换
    // uniforms/纹理绑定——GL 状态与 program 无关，几何映射逐像素一致。
    if (!gl_context_ || !cur_target_ || !program_) return false;
    GlesTexture* cap = gles_tex(capture);
    GlesTexture* rl = gles_tex(rule);
    if (!cap || !cap->tex || !rl || !rl->tex) return false;
    if (!ensure_rule_program()) return false;
    flush_batch(); // 排队的场景绘制先于 rule 溶解落地
    apply_draw_state(nullptr, BlendMode::Blend, clip_enabled_, clip_);
    glUseProgram(rule_program_);
    gl_cur_program_ = rule_program_;
    glUniform2f(rule_loc_scale_, last_scale_[0], last_scale_[1]);
    glUniform2f(rule_loc_off_, last_off_[0], last_off_[1]);
    glUniform1f(rule_loc_progress_, progress);
    glUniform1f(rule_loc_band_, band);
    glActiveTexture(GL_TEXTURE0_);
    glBindTexture(GL_TEXTURE_2D_, cap->tex);
    glUniform1i(rule_loc_utex_, 0);
    glActiveTexture(GL_TEXTURE1_);
    glBindTexture(GL_TEXTURE_2D_, rl->tex);
    glUniform1i(rule_loc_urule_, 1);
    glActiveTexture(GL_TEXTURE0_);
    gl_bound_tex_ = cap->tex; // 主 program 路径的绑定缓存同步
    const float col[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // 顶点色未参与 rule 求值
    const float w = float(cur_target_->w), h = float(cur_target_->h);
    GLfloat_ verts[6 * 8];
    emit_quad(0.0f, 0.0f, w, h, 0.0f, 0.0f, 1.0f, 1.0f, col, verts);
    glBindBuffer(GL_ARRAY_BUFFER_, vbo_);
    glBufferData(GL_ARRAY_BUFFER_, sizeof(verts), verts, GL_DYNAMIC_DRAW_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES_, 0, 6);
    glBindVertexArray(0);
    glUseProgram(program_); // 后续 flush 的 program 去重缓存同步
    gl_cur_program_ = program_;
    return true;
}

void GlesRenderBackend::present()
{
    if (!gl_context_) return;
    ++stats_.presents;
    flush_batch();
    ensure_window_ready();
    // GL 错误 canary（OA_RENDER_DIAG 的后端错误面）：仅诊断开关打开时
    // drain——部分 ARM 驱动上 glGetError 触发隐式同步，出厂路径每帧
    // 白付一次（docs/PERFORMANCE_OPTIMIZATION_PLAN.md §1.2）。
    static const bool diag = std::getenv("OA_RENDER_DIAG") != nullptr;
    if (diag) {
        const GLenum_ e = glGetError();
        if (e != GL_NO_ERROR_) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "gles: GL error 0x%x", (unsigned)e);
            err_ = buf; // 保留最后一条（下一个 present 前不再覆盖）
        }
    }
    SDL_GL_SwapWindow(window_);
    // present 后引擎把 target 切回 stage FBO（render_end 尾部）；无需复位。
}

// ---------------------------------------------------------------------------
// 纹理对象
// ---------------------------------------------------------------------------

TextureRef GlesRenderBackend::create_texture(int w, int h,
                                             TextureAccess access)
{
    if (!gl_context_ || w <= 0 || h <= 0 || w > 8192 || h > 8192) return nullptr;
    GlesTexture* gt = new GlesTexture();
    gt->w = w;
    gt->h = h;
    gt->access = access;
    glGenTextures(1, &gt->tex);
    ++stats_.textures_created;
    glBindTexture(GL_TEXTURE_2D_, gt->tex);
    gl_bound_tex_ = gt->tex;
    glTexImage2D(GL_TEXTURE_2D_, 0, GL_RGBA8_, w, h, 0, GL_RGBA_,
                 GL_UNSIGNED_BYTE_, nullptr);
    // 无 mipmap（sdl 线默认 scale mode = LINEAR，SDL GL 驱动同配置）
    glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
    glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_LINEAR_);
    glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
    glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
    if (access == TextureAccess::Streaming) {
        gt->staging.resize(size_t(w) * h * 4);
    } else if (access == TextureAccess::Target) {
        glGenFramebuffers(1, &gt->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER_, gt->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                               GL_TEXTURE_2D_, gt->tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER_) !=
            GL_FRAMEBUFFER_COMPLETE_) {
            fail("gles: framebuffer incomplete (%dx%d %s)", w, h,
                 access == TextureAccess::Target ? "target" : "");
            glBindFramebuffer(GL_FRAMEBUFFER_, 0);
            glDeleteFramebuffers(1, &gt->fbo);
            glDeleteTextures(1, &gt->tex);
            gl_bound_tex_ = 0;
            delete gt;
            return nullptr;
        }
        glBindFramebuffer(GL_FRAMEBUFFER_, 0);
    }
    return gt;
}

void GlesRenderBackend::destroy_texture(TextureRef t)
{
    if (!t) return;
    GlesTexture* gt = static_cast<GlesTexture*>(t);
    if (gt == cur_target_) cur_target_ = nullptr;
    if (batch_tex_ == gt) { // 排队绘制引用了它：先落地再销毁
        flush_batch();
        batch_tex_ = nullptr;
        batch_key_valid_ = false;
    }
    if (gl_context_) { // 后端 shutdown 后的销毁（字形纹理随 FontSystem 晚于
        // release_all 析构）只能释放包装——GL 对象已随上下文销毁。
        // （SDL 端同路径靠 SDL3 对象校验 no-op；GLES 必须显式守卫。）
        if (gt->fbo) glDeleteFramebuffers(1, &gt->fbo);
        if (gt->tex) glDeleteTextures(1, &gt->tex);
        if (gl_bound_tex_ == gt->tex) gl_bound_tex_ = 0;
    }
    delete gt;
}

void GlesRenderBackend::update_texture(TextureRef t, const uint8_t* rgba,
                                       int pitch)
{
    if (!gl_context_) return;
    GlesTexture* gt = gles_tex(t);
    if (!gt || !gt->tex || !rgba) return;
    if (batch_tex_ == gt) flush_batch(); // 排队绘制引用旧内容，先落地
    const int row_bytes = gt->w * 4;
    if (pitch < row_bytes) return;
    ++stats_.texture_uploads;
    stats_.upload_bytes += uint64_t(gt->w) * uint64_t(gt->h) * 4ull;
    glBindTexture(GL_TEXTURE_2D_, gt->tex);
    gl_bound_tex_ = gt->tex;
    glPixelStorei(GL_UNPACK_ALIGNMENT_, 1);
    if (pitch != row_bytes) glPixelStorei(GL_UNPACK_ROW_LENGTH_, pitch / 4);
    glTexSubImage2D(GL_TEXTURE_2D_, 0, 0, 0, gt->w, gt->h, GL_RGBA_,
                    GL_UNSIGNED_BYTE_, rgba);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT_, 4);
}

bool GlesRenderBackend::update_texture_region(TextureRef t, int x, int y,
                                              int w, int h,
                                              const uint8_t* rgba, int pitch)
{
    if (!gl_context_) return false;
    GlesTexture* gt = gles_tex(t);
    if (!gt || !gt->tex || !rgba) return false;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > gt->w || y + h > gt->h)
        return false;
    if (batch_tex_ == gt) flush_batch();
    const int row_bytes = w * 4;
    if (pitch < row_bytes) return false;
    ++stats_.texture_uploads;
    stats_.upload_bytes += uint64_t(w) * uint64_t(h) * 4ull;
    glBindTexture(GL_TEXTURE_2D_, gt->tex);
    gl_bound_tex_ = gt->tex;
    glPixelStorei(GL_UNPACK_ALIGNMENT_, 1);
    if (pitch != row_bytes) glPixelStorei(GL_UNPACK_ROW_LENGTH_, pitch / 4);
    glTexSubImage2D(GL_TEXTURE_2D_, 0, (GLint_)x, (GLint_)y, (GLsizei_)w,
                    (GLsizei_)h, GL_RGBA_, GL_UNSIGNED_BYTE_, rgba);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT_, 4);
    return true;
}

bool GlesRenderBackend::lock_texture(TextureRef t, uint8_t** pixels,
                                     int* pitch)
{
    GlesTexture* gt = gles_tex(t);
    if (!gt || !gt->tex) return false;
    if (gt->staging.empty()) gt->staging.assign(size_t(gt->w) * gt->h * 4, 0);
    *pixels = gt->staging.data();
    *pitch = gt->w * 4;
    return true;
}

void GlesRenderBackend::unlock_texture(TextureRef t)
{
    GlesTexture* gt = gles_tex(t);
    if (!gt || gt->staging.empty()) return;
    update_texture(t, gt->staging.data(), gt->w * 4);
}

bool GlesRenderBackend::texture_size(TextureRef t, float* w, float* h)
{
    GlesTexture* gt = gles_tex(t);
    if (!gt || !gt->tex) return false;
    if (w) *w = float(gt->w);
    if (h) *h = float(gt->h);
    return true;
}

void GlesRenderBackend::set_texture_blend(TextureRef t, BlendMode m)
{
    GlesTexture* gt = gles_tex(t);
    if (gt) gt->blend = m;
}

void GlesRenderBackend::set_texture_alpha_mod(TextureRef t, uint8_t a)
{
    GlesTexture* gt = gles_tex(t);
    if (gt) gt->alpha_mod = a;
}

void GlesRenderBackend::set_texture_color_mod(TextureRef t, uint8_t cr,
                                              uint8_t cg, uint8_t cb)
{
    GlesTexture* gt = gles_tex(t);
    if (gt) {
        gt->color_mod[0] = cr;
        gt->color_mod[1] = cg;
        gt->color_mod[2] = cb;
    }
}

// ---------------------------------------------------------------------------
// 读回 + 坐标
// ---------------------------------------------------------------------------

bool GlesRenderBackend::read_target(int* w, int* h,
                                    std::vector<uint8_t>* rgba)
{
    if (!gl_context_) return false;
    flush_batch(); // 读回必须包含所有已排队绘制
    if (!cur_target_) {
        // 窗口：读 letterbox dst 内容区（SDL 窗口 readback 语义：
        // y 翻行读 + 行序翻转 → top-down）
        if (!out_w_ || !out_h_ || dst_w_ <= 0 || dst_h_ <= 0) return false;
        const int rw = (int)std::ceil(dst_w_);
        const int rh = (int)std::ceil(dst_h_);
        const int rx = (int)std::floor(dst_x_);
        const int ry = (int)std::floor(dst_y_);
        rgba->resize(size_t(rw) * rh * 4);
        std::vector<uint8_t> raw(size_t(rw) * rh * 4);
        glPixelStorei(GL_PACK_ALIGNMENT_, 1);
        glReadPixels(rx, out_h_ - ry - rh, rw, rh, GL_RGBA_,
                     GL_UNSIGNED_BYTE_, raw.data());
        // 行翻转：raw 底→顶 → top-down
        for (int y = 0; y < rh; ++y) {
            std::memcpy(rgba->data() + size_t(y) * rw * 4,
                        raw.data() + size_t(rh - 1 - y) * rw * 4,
                        size_t(rw) * 4);
        }
        if (w) *w = rw;
        if (h) *h = rh;
        return true;
    }
    // FBO：row0 = 内容顶（无需翻转，SDL target readback 语义）
    GlesTexture* gt = cur_target_;
    rgba->resize(size_t(gt->w) * gt->h * 4);
    glPixelStorei(0x0D05, 1);
    glReadPixels(0, 0, gt->w, gt->h, GL_RGBA_, GL_UNSIGNED_BYTE_,
                 rgba->data());
    if (w) *w = gt->w;
    if (h) *h = gt->h;
    return true;
}

bool GlesRenderBackend::window_to_render(float wx, float wy, float* rx,
                                         float* ry)
{
    if (dst_w_ <= 0.0f || dst_h_ <= 0.0f || logical_w_ <= 0 || logical_h_ <= 0)
        return false;
    ensure_window_ready();
    // SDL_RenderCoordinatesFromWindow 数学镜像（main_view viewport 0, scale 1）
    // KR2: window → drawable, then subtract letterbox origin / scale.
    float x = wx * dpi_x_;
    float y = wy * dpi_y_;
    const float lw = float(logical_w_), lh = float(logical_h_);
    x = (x - dst_x_) * lw / dst_w_;
    y = (y - dst_y_) * lh / dst_h_;
    if (rx) *rx = x;
    if (ry) *ry = y;
    return true;
}

bool GlesRenderBackend::render_to_window(float rx, float ry, float* wx,
                                         float* wy)
{
    if (dst_w_ <= 0.0f || dst_h_ <= 0.0f || logical_w_ <= 0 || logical_h_ <= 0)
        return false;
    ensure_window_ready();
    float x = rx, y = ry;
    const float lw = float(logical_w_), lh = float(logical_h_);
    x = dst_x_ + x * dst_w_ / lw;
    y = dst_y_ + y * dst_h_ / lh;
    if (wx) *wx = dpi_x_ > 0.0f ? x / dpi_x_ : x;
    if (wy) *wy = dpi_y_ > 0.0f ? y / dpi_y_ : y;
    return true;
}

void GlesRenderBackend::note_window_size(int w, int h)
{
    if (w > 1 && h > 1) {
        noted_w_ = w;
        noted_h_ = h;
    }
}

bool GlesRenderBackend::present_size(int* w, int* h)
{
    ensure_window_ready();
    if (out_w_ <= 1 || out_h_ <= 1) return false;
    if (w) *w = out_w_;
    if (h) *h = out_h_;
    return true;
}

} // namespace oa::render
