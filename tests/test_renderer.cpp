// test_renderer.cpp - uji renderer cel shading di Mesa (EGL surfaceless, rendering software).
//
// Yang diuji:
//   1. state GL sebelum == sesudah OnSwap (banyak kombinasi state "kotor" milik game)
//   2. hasil gambar tidak bergantung pada state game (piksel identik dengan state bersih)
//   3. matematika shader cocok dengan referensi CPU (cel shading, Sobel, Roberts-cross)
//   4. perilaku: nonaktif, ukuran berubah, context berganti, surface salah, kegagalan GL berulang
//
// Jalankan:  test_renderer [--es2] [--out DIR] [--verbose]
//   --es2  minta context ES 2 (jalankan bersama MESA_GLES_VERSION_OVERRIDE=2.0 agar benar-benar ES 2.0)
//   --out  tulis gambar PPM (raw / hasil) ke DIR untuk dilihat mata
#include "check.h"
#include "gl_test_support.h"
#include "scene.h"
#include "cel_renderer.h"

#include <math.h>
#include <string>
#include <vector>

using namespace gltest;

// ------------------------------------------------------------------ injeksi kegagalan / pelacak error
// (--wrap di perintah link: semua panggilan renderer ke fungsi ini lewat sini)
static bool   g_injectFault = false;   // glCopyTexSubImage2D diberi level = -1 -> GL_INVALID_VALUE
static bool   g_trackErr    = false;
static int    g_errSeen     = 0;
static GLenum g_firstErr    = 0;

extern "C" {
void   __real_glCopyTexSubImage2D(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
GLenum __real_glGetError(void);

void __wrap_glCopyTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (g_injectFault) level = -1;
    __real_glCopyTexSubImage2D(target, level, xo, yo, x, y, w, h);
}

GLenum __wrap_glGetError(void)
{
    GLenum e = __real_glGetError();
    if (e != GL_NO_ERROR && g_trackErr)
    {
        ++g_errSeen;
        if (!g_firstErr) g_firstErr = e;
    }
    return e;
}
}

// ------------------------------------------------------------------ opsi & log
static bool        g_es2 = false;
static bool        g_verbose = false;
static const char* g_outDir = nullptr;

static std::vector<std::string> g_log;
static void LogSink(int level, const char* msg)
{
    g_log.push_back(std::string(level ? "E: " : "I: ") + msg);
    if (g_verbose) printf("      [renderer %s] %s\n", level ? "ERR " : "info", msg);
}
static bool LogHas(const char* needle)
{
    for (const std::string& s : g_log)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

static int Major() { return g_es2 ? 2 : 3; }

static void Section(const char* name) { printf("== %s\n", name); }

// ------------------------------------------------------------------ rig: context + adegan
struct Rig
{
    Env env;
    scene::Scene sc;
    bool Init(int w, int h, bool alpha)
    {
        if (!CreateEnv(env, w, h, alpha, Major())) return false;
        return sc.Init();
    }
    void Shutdown() { DestroyEnv(env); }
};

// Kembalikan semua state ke default GL supaya setiap skenario mulai dari kondisi yang sama.
static void ResetState(const Env& e)
{
    const bool es3 = GlesMajor() >= 3;
    PfnBind1 bindVao = nullptr;
    if (es3) bindVao = Proc<PfnBind1>("glBindVertexArray");
    else if (HasExt("GL_OES_vertex_array_object")) bindVao = Proc<PfnBind1>("glBindVertexArrayOES");
    PfnBind2 bindSampler = es3 ? Proc<PfnBind2>("glBindSampler") : nullptr;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    if (es3) glBindBuffer(kPixelUnpackBuffer, 0);
    if (bindVao) bindVao(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    for (int i = 0; i < 8; ++i) { glDisableVertexAttribArray(i); glVertexAttrib4f(i, 0, 0, 0, 1); }
    glUseProgram(0);

    const GLenum caps[] = { GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_POLYGON_OFFSET_FILL, GL_SCISSOR_TEST,
                            GL_STENCIL_TEST, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE };
    for (GLenum c : caps) glDisable(c);
    glEnable(GL_DITHER);
    glBlendFunc(GL_ONE, GL_ZERO);
    glBlendEquation(GL_FUNC_ADD);
    glBlendColor(0, 0, 0, 0);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDepthRangef(0.0f, 1.0f);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glStencilFunc(GL_ALWAYS, 0, 0xFFFFFFFFu);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilMask(0xFFFFFFFFu);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonOffset(0, 0);
    glSampleCoverage(1.0f, GL_FALSE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    for (int u = 3; u >= 0; --u)
    {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        if (bindSampler) bindSampler(u, 0);
    }
    glClearColor(0, 0, 0, 0);
    glClearDepthf(1.0f);
    glClearStencil(0);
    glViewport(0, 0, e.w, e.h);
    glScissor(0, 0, e.w, e.h);
    while (glGetError() != GL_NO_ERROR) {}
}

// ------------------------------------------------------------------ state "kotor" ala game
struct GameOptions
{
    const char* name;
    bool vao;         // pakai VAO (ES3 / OES_vertex_array_object)
    bool fbo;         // FBO offscreen terpasang saat swap (+ read FBO terpisah di ES3)
    bool pbo;         // PIXEL_UNPACK_BUFFER terpasang (ES3)
    bool clientAttr;  // atribut 0 memakai pointer memori klien (tanpa VBO)
};

struct GameGL
{
    GLuint tex[3] = { 0, 0, 0 }, cube = 0, buf[3] = { 0, 0, 0 }, prog = 0, vao = 0;
    GLuint fboDraw = 0, fboRead = 0, rbo[2] = { 0, 0 }, sampler = 0, pbo = 0;
    PfnGen genVao = nullptr;
    PfnBind1 bindVao = nullptr;
    PfnBind2 bindSampler = nullptr;
    bool es3 = false;
    unsigned char client[256];

    void Create()
    {
        es3 = GlesMajor() >= 3;
        if (es3)
        {
            genVao = Proc<PfnGen>("glGenVertexArrays");
            bindVao = Proc<PfnBind1>("glBindVertexArray");
            bindSampler = Proc<PfnBind2>("glBindSampler");
        }
        else if (HasExt("GL_OES_vertex_array_object"))
        {
            genVao = Proc<PfnGen>("glGenVertexArraysOES");
            bindVao = Proc<PfnBind1>("glBindVertexArrayOES");
        }
        for (int i = 0; i < 256; ++i) client[i] = (unsigned char)(i * 7 + 3);

        static const GLenum mins[3] = { GL_NEAREST, GL_LINEAR, GL_NEAREST };
        static const GLenum mags[3] = { GL_NEAREST, GL_LINEAR, GL_LINEAR };
        static const GLenum wss[3]  = { GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE };
        static const GLenum wts[3]  = { GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_REPEAT };
        unsigned char texels[4 * 4 * 4];
        for (int i = 0; i < 64; ++i) texels[i] = (unsigned char)(i * 4);
        glGenTextures(3, tex);
        for (int u = 0; u < 3; ++u)
        {
            glBindTexture(GL_TEXTURE_2D, tex[u]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mins[u]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mags[u]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wss[u]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wts[u]);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenTextures(1, &cube);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

        float data[64];
        for (int i = 0; i < 64; ++i) data[i] = (float)i;
        glGenBuffers(3, buf);
        for (int i = 0; i < 3; ++i)
        {
            glBindBuffer(GL_ARRAY_BUFFER, buf[i]);
            glBufferData(GL_ARRAY_BUFFER, sizeof(data), data, GL_STATIC_DRAW);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        prog = scene::Link(scene::kObjVS, scene::kObjFS);

        if (genVao) genVao(1, &vao);

        glGenRenderbuffers(2, rbo);
        glGenFramebuffers(1, &fboDraw);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo[0]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, 32, 32);
        glBindFramebuffer(GL_FRAMEBUFFER, fboDraw);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo[0]);
        if (es3)
        {
            glGenFramebuffers(1, &fboRead);
            glBindRenderbuffer(GL_RENDERBUFFER, rbo[1]);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGB565, 16, 16);
            glBindFramebuffer(GL_FRAMEBUFFER, fboRead);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo[1]);
            glGenBuffers(1, &pbo);
            glBindBuffer(kPixelUnpackBuffer, pbo);
            glBufferData(kPixelUnpackBuffer, 16, nullptr, GL_STREAM_DRAW);
            glBindBuffer(kPixelUnpackBuffer, 0);
            PfnGen genSamplers = Proc<PfnGen>("glGenSamplers");
            PfnSamplerParam sp = Proc<PfnSamplerParam>("glSamplerParameteri");
            genSamplers(1, &sampler);
            sp(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            sp(sampler, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
        }
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // Pasang state seperti game di tengah frame, tepat sebelum eglSwapBuffers.
    void Apply(const GameOptions& o)
    {
        // unit tekstur
        for (int u = 0; u < 3; ++u)
        {
            glActiveTexture(GL_TEXTURE0 + u);
            glBindTexture(GL_TEXTURE_2D, tex[u]);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
        if (bindSampler) { bindSampler(0, sampler); bindSampler(2, sampler); }

        // blending, depth, stencil, dll.
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_REVERSE_SUBTRACT);
        glBlendColor(0.1f, 0.2f, 0.3f, 0.4f);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_FALSE);
        glDepthRangef(0.1f, 0.9f);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        glFrontFace(GL_CW);
        glEnable(GL_STENCIL_TEST);
        glStencilFuncSeparate(GL_FRONT, GL_LESS, 3, 0x7F);
        glStencilFuncSeparate(GL_BACK, GL_GEQUAL, 5, 0x3F);
        glStencilOpSeparate(GL_FRONT, GL_INCR, GL_DECR, GL_REPLACE);
        glStencilMaskSeparate(GL_FRONT, 0x0F);
        glStencilMaskSeparate(GL_BACK, 0x07);
        glDisable(GL_DITHER);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.5f, 2.5f);
        glEnable(GL_SAMPLE_COVERAGE);
        glSampleCoverage(0.5f, GL_TRUE);
        glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        glEnable(GL_SCISSOR_TEST);
        glScissor(11, 13, 100, 80);
        glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
        glViewport(3, 5, 200, 100);
        glClearColor(0.1f, 0.2f, 0.3f, 0.4f);
        glClearDepthf(0.25f);
        glClearStencil(7);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 2);

        // atribut vertex
        const bool useVao = o.vao && bindVao && vao;
        if (useVao)
        {
            // isi dulu VAO bawaan (0) dengan sesuatu yang khas, lalu pindah ke VAO game
            bindVao(0);
            glBindBuffer(GL_ARRAY_BUFFER, buf[2]);
            glVertexAttribPointer(0, 2, GL_SHORT, GL_TRUE, 12, (const void*)8);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(5, 3, GL_UNSIGNED_BYTE, GL_FALSE, 7, (const void*)3);
            glEnableVertexAttribArray(5);
            bindVao(vao);
            glBindBuffer(GL_ARRAY_BUFFER, buf[0]);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (const void*)0);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (const void*)12);
            glBindBuffer(GL_ARRAY_BUFFER, buf[2]);
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4, (const void*)0);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glEnableVertexAttribArray(2);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf[1]);
            glVertexAttrib4f(6, 1.0f, 2.0f, 3.0f, 4.0f);
        }
        else
        {
            if (o.clientAttr)
            {
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, client + 16);
                glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 8, client + 4);
            }
            else
            {
                glBindBuffer(GL_ARRAY_BUFFER, buf[0]);
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (const void*)12);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (const void*)0);
            }
            glBindBuffer(GL_ARRAY_BUFFER, buf[2]);
            glVertexAttribPointer(3, 2, GL_SHORT, GL_TRUE, 6, (const void*)2);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glEnableVertexAttribArray(3);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf[1]);
            glVertexAttrib4f(6, 1.0f, 2.0f, 3.0f, 4.0f);
        }
        glBindBuffer(GL_ARRAY_BUFFER, buf[0]);
        glUseProgram(prog);

        if (o.fbo)
        {
            glBindRenderbuffer(GL_RENDERBUFFER, rbo[0]);
            glBindFramebuffer(GL_FRAMEBUFFER, fboDraw);
            if (es3) glBindFramebuffer(kReadFramebuffer, fboRead);
        }
        if (o.pbo && es3) glBindBuffer(kPixelUnpackBuffer, pbo);
        glActiveTexture(GL_TEXTURE2);   // game meninggalkan unit 2 aktif
    }
};

// ------------------------------------------------------------------ satu kali OnSwap + pengamatan
struct SwapResult
{
    Image  out;
    int    stateDiffs = 0;
    int    errorsSeen = 0;
    GLenum firstError = 0;
};

static SwapResult DoSwap(cel::Renderer& r, const Env& e, const char* label)
{
    SwapResult sr;
    while (glGetError() != GL_NO_ERROR) {}
    const std::string before = DumpState();
    while (glGetError() != GL_NO_ERROR) {}

    g_errSeen = 0;
    g_firstErr = 0;
    g_trackErr = true;
    r.OnSwap(e.dpy, e.surf);
    g_trackErr = false;
    sr.errorsSeen = g_errSeen;
    sr.firstError = g_firstErr;

    const std::string after = DumpState();
    sr.stateDiffs = DiffStates(before, after, label);
    sr.out = ReadScreen(e.w, e.h);
    return sr;
}

static Image DrawScene(Rig& rig)
{
    ResetState(rig.env);
    rig.sc.Draw(rig.env.w, rig.env.h);
    return ReadScreen(rig.env.w, rig.env.h);
}

static void SaveImage(const char* name, const Image& im)
{
    if (!g_outDir) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", g_outDir, name);
    if (!WritePPM(path, im)) fprintf(stderr, "  gagal menulis %s\n", path);
}

// ------------------------------------------------------------------ referensi CPU
struct V3 { float x, y, z; };

static float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static float Smooth(float a, float b, float x)
{
    const float t = Clamp01((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static V3 ToonShadeCpu(V3 c, const cel::Settings& s)
{
    const V3 LUMA = { 0.299f, 0.587f, 0.114f };
    const float lum = Dot(c, LUMA);
    const float q = lum * s.bands;
    const float f = floorf(q);
    const float fr = q - f;
    const float hw = s.softness * 0.5f;
    const float sm = Smooth(0.5f - hw, 0.5f + hw, fr);
    float lq = (f + sm) / s.bands;
    lq = s.shadowFloor + (1.0f - s.shadowFloor) * lq;
    const float k = lq / fmaxf(lum, 0.05f);
    V3 t = { c.x * k, c.y * k, c.z * k };
    const float g = Dot(t, LUMA);
    t = { g + (t.x - g) * s.saturation, g + (t.y - g) * s.saturation, g + (t.z - g) * s.saturation };
    t = { t.x * s.brightness, t.y * s.brightness, t.z * s.brightness };
    const float L = s.posterizeLevels;
    V3 p = { floorf(t.x * L + 0.5f) / L, floorf(t.y * L + 0.5f) / L, floorf(t.z * L + 0.5f) / L };
    t = { t.x + (p.x - t.x) * s.posterize, t.y + (p.y - t.y) * s.posterize, t.z + (p.z - t.z) * s.posterize };
    return { Clamp01(t.x), Clamp01(t.y), Clamp01(t.z) };
}

static V3 TexClamp(const Image& im, int x, int yBottom)
{
    if (x < 0) x = 0;
    if (x >= im.w) x = im.w - 1;
    if (yBottom < 0) yBottom = 0;
    if (yBottom >= im.h) yBottom = im.h - 1;
    const unsigned char* p = im.at(x, yBottom);
    return { p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f };
}
static V3 Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 Mul(V3 a, float k) { return { a.x * k, a.y * k, a.z * k }; }

// Kekuatan tepi di piksel (x, y) dengan offset bulat `o` (sampel jatuh persis di pusat texel).
static float EdgeCpu(const Image& im, int x, int y, int o, int mode)
{
    const V3 W = { 0.33f, 0.45f, 0.22f };
    if (mode == 2)
    {
        V3 tl = TexClamp(im, x - o, y + o), tc = TexClamp(im, x, y + o), tr = TexClamp(im, x + o, y + o);
        V3 ml = TexClamp(im, x - o, y),                                   mr = TexClamp(im, x + o, y);
        V3 bl = TexClamp(im, x - o, y - o), bc = TexClamp(im, x, y - o), br = TexClamp(im, x + o, y - o);
        V3 gx = Sub(Add(Add(tr, Mul(mr, 2)), br), Add(Add(tl, Mul(ml, 2)), bl));
        V3 gy = Sub(Add(Add(tl, Mul(tc, 2)), tr), Add(Add(bl, Mul(bc, 2)), br));
        V3 a = { gx.x * gx.x + gy.x * gy.x, gx.y * gx.y + gy.y * gy.y, gx.z * gx.z + gy.z * gy.z };
        return sqrtf(Dot(a, W)) * 0.25f;
    }
    V3 a = TexClamp(im, x - o, y + o), b = TexClamp(im, x + o, y + o);
    V3 c = TexClamp(im, x - o, y - o), d = TexClamp(im, x + o, y - o);
    V3 g1 = Sub(a, d), g2 = Sub(b, c);
    V3 s = { g1.x * g1.x + g2.x * g2.x, g1.y * g1.y + g2.y * g2.y, g1.z * g1.z + g2.z * g2.z };
    return sqrtf(Dot(s, W)) * 0.7071f;
}

static int ToByte(float v) { return (int)floorf(Clamp01(v) * 255.0f + 0.5f); }

// ------------------------------------------------------------------ utilitas gambar
static void ClearRect(int x, int y, int w, int h, float r, float g, float b)
{
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

static void DrawGradient(int w, int h)
{
    static const char* vs = "attribute vec2 aPos; varying vec2 vUV;"
                            "void main(){ vUV = aPos*0.5+0.5; gl_Position = vec4(aPos,0.0,1.0); }";
    static const char* fs = "precision mediump float; varying vec2 vUV;"
                            "void main(){ gl_FragColor = vec4(vec3(vUV.x), 1.0); }";
    GLuint prog = scene::Link(vs, fs);
    static const float tri[6] = { -1, -1, 3, -1, -1, 3 };
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(prog);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void*)0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glDeleteBuffers(1, &vbo);
    glDeleteProgram(prog);
}

static double DarkFraction(const Image& im, float lumaMax)
{
    long n = 0;
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x)
            if (Luma(im.at(x, y)) < lumaMax) ++n;
    return (double)n / ((double)im.w * im.h);
}

// Jumlah level luminans (0..255) yang dipakai oleh minimal `minFrac` dari seluruh piksel.
static int LumaLevels(const Image& im, double minFrac)
{
    std::vector<long> hist(256, 0);
    for (size_t i = 0; i < im.px.size(); i += 4)
        ++hist[(int)(Luma(&im.px[i]) * 255.0f + 0.5f)];
    const double need = minFrac * (double)im.w * im.h;
    int n = 0;
    for (long c : hist) n += (double)c >= need;
    return n;
}

static cel::Settings Plain()    // cel shading saja, tanpa garis
{
    cel::Settings s;
    s.outline = false;
    return s;
}

// ================================================================== TES
static void TestStateRestore()
{
    Section("state GL kembali persis + hasil tidak bergantung pada state game");
    Rig rig;
    CHECK(rig.Init(640, 360, false));
    if (rig.env.ctx == EGL_NO_CONTEXT) return;

    const bool es3 = GlesMajor() >= 3;
    printf("   GL_RENDERER=%s | GL_VERSION=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    // DumpState sendiri tidak boleh menimbulkan GL error (kalau ya, query-nya tidak valid di versi ini)
    while (glGetError() != GL_NO_ERROR) {}
    DumpState();
    CHECK_EQ_INT(glGetError(), GL_NO_ERROR);

    GameGL game;
    game.Create();
    const bool vaoOk = game.bindVao && game.vao;
    printf("   ES%d  VAO=%d  sampler=%d\n", es3 ? 3 : 2, vaoOk ? 1 : 0, game.bindSampler ? 1 : 0);

    std::vector<GameOptions> variants;
    variants.push_back({ "bersih (state apa adanya dari adegan)", false, false, false, false });
    if (vaoOk) variants.push_back({ "VAO + FBO + PBO", true, true, true, false });
    variants.push_back({ "pointer atribut memori klien + FBO", false, true, false, true });
    variants.push_back({ "VBO biasa, tanpa VAO", false, false, es3, false });

    cel::Renderer r;
    r.SetLogger(LogSink);
    r.SetSettings(cel::Settings());

    Image reference;
    unsigned expectFrames = 0;
    for (size_t i = 0; i < variants.size(); ++i)
    {
        const GameOptions& v = variants[i];
        Image raw = DrawScene(rig);
        if (i > 0) game.Apply(v);
        SwapResult sr = DoSwap(r, rig.env, v.name);
        ++expectFrames;
        printf("   - %-42s : beda-state=%d  error-GL=%d (0x%X)\n", v.name, sr.stateDiffs, sr.errorsSeen, sr.firstError);
        CHECK_EQ_INT(sr.stateDiffs, 0);
        CHECK_EQ_INT(sr.errorsSeen, 0);
        CHECK_EQ_INT(r.FramesProcessed(), expectFrames);
        CHECK(!r.DisabledByError());
        if (i == 0)
        {
            reference = sr.out;
            // gambar benar-benar berubah, dan jumlah warna turun drastis (efek cel shading)
            const long changed = sr.out.CountDiff(raw, 8);
            const double frac = (double)changed / ((double)raw.w * raw.h);
            const int lRaw = LumaLevels(raw, 0.0005), lOut = LumaLevels(sr.out, 0.0005);
            printf("     piksel berubah: %.1f%%   level luminans yang dipakai (>=0.05%% piksel): %d -> %d\n", frac * 100, lRaw, lOut);
            CHECK(frac > 0.30);
            CHECK(lOut * 3 < lRaw);        // luminans terkuantisasi jadi sedikit pita
            SaveImage("raw", raw);
            SaveImage("default", sr.out);
        }
        else
        {
            const long d = sr.out.CountDiff(reference, 0);
            if (d != 0) printf("     !! %ld piksel berbeda dari hasil state-bersih\n", d);
            CHECK_EQ_INT(d, 0);
        }
        ResetState(rig.env);
    }
    CHECK(LogHas("efek aktif"));
    rig.Shutdown();
}

static void TestFirstFrameWithGameState()
{
    Section("frame PERTAMA langsung dengan state kotor (pembuatan sumber daya di tengah state game)");
    Rig rig;
    CHECK(rig.Init(640, 360, false));
    GameGL game;
    game.Create();
    const bool vaoOk = game.bindVao && game.vao;

    Image raw = DrawScene(rig);
    GameOptions v = { "kotor-penuh", vaoOk, true, true, false };
    game.Apply(v);
    cel::Renderer r;
    r.SetLogger(LogSink);
    SwapResult sr = DoSwap(r, rig.env, v.name);
    printf("   beda-state=%d  error-GL=%d (0x%X)  frames=%u\n", sr.stateDiffs, sr.errorsSeen, sr.firstError, r.FramesProcessed());
    CHECK_EQ_INT(sr.stateDiffs, 0);
    CHECK_EQ_INT(sr.errorsSeen, 0);
    CHECK_EQ_INT(r.FramesProcessed(), 1);
    CHECK(sr.out.CountDiff(raw, 8) > 0);
    rig.Shutdown();
}

static void TestDisabled()
{
    Section("Enabled=0 -> gambar tidak disentuh sama sekali");
    Rig rig;
    CHECK(rig.Init(640, 360, false));
    cel::Settings s;
    s.enabled = false;
    cel::Renderer r;
    r.SetLogger(LogSink);
    r.SetSettings(s);
    Image raw = DrawScene(rig);
    SwapResult sr = DoSwap(r, rig.env, "nonaktif");
    CHECK(sr.out.SameAs(raw));
    CHECK_EQ_INT(r.FramesProcessed(), 0);
    CHECK_EQ_INT(sr.stateDiffs, 0);

    // hidupkan lagi lewat SetSettings (seperti reload INI) -> mulai bekerja
    s.enabled = true;
    r.SetSettings(s);
    raw = DrawScene(rig);
    sr = DoSwap(r, rig.env, "diaktifkan");
    CHECK(!sr.out.SameAs(raw));
    CHECK_EQ_INT(r.FramesProcessed(), 1);
    rig.Shutdown();
}

static void TestAlphaSurface()
{
    Section("surface dengan alpha vs tanpa alpha");
    Image outNoAlpha, outAlpha;
    for (int a = 0; a < 2; ++a)
    {
        Rig rig;
        if (!rig.Init(640, 360, a == 1))
        {
            printf("   (config %s tidak tersedia - dilewati)\n", a ? "RGBA" : "RGB");
            continue;
        }
        cel::Renderer r;
        r.SetLogger(LogSink);
        DrawScene(rig);
        SwapResult sr = DoSwap(r, rig.env, a ? "alpha" : "tanpa-alpha");
        CHECK_EQ_INT(sr.stateDiffs, 0);
        CHECK_EQ_INT(sr.errorsSeen, 0);
        CHECK_EQ_INT(r.FramesProcessed(), 1);
        (a ? outAlpha : outNoAlpha) = sr.out;
        if (a)
        {
            // alpha sumber (1.0) harus tetap 255 di seluruh layar
            long bad = 0;
            for (size_t i = 3; i < sr.out.px.size(); i += 4) bad += sr.out.px[i] != 255;
            CHECK_EQ_INT(bad, 0);
        }
        rig.Shutdown();
    }
    if (!outAlpha.empty() && !outNoAlpha.empty())
    {
        const long d = outAlpha.CountDiff(outNoAlpha, 0);
        printf("   piksel RGB berbeda antara RGBA dan RGB: %ld\n", d);
        CHECK_EQ_INT(d, 0);
    }
}

static void TestGradientReference()
{
    Section("cel shading == referensi CPU (gradasi abu-abu, beberapa pengaturan)");
    Rig rig;
    CHECK(rig.Init(512, 128, false));

    struct Case { const char* name; float bands, soft, floorV, bright; };
    const Case cases[] = {
        { "bawaan",              5, 0.15f, 0.10f, 1.00f },
        { "4 pita tajam",        4, 0.002f, 0.00f, 1.00f },
        { "8 pita lembut",       8, 0.60f, 0.20f, 1.10f },
    };
    for (const Case& c : cases)
    {
        cel::Settings s = Plain();
        s.bands = c.bands;
        s.softness = c.soft;
        s.shadowFloor = c.floorV;
        s.brightness = c.bright;
        s.saturation = 1.0f;
        s.Clamp();

        ResetState(rig.env);
        DrawGradient(rig.env.w, rig.env.h);
        Image raw = ReadScreen(rig.env.w, rig.env.h);
        cel::Renderer r;
        r.SetLogger(LogSink);
        r.SetSettings(s);
        SwapResult sr = DoSwap(r, rig.env, c.name);

        int worst = 0;
        long bad = 0;
        int levels = 0;
        int count[256] = { 0 };
        for (int x = 0; x < raw.w; ++x)
        {
            const unsigned char* in = raw.at(x, 64);
            const unsigned char* got = sr.out.at(x, 64);
            const float g = in[0] / 255.0f;
            const V3 e = ToonShadeCpu({ g, g, g }, s);
            const int want = ToByte(e.x);
            const int d = abs(want - (int)got[0]);
            if (d > worst) worst = d;
            if (d > 2) ++bad;
            if (got[0] != got[1] || got[1] != got[2]) ++bad;      // abu-abu harus tetap abu-abu
            ++count[got[0]];
        }
        // anak tangga = kelompok level berurutan yang dipakai >= 4 kolom. (Level 0.5*255 = 127.5 jatuh tepat
        // di batas pembulatan sehingga bisa tampil sebagai 127 atau 128; keduanya dihitung satu anak tangga.)
        for (int l = 0; l < 256; ++l)
            if (count[l] >= 4 && (l == 0 || count[l - 1] < 4)) ++levels;
        printf("   %-16s selisih terbesar vs CPU: %d level, jumlah anak tangga: %d\n", c.name, worst, levels);
        CHECK_EQ_INT(bad, 0);
        CHECK_EQ_INT(sr.stateDiffs, 0);
        if (c.soft < 0.01f) CHECK_EQ_INT(levels, (int)c.bands + 1);   // tangga persis bands+1 anak tangga
    }
    rig.Shutdown();
}

static void TestSceneToonReference()
{
    Section("cel shading == referensi CPU pada adegan berwarna (jenuh, bayangan, HUD)");
    Rig rig;
    CHECK(rig.Init(640, 360, false));
    const cel::Settings variants[] = {
        Plain(),
        [] { cel::Settings s = Plain(); s.bands = 3; s.softness = 0.4f; s.saturation = 1.6f; s.brightness = 1.15f; s.Clamp(); return s; }(),
        [] { cel::Settings s = Plain(); s.posterize = 0.8f; s.posterizeLevels = 4; s.shadowFloor = 0.25f; s.saturation = 0.6f; s.Clamp(); return s; }(),
    };
    int idx = 0;
    for (const cel::Settings& s : variants)
    {
        Image raw = DrawScene(rig);
        cel::Renderer r;
        r.SetLogger(LogSink);
        r.SetSettings(s);
        SwapResult sr = DoSwap(r, rig.env, "adegan");
        long bad = 0;
        int worst = 0;
        for (int y = 0; y < raw.h; ++y)
            for (int x = 0; x < raw.w; ++x)
            {
                const unsigned char* in = raw.at(x, y);
                const unsigned char* got = sr.out.at(x, y);
                const V3 e = ToonShadeCpu({ in[0] / 255.0f, in[1] / 255.0f, in[2] / 255.0f }, s);
                const int w[3] = { ToByte(e.x), ToByte(e.y), ToByte(e.z) };
                int d = 0;
                for (int c = 0; c < 3; ++c) d = d > abs(w[c] - (int)got[c]) ? d : abs(w[c] - (int)got[c]);
                if (d > worst) worst = d;
                if (d > 3) ++bad;
            }
        const double frac = (double)bad / ((double)raw.w * raw.h);
        printf("   variasi %d: piksel di luar toleransi 3 level = %ld (%.4f%%), selisih terbesar = %d\n", idx, bad, frac * 100, worst);
        CHECK(frac < 0.001);
        CHECK_EQ_INT(sr.stateDiffs, 0);
        ++idx;
    }
    rig.Shutdown();
}

static void TestEdgeReference()
{
    Section("peta garis (debug 1) == referensi CPU: Sobel (quality 1) dan Roberts (quality 0)");
    Rig rig;
    CHECK(rig.Init(640, 360, false));
    // tinggi 360 -> offset = lebar * 0.5 px; lebar 2.0 -> offset 1 px tepat (sampel jatuh di pusat texel)
    for (int quality = 1; quality >= 0; --quality)
    {
        cel::Settings s;
        s.quality = quality;
        s.debugView = 1;
        s.outlineWidth = 2.0f;
        s.Clamp();
        Image raw = DrawScene(rig);
        cel::Renderer r;
        r.SetLogger(LogSink);
        r.SetSettings(s);
        SwapResult sr = DoSwap(r, rig.env, "garis");

        long bad = 0;
        int worst = 0;
        for (int y = 0; y < raw.h; ++y)
            for (int x = 0; x < raw.w; ++x)
            {
                const float e = EdgeCpu(raw, x, y, 1, quality ? 2 : 1);
                const float v = 1.0f - Smooth(s.outlineThreshold, s.outlineThreshold + s.outlineSoftness, e);
                const int want = ToByte(v);
                const unsigned char* got = sr.out.at(x, y);
                const int d = abs(want - (int)got[0]);
                if (d > worst) worst = d;
                if (d > 3 || got[0] != got[1] || got[1] != got[2]) ++bad;
            }
        const double frac = (double)bad / ((double)raw.w * raw.h);
        const double dark = DarkFraction(sr.out, 0.5f);
        printf("   quality=%d: piksel di luar toleransi = %ld (%.4f%%), selisih terbesar = %d, piksel garis = %.2f%%\n",
               quality, bad, frac * 100, worst, dark * 100);
        CHECK(frac < 0.001);
        CHECK(dark > 0.002 && dark < 0.30);
        SaveImage(quality ? "debug_edges_sobel" : "debug_edges_fast", sr.out);
        CHECK_EQ_INT(sr.stateDiffs, 0);
    }
    rig.Shutdown();
}

static float RowMinLuma(const Image& im, int yTop, int x0, int x1)
{
    float m = 1.0f;
    for (int x = x0; x < x1; ++x) m = fminf(m, Luma(im.top(x, yTop)));
    return m;
}
static int RowDarkCount(const Image& im, int yTop, int x0, int x1, float thr)
{
    int n = 0;
    for (int x = x0; x < x1; ++x) n += Luma(im.top(x, yTop)) < thr;
    return n;
}

static void TestOutlineBehaviour()
{
    Section("perilaku garis: posisi, lebar, ambang, kontras rendah, warna garis, kekuatan");
    Rig rig;
    CHECK(rig.Init(640, 360, false));
    const int B = 320;     // batas kiri/kanan

    auto run = [&](const cel::Settings& s, V3 left, V3 right) {
        ResetState(rig.env);
        ClearRect(0, 0, B, 360, left.x, left.y, left.z);
        ClearRect(B, 0, 640 - B, 360, right.x, right.y, right.z);
        cel::Renderer r;
        r.SetLogger(LogSink);
        r.SetSettings(s);
        SwapResult sr = DoSwap(r, rig.env, "garis");
        CHECK_EQ_INT(sr.stateDiffs, 0);
        return sr.out;
    };

    // 1) kontras tinggi: garis tepat di batas, tinggi penuh, area datar tidak tersentuh
    {
        cel::Settings s;
        Image out = run(s, { 0.2f, 0.2f, 0.2f }, { 0.8f, 0.8f, 0.8f });
        Image toonOnly;
        {
            cel::Settings t = s;
            t.debugView = 2;
            toonOnly = run(t, { 0.2f, 0.2f, 0.2f }, { 0.8f, 0.8f, 0.8f });
        }
        for (int yTop : { 0, 90, 180, 359 })
        {
            CHECK(RowMinLuma(out, yTop, B - 3, B + 3) < 0.12f);          // ada garis gelap di batas
            CHECK(RowDarkCount(out, yTop, B - 8, B + 8, 0.15f) <= 4);    // tipis di lebar bawaan
        }
        // di luar pita sempit sekitar batas, hasil harus persis sama dengan "hanya cel shading"
        long diffOutside = 0;
        for (int y = 0; y < 360; ++y)
            for (int x = 0; x < 640; ++x)
                if (abs(x - B) > 4 && memcmp(out.top(x, y), toonOnly.top(x, y), 3) != 0) ++diffOutside;
        printf("   garis kontras tinggi: piksel berbeda di luar pita batas = %ld\n", diffOutside);
        CHECK_EQ_INT(diffOutside, 0);
        SaveImage("edge_step", out);
    }

    // 2) lebar garis tidak menyusut dan bertambah dengan OutlineWidth
    //    (pada tepi tajam lebar naik bertahap karena ambang: 1 -> 2 px, 6 -> 6 px, dst.)
    {
        const float widths[4] = { 1.0f, 2.0f, 4.0f, 6.0f };
        int width[4];
        for (int i = 0; i < 4; ++i)
        {
            cel::Settings s;
            s.outlineWidth = widths[i];
            s.Clamp();
            Image out = run(s, { 0.2f, 0.2f, 0.2f }, { 0.8f, 0.8f, 0.8f });
            width[i] = RowDarkCount(out, 180, B - 30, B + 30, 0.15f);
        }
        printf("   lebar garis (px gelap) untuk OutlineWidth 1/2/4/6: %d / %d / %d / %d\n", width[0], width[1], width[2], width[3]);
        CHECK(width[0] >= 1);
        CHECK(width[1] >= width[0]);
        CHECK(width[2] >= width[1]);
        CHECK(width[3] >= width[2]);
        CHECK(width[3] >= width[0] + 3);
    }

    // 3) kontras rendah di bawah ambang -> tanpa garis
    {
        cel::Settings s;
        Image out = run(s, { 0.50f, 0.50f, 0.50f }, { 0.55f, 0.55f, 0.55f });
        CHECK(RowMinLuma(out, 180, 0, 640) > 0.35f);
        // ambang diturunkan -> garis muncul
        s.outlineThreshold = 0.02f;
        s.outlineSoftness = 0.05f;
        s.Clamp();
        out = run(s, { 0.50f, 0.50f, 0.50f }, { 0.55f, 0.55f, 0.55f });
        const float mn = RowMinLuma(out, 180, B - 3, B + 3), far = Luma(out.top(100, 180));
        printf("   kontras rendah, ambang 0.02: luma di batas %.3f vs jauh dari batas %.3f\n", mn, far);
        CHECK(mn < far - 0.05f);
    }

    // 4) tepi hanya-warna (merah murni vs hijau murni) ikut terdeteksi
    {
        cel::Settings s;
        Image out = run(s, { 0.9f, 0.1f, 0.1f }, { 0.1f, 0.65f, 0.1f });
        const float mn = RowMinLuma(out, 180, B - 3, B + 3);
        printf("   tepi warna (merah|hijau): luma minimum di batas = %.3f\n", mn);
        CHECK(mn < 0.12f);
    }

    // 5) warna garis & kekuatan
    {
        cel::Settings s;
        s.outlineColor[0] = 1.0f; s.outlineColor[1] = 0.0f; s.outlineColor[2] = 0.0f;
        Image out = run(s, { 0.2f, 0.2f, 0.2f }, { 0.8f, 0.8f, 0.8f });
        // piksel tepi: merah dominan
        int bestX = B - 1;
        float bestRed = -1;
        for (int x = B - 3; x < B + 3; ++x)
        {
            const unsigned char* p = out.top(x, 180);
            const float red = (float)p[0] - 0.5f * (p[1] + p[2]);
            if (red > bestRed) { bestRed = red; bestX = x; }
        }
        const unsigned char* p = out.top(bestX, 180);
        printf("   warna garis merah: piksel tepi = (%d,%d,%d)\n", p[0], p[1], p[2]);
        CHECK(p[0] > p[1] + 30 && p[0] > p[2] + 30);

        s.outlineColor[0] = s.outlineColor[1] = s.outlineColor[2] = 0.0f;
        s.outlineStrength = 0.0f;
        out = run(s, { 0.2f, 0.2f, 0.2f }, { 0.8f, 0.8f, 0.8f });
        CHECK(RowMinLuma(out, 180, B - 3, B + 3) > 0.25f);       // kekuatan 0 -> tidak ada garis
        s.outlineStrength = 0.5f;
        out = run(s, { 0.2f, 0.2f, 0.2f }, { 0.8f, 0.8f, 0.8f });
        const float half = RowMinLuma(out, 180, B - 3, B + 3);
        CHECK(half > 0.04f && half < 0.25f);                     // setengah pekat -> abu-abu gelap
    }

    // 6) outline dimatikan -> tidak ada garis, dan sama dengan debug 2
    {
        cel::Settings s = Plain();
        Image off = run(s, { 0.2f, 0.2f, 0.2f }, { 0.8f, 0.8f, 0.8f });
        CHECK(RowMinLuma(off, 180, B - 3, B + 3) > 0.25f);
        cel::Settings t;
        t.debugView = 2;
        Image dbg = run(t, { 0.2f, 0.2f, 0.2f }, { 0.8f, 0.8f, 0.8f });
        CHECK(off.SameAs(dbg));
    }
    rig.Shutdown();
}

static void TestScenePlausibility()
{
    Section("adegan lengkap: garis wajar (tidak kosong, tidak menutup layar)");
    Rig rig;
    CHECK(rig.Init(960, 540, false));
    for (int quality = 0; quality < 2; ++quality)
    {
        cel::Settings s;
        s.quality = quality;
        Image raw = DrawScene(rig);
        cel::Renderer r;
        r.SetLogger(LogSink);
        r.SetSettings(s);
        SwapResult sr = DoSwap(r, rig.env, "adegan");
        Image toon;
        {
            cel::Settings t = Plain();
            t.quality = quality;
            DrawScene(rig);
            cel::Renderer r2;
            r2.SetSettings(t);
            toon = DoSwap(r2, rig.env, "toon").out;
        }
        // piksel gelap baru yang muncul karena garis = (gelap di hasil) - (gelap di hanya-toon)
        const double extra = DarkFraction(sr.out, 0.10f) - DarkFraction(toon, 0.10f);
        printf("   quality=%d: piksel gelap tambahan karena garis = %.2f%%\n", quality, extra * 100);
        CHECK(extra > 0.003 && extra < 0.12);
        CHECK_EQ_INT(sr.stateDiffs, 0);
        if (quality == 0) SaveImage("default_fast", sr.out);
    }
    rig.Shutdown();
}

static void TestResizeAndContextSwitch()
{
    Section("ukuran surface berubah, context berganti, OnContextDestroyed");
    Rig a;
    CHECK(a.Init(640, 360, false));
    cel::Renderer r;
    r.SetLogger(LogSink);

    Image raw = DrawScene(a);
    SwapResult sr = DoSwap(r, a.env, "ctx-A");
    CHECK(!sr.out.SameAs(raw));
    CHECK_EQ_INT(r.FramesProcessed(), 1);
    Image ref640 = sr.out;

    // 1) ukuran surface berubah (context sama)
    CHECK(ResizeSurface(a.env, 800, 450));
    raw = DrawScene(a);
    sr = DoSwap(r, a.env, "resize");
    CHECK_EQ_INT(sr.errorsSeen, 0);
    CHECK(sr.out.w == 800 && sr.out.h == 450);
    CHECK(sr.out.CountDiff(raw, 8) > 0);
    CHECK_EQ_INT(r.FramesProcessed(), 2);
    CHECK_EQ_INT(sr.stateDiffs, 0);

    // 2) context lain di thread yang sama: sumber daya lama dilepas (tanpa glDelete) lalu dibuat ulang
    Rig b;
    CHECK(b.Init(512, 288, false));
    const size_t logsBefore = g_log.size();
    raw = DrawScene(b);
    sr = DoSwap(r, b.env, "ctx-B");
    CHECK_EQ_INT(sr.errorsSeen, 0);
    CHECK(sr.out.CountDiff(raw, 8) > 0);
    CHECK_EQ_INT(r.FramesProcessed(), 3);
    bool relogged = false;
    for (size_t i = logsBefore; i < g_log.size(); ++i) relogged = relogged || g_log[i].find("GL:") != std::string::npos;
    CHECK(relogged);       // caps di-query ulang untuk context baru

    // 3) kembali ke context A
    MakeCurrent(a.env);
    raw = DrawScene(a);
    sr = DoSwap(r, a.env, "kembali-ke-A");
    CHECK_EQ_INT(sr.errorsSeen, 0);
    CHECK(sr.out.CountDiff(raw, 8) > 0);
    CHECK_EQ_INT(r.FramesProcessed(), 4);

    // 4) context dihancurkan -> OnContextDestroyed; context baru sesudahnya bekerja
    r.OnContextDestroyed(a.env.ctx);
    a.Shutdown();
    Rig c;
    CHECK(c.Init(640, 360, false));
    raw = DrawScene(c);
    sr = DoSwap(r, c.env, "ctx-C");
    CHECK_EQ_INT(sr.errorsSeen, 0);
    CHECK(sr.out.SameAs(ref640));      // hasil sama dengan frame pertama di context A (deterministik)
    CHECK_EQ_INT(r.FramesProcessed(), 5);

    // 5) display di-terminate
    r.OnDisplayTerminated();
    raw = DrawScene(c);
    sr = DoSwap(r, c.env, "setelah-terminate");
    CHECK(sr.out.SameAs(ref640));
    CHECK_EQ_INT(r.FramesProcessed(), 6);
    b.Shutdown();
    c.Shutdown();
}

static void TestGuards()
{
    Section("penjaga: surface salah, surface kecil, tanpa context");
    Rig rig;
    CHECK(rig.Init(640, 360, false));
    cel::Renderer r;
    r.SetLogger(LogSink);

    // surface lain yang bukan surface gambar context saat ini
    EGLSurface other = MakePbuffer(rig.env, 64, 64);
    Image raw = DrawScene(rig);
    while (glGetError() != GL_NO_ERROR) {}
    r.OnSwap(rig.env.dpy, other);
    Image after = ReadScreen(640, 360);
    CHECK(after.SameAs(raw));
    CHECK_EQ_INT(r.FramesProcessed(), 0);
    eglDestroySurface(rig.env.dpy, other);

    // display salah
    r.OnSwap((EGLDisplay)0x1234, rig.env.surf);
    CHECK_EQ_INT(r.FramesProcessed(), 0);

    // surface kecil (<16 px) diabaikan
    CHECK(ResizeSurface(rig.env, 8, 8));
    DrawScene(rig);
    r.OnSwap(rig.env.dpy, rig.env.surf);
    CHECK_EQ_INT(r.FramesProcessed(), 0);

    // tanpa context current
    eglMakeCurrent(rig.env.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    r.OnSwap(rig.env.dpy, rig.env.surf);
    CHECK_EQ_INT(r.FramesProcessed(), 0);
    MakeCurrent(rig.env);

    // setelah semua itu renderer tetap sehat di surface normal
    CHECK(ResizeSurface(rig.env, 640, 360));
    raw = DrawScene(rig);
    SwapResult sr = DoSwap(r, rig.env, "pulih");
    CHECK(!sr.out.SameAs(raw));
    CHECK_EQ_INT(r.FramesProcessed(), 1);
    rig.Shutdown();
}

static void TestFailureAutoDisable()
{
    Section("kegagalan GL berulang -> efek mati otomatis, layar tetap utuh");
    Rig rig;
    CHECK(rig.Init(640, 360, false));
    cel::Renderer r;
    r.SetLogger(LogSink);

    // satu frame sehat dulu
    DrawScene(rig);
    DoSwap(r, rig.env, "sehat");
    CHECK_EQ_INT(r.FramesProcessed(), 1);

    g_injectFault = true;
    for (int i = 1; i <= 3; ++i)
    {
        Image raw = DrawScene(rig);
        SwapResult sr = DoSwap(r, rig.env, "gagal");
        printf("   frame gagal %d: state-beda=%d  layar=%s  dimatikan=%d\n", i, sr.stateDiffs,
               sr.out.SameAs(raw) ? "utuh (tidak ditimpa)" : "BERUBAH", r.DisabledByError() ? 1 : 0);
        CHECK_EQ_INT(sr.stateDiffs, 0);
        CHECK(sr.out.SameAs(raw));                       // salinan gagal -> jangan menimpa layar dengan tekstur basi
        CHECK_EQ_INT(r.DisabledByError(), i >= 3);
    }
    g_injectFault = false;
    CHECK(LogHas("dimatikan otomatis"));
    CHECK_EQ_INT(r.FramesProcessed(), 1);

    // sesudah mati, tidak memproses lagi
    Image raw = DrawScene(rig);
    SwapResult sr = DoSwap(r, rig.env, "sesudah-mati");
    CHECK(sr.out.SameAs(raw));
    CHECK_EQ_INT(r.FramesProcessed(), 1);

    // dua kegagalan yang disusul frame sehat tidak boleh mematikan (hitungan berurutan di-reset)
    cel::Renderer r2;
    r2.SetLogger(LogSink);
    for (int i = 0; i < 2; ++i)
    {
        g_injectFault = true;
        DrawScene(rig);
        DoSwap(r2, rig.env, "gagal-2x");
        g_injectFault = false;
    }
    DrawScene(rig);
    DoSwap(r2, rig.env, "sehat-lagi");
    g_injectFault = true;
    DrawScene(rig);
    DoSwap(r2, rig.env, "gagal-lagi");
    g_injectFault = false;
    CHECK(!r2.DisabledByError());
    CHECK_EQ_INT(r2.FramesProcessed(), 1);
    rig.Shutdown();
}

static void TestSmallResolutions()
{
    Section("berbagai resolusi (termasuk rasio ganjil) tanpa error");
    const int sizes[][2] = { { 16, 16 }, { 17, 33 }, { 320, 240 }, { 854, 480 }, { 1280, 720 }, { 1920, 1080 }, { 2400, 1080 } };
    for (const auto& sz : sizes)
    {
        Rig rig;
        if (!rig.Init(sz[0], sz[1], false)) { printf("   %dx%d: gagal membuat surface\n", sz[0], sz[1]); CHECK(false); continue; }
        cel::Renderer r;
        r.SetLogger(LogSink);
        Image raw = DrawScene(rig);
        SwapResult sr = DoSwap(r, rig.env, "resolusi");
        CHECK_EQ_INT(sr.errorsSeen, 0);
        CHECK_EQ_INT(sr.stateDiffs, 0);
        CHECK_EQ_INT(r.FramesProcessed(), 1);
        CHECK(sr.out.CountDiff(raw, 0) > 0);
        rig.Shutdown();
    }
}

// ------------------------------------------------------------------ galeri untuk dilihat mata
static void RenderGallery()
{
    if (!g_outDir) return;
    Section("galeri (PPM) -> ditulis ke --out");
    Rig rig;
    if (!rig.Init(960, 540, false)) return;
    struct Preset { const char* name; cel::Settings s; };
    std::vector<Preset> presets;
    { cel::Settings s; presets.push_back({ "g_default", s }); }
    { cel::Settings s; s.debugView = 1; presets.push_back({ "g_edges", s }); }
    { cel::Settings s; s.debugView = 2; presets.push_back({ "g_toononly", s }); }
    { cel::Settings s = Plain(); presets.push_back({ "g_nooutline", s }); }
    { cel::Settings s; s.outlineWidth = 2.0f; presets.push_back({ "g_thick", s }); }
    { cel::Settings s; s.bands = 3; s.softness = 0.08f; s.outlineThreshold = 0.22f; presets.push_back({ "g_hard3", s }); }
    { cel::Settings s; s.bands = 8; s.softness = 0.35f; s.shadowFloor = 0.2f; s.outlineStrength = 0.8f; presets.push_back({ "g_soft8", s }); }
    { cel::Settings s; s.posterize = 0.6f; s.posterizeLevels = 5; presets.push_back({ "g_poster", s }); }
    for (Preset& p : presets)
    {
        p.s.Clamp();
        DrawScene(rig);
        cel::Renderer r;
        r.SetSettings(p.s);
        SwapResult sr = DoSwap(r, rig.env, p.name);
        SaveImage(p.name, sr.out);
        printf("   %s\n", p.name);
    }
    rig.Shutdown();
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--es2")) g_es2 = true;
        else if (!strcmp(argv[i], "--verbose")) g_verbose = true;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) g_outDir = argv[++i];
        else { fprintf(stderr, "opsi tidak dikenal: %s\n", argv[i]); return 2; }
    }
    if (GetDisplay() == EGL_NO_DISPLAY)
    {
        printf("[renderer] EGL tidak tersedia di mesin ini - tes dilewati\n");
        return 0;
    }
    printf("test_renderer: meminta context ES %d\n", Major());

    TestStateRestore();
    TestFirstFrameWithGameState();
    TestDisabled();
    TestAlphaSurface();
    TestGradientReference();
    TestSceneToonReference();
    TestEdgeReference();
    TestOutlineBehaviour();
    TestScenePlausibility();
    TestResizeAndContextSwitch();
    TestGuards();
    TestFailureAutoDisable();
    TestSmallResolutions();
    RenderGallery();
    return TestSummary(g_es2 ? "renderer-es2" : "renderer-es3");
}
