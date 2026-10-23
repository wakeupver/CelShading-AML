// gl_test_support.h - pembantu tes GL tanpa layar.
//
// Memakai EGL "surfaceless" + pbuffer di Mesa (llvmpipe, rendering software) sehingga tes bisa
// jalan di PC/CI tanpa GPU maupun layar. Berisi:
//   * Env       : display + config + pbuffer + context
//   * Image     : hasil glReadPixels, penulis PPM
//   * DumpState : foto seluruh state GL yang bisa disentuh renderer (untuk uji "state kembali persis")
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace gltest {

// ------------------------------------------------------------------ konstanta ES3 / ekstensi
const GLenum kReadFramebufferBinding = 0x8CAA;
const GLenum kReadFramebuffer        = 0x8CA8;
const GLenum kDrawFramebuffer        = 0x8CA9;
const GLenum kSamplerBinding         = 0x8919;
const GLenum kVertexArrayBinding     = 0x85B5;
const GLenum kAttribDivisor          = 0x88FE;
const GLenum kAttribInteger          = 0x88FD;
const GLenum kPixelUnpackBuffer      = 0x88EC;
const GLenum kPixelUnpackBufferBind  = 0x88EF;
const GLenum kRasterizerDiscard      = 0x8C89;

typedef void (*PfnGen)(GLsizei, GLuint*);
typedef void (*PfnBind1)(GLuint);
typedef void (*PfnBind2)(GLuint, GLuint);
typedef void (*PfnSamplerParam)(GLuint, GLenum, GLint);

template <class F> inline F Proc(const char* name) { return reinterpret_cast<F>(eglGetProcAddress(name)); }

// ------------------------------------------------------------------ lingkungan EGL
struct Env
{
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLConfig  cfg = nullptr;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;
    int  w = 0, h = 0;
    int  requestedMajor = 3;
    bool alpha = false;
};

inline EGLDisplay GetDisplay()
{
    static EGLDisplay dpy = EGL_NO_DISPLAY;
    static bool tried = false;
    if (tried) return dpy;
    tried = true;
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (getPlatformDisplay) dpy = getPlatformDisplay(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) { dpy = EGL_NO_DISPLAY; return dpy; }
    eglBindAPI(EGL_OPENGL_ES_API);
    return dpy;
}

// Cari config pbuffer RGB 8-8-8 dengan jumlah bit alpha PERSIS sesuai permintaan
// (Mesa juga menawarkan config 10-bit/float; itu bukan yang dipakai game).
inline bool ChooseConfig(EGLDisplay dpy, int major, bool alpha, EGLConfig* out)
{
    const EGLint attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, major >= 3 ? 0x40 /* EGL_OPENGL_ES3_BIT */ : EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 16, EGL_STENCIL_SIZE, 8,
        EGL_NONE };
    EGLConfig all[128];
    EGLint n = 0;
    if (!eglChooseConfig(dpy, attr, all, 128, &n)) return false;
    for (int i = 0; i < n; ++i)
    {
        EGLint r = 0, g = 0, b = 0, a = -1, samples = 0, type = 0;
        eglGetConfigAttrib(dpy, all[i], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(dpy, all[i], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(dpy, all[i], EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(dpy, all[i], EGL_ALPHA_SIZE, &a);
        eglGetConfigAttrib(dpy, all[i], EGL_SAMPLES, &samples);
        eglGetConfigAttrib(dpy, all[i], 0x3339 /* EGL_COLOR_COMPONENT_TYPE_EXT */, &type);
        const bool fixedPoint = type == 0 || type == 0x333A /* EGL_COLOR_COMPONENT_TYPE_FIXED_EXT */;
        if (r != 8 || g != 8 || b != 8 || samples != 0 || !fixedPoint) continue;
        if ((alpha && a == 8) || (!alpha && a == 0)) { *out = all[i]; return true; }
    }
    return false;
}

inline EGLSurface MakePbuffer(const Env& e, int w, int h)
{
    const EGLint pb[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    return eglCreatePbufferSurface(e.dpy, e.cfg, pb);
}

inline bool CreateContext(Env& e)
{
    const EGLint ctxAttr[] = { 0x3098 /* EGL_CONTEXT_MAJOR_VERSION */, e.requestedMajor, EGL_NONE };
    e.ctx = eglCreateContext(e.dpy, e.cfg, EGL_NO_CONTEXT, ctxAttr);
    return e.ctx != EGL_NO_CONTEXT;
}

// Buat display + pbuffer w x h + context, lalu jadikan current. false kalau ada langkah yang gagal.
inline bool CreateEnv(Env& e, int w, int h, bool alpha, int major)
{
    e.requestedMajor = major;
    e.alpha = alpha;
    e.w = w;
    e.h = h;
    e.dpy = GetDisplay();
    if (e.dpy == EGL_NO_DISPLAY) return false;
    if (!ChooseConfig(e.dpy, major, alpha, &e.cfg)) return false;
    e.surf = MakePbuffer(e, w, h);
    if (e.surf == EGL_NO_SURFACE) return false;
    if (!CreateContext(e)) return false;
    return eglMakeCurrent(e.dpy, e.surf, e.surf, e.ctx) == EGL_TRUE;
}

// Ganti pbuffer dengan ukuran baru (context tetap).
inline bool ResizeSurface(Env& e, int w, int h)
{
    EGLSurface ns = MakePbuffer(e, w, h);
    if (ns == EGL_NO_SURFACE) return false;
    if (!eglMakeCurrent(e.dpy, ns, ns, e.ctx)) return false;
    eglDestroySurface(e.dpy, e.surf);
    e.surf = ns;
    e.w = w;
    e.h = h;
    return true;
}

inline void MakeCurrent(const Env& e) { eglMakeCurrent(e.dpy, e.surf, e.surf, e.ctx); }

inline void DestroyEnv(Env& e)
{
    if (e.dpy == EGL_NO_DISPLAY) return;
    eglMakeCurrent(e.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (e.ctx != EGL_NO_CONTEXT) eglDestroyContext(e.dpy, e.ctx);
    if (e.surf != EGL_NO_SURFACE) eglDestroySurface(e.dpy, e.surf);
    e.ctx = EGL_NO_CONTEXT;
    e.surf = EGL_NO_SURFACE;
}

inline int GlesMajor()
{
    const char* v = (const char*)glGetString(GL_VERSION);
    const char* p = v ? strstr(v, "OpenGL ES") : nullptr;
    if (!p) return 2;
    p += 9;
    while (*p && (*p < '0' || *p > '9')) ++p;
    return (*p >= '0' && *p <= '9') ? (*p - '0') : 2;
}

inline bool HasExt(const char* name)
{
    const char* ext = (const char*)glGetString(GL_EXTENSIONS);
    if (!ext) return false;
    const size_t n = strlen(name);
    for (const char* p = ext; (p = strstr(p, name)) != nullptr; p += n)
        if ((p == ext || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return true;
    return false;
}

// ------------------------------------------------------------------ gambar
struct Image
{
    int w = 0, h = 0;
    std::vector<unsigned char> px;     // RGBA, baris 0 = BAWAH layar (urutan glReadPixels)

    bool empty() const { return px.empty(); }
    const unsigned char* at(int x, int yBottom) const { return &px[((size_t)yBottom * w + x) * 4]; }
    const unsigned char* top(int x, int yTop) const { return at(x, h - 1 - yTop); }    // y dari ATAS
    bool SameAs(const Image& o) const { return w == o.w && h == o.h && px == o.px; }

    // jumlah piksel yang berbeda (kanal RGB, selisih > tol)
    long CountDiff(const Image& o, int tol = 0) const
    {
        if (w != o.w || h != o.h) return -1;
        long n = 0;
        for (size_t i = 0; i < px.size(); i += 4)
        {
            int d = abs((int)px[i] - o.px[i]);
            int d1 = abs((int)px[i + 1] - o.px[i + 1]);
            int d2 = abs((int)px[i + 2] - o.px[i + 2]);
            if (d1 > d) d = d1;
            if (d2 > d) d = d2;
            if (d > tol) ++n;
        }
        return n;
    }
};

inline float Luma(const unsigned char* p) { return (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.0f; }

// Baca framebuffer bawaan (mengubah binding FBO ke 0 - panggil hanya setelah state diperiksa).
inline Image ReadScreen(int w, int h)
{
    Image im;
    im.w = w;
    im.h = h;
    im.px.assign((size_t)w * h * 4, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glFinish();
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, im.px.data());
    return im;
}

inline bool WritePPM(const char* path, const Image& im)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", im.w, im.h);
    for (int y = im.h - 1; y >= 0; --y)
        for (int x = 0; x < im.w; ++x) fwrite(im.at(x, y), 1, 3, f);
    fclose(f);
    return true;
}

// ------------------------------------------------------------------ foto state GL
class Dump
{
public:
    std::string text;

    void Add(const char* fmt, ...)
    {
        char buf[320];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        text += buf;
        text += '\n';
    }
    void I(const char* name, GLenum pname)
    {
        GLint v = 0;
        glGetIntegerv(pname, &v);
        Add("%s=%d", name, v);
    }
    void IV(const char* name, GLenum pname, int n)
    {
        GLint v[4] = { 0, 0, 0, 0 };
        glGetIntegerv(pname, v);
        std::string s;
        for (int i = 0; i < n; ++i) { char b[24]; snprintf(b, sizeof(b), "%d ", v[i]); s += b; }
        Add("%s=%s", name, s.c_str());
    }
    void FV(const char* name, GLenum pname, int n)
    {
        GLfloat v[4] = { 0, 0, 0, 0 };
        glGetFloatv(pname, v);
        std::string s;
        for (int i = 0; i < n; ++i) { char b[32]; snprintf(b, sizeof(b), "%.5g ", v[i]); s += b; }
        Add("%s=%s", name, s.c_str());
    }
    void BV(const char* name, GLenum pname, int n)
    {
        GLboolean v[4] = { 0, 0, 0, 0 };
        glGetBooleanv(pname, v);
        std::string s;
        for (int i = 0; i < n; ++i) s += v[i] ? "1 " : "0 ";
        Add("%s=%s", name, s.c_str());
    }
    void Cap(const char* name, GLenum cap) { Add("%s=%d", name, (int)glIsEnabled(cap)); }
};

inline void DumpAttribs(Dump& d, const char* label, int count, bool es3)
{
    for (int i = 0; i < count; ++i)
    {
        GLint en = 0, size = 0, type = 0, norm = 0, stride = 0, buf = 0, div = 0, isInt = 0;
        void* ptr = nullptr;
        GLfloat cur[4] = { 0, 0, 0, 0 };
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &norm);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
        glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
        glGetVertexAttribfv(i, GL_CURRENT_VERTEX_ATTRIB, cur);
        if (es3)
        {
            glGetVertexAttribiv(i, kAttribDivisor, &div);
            glGetVertexAttribiv(i, kAttribInteger, &isInt);
        }
        d.Add("%s.attrib%d: en=%d size=%d type=0x%X norm=%d stride=%d buf=%d ptr=%p div=%d int=%d cur=%.4g,%.4g,%.4g,%.4g",
              label, i, en, size, type, norm, stride, buf, ptr, div, isInt, cur[0], cur[1], cur[2], cur[3]);
    }
}

// Mengambil foto semua state yang mungkin disentuh post-process. Fungsi ini sendiri memulihkan
// apa pun yang sempat ia ubah (unit tekstur aktif, VAO).
inline std::string DumpState()
{
    Dump d;
    const int major = GlesMajor();
    const bool es3 = major >= 3;
    PfnBind1 bindVao = nullptr;
    if (es3) bindVao = Proc<PfnBind1>("glBindVertexArray");
    else if (HasExt("GL_OES_vertex_array_object")) bindVao = Proc<PfnBind1>("glBindVertexArrayOES");

    d.Add("gles=%d vaoFn=%d", major, bindVao ? 1 : 0);

    // --- tekstur per unit
    GLint activeTex = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
    d.Add("activeTexture=0x%X", activeTex);
    for (int u = 0; u < 4; ++u)
    {
        glActiveTexture(GL_TEXTURE0 + u);
        GLint t2 = 0, tc = 0, smp = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &t2);
        glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &tc);
        if (es3) glGetIntegerv(kSamplerBinding, &smp);
        GLint mn = 0, mg = 0, ws = 0, wt = 0;
        if (t2)
        {
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &mn);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mg);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &ws);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wt);
        }
        d.Add("unit%d: tex2d=%d cube=%d sampler=%d params=%X/%X/%X/%X", u, t2, tc, smp, mn, mg, ws, wt);
    }
    glActiveTexture((GLenum)activeTex);

    // --- binding objek
    d.I("arrayBuffer", GL_ARRAY_BUFFER_BINDING);
    d.I("renderbuffer", GL_RENDERBUFFER_BINDING);
    d.I("drawFbo", GL_FRAMEBUFFER_BINDING);
    if (es3)
    {
        d.I("readFbo", kReadFramebufferBinding);
        d.I("pixelUnpackBuffer", kPixelUnpackBufferBind);
    }
    d.I("program", GL_CURRENT_PROGRAM);

    // --- raster / blending / depth / stencil
    d.IV("viewport", GL_VIEWPORT, 4);
    d.IV("scissor", GL_SCISSOR_BOX, 4);
    d.BV("colorMask", GL_COLOR_WRITEMASK, 4);
    d.BV("depthMask", GL_DEPTH_WRITEMASK, 1);
    d.FV("clearColor", GL_COLOR_CLEAR_VALUE, 4);
    d.FV("clearDepth", GL_DEPTH_CLEAR_VALUE, 1);
    d.I("clearStencil", GL_STENCIL_CLEAR_VALUE);
    d.I("depthFunc", GL_DEPTH_FUNC);
    d.FV("depthRange", GL_DEPTH_RANGE, 2);
    d.I("cullMode", GL_CULL_FACE_MODE);
    d.I("frontFace", GL_FRONT_FACE);
    d.I("blendSrcRGB", GL_BLEND_SRC_RGB);
    d.I("blendDstRGB", GL_BLEND_DST_RGB);
    d.I("blendSrcA", GL_BLEND_SRC_ALPHA);
    d.I("blendDstA", GL_BLEND_DST_ALPHA);
    d.I("blendEqRGB", GL_BLEND_EQUATION_RGB);
    d.I("blendEqA", GL_BLEND_EQUATION_ALPHA);
    d.FV("blendColor", GL_BLEND_COLOR, 4);
    d.FV("lineWidth", GL_LINE_WIDTH, 1);
    d.FV("polyOffsetFactor", GL_POLYGON_OFFSET_FACTOR, 1);
    d.FV("polyOffsetUnits", GL_POLYGON_OFFSET_UNITS, 1);
    d.FV("sampleCoverageValue", GL_SAMPLE_COVERAGE_VALUE, 1);
    d.BV("sampleCoverageInvert", GL_SAMPLE_COVERAGE_INVERT, 1);
    d.I("stencilFunc", GL_STENCIL_FUNC);
    d.I("stencilRef", GL_STENCIL_REF);
    d.I("stencilValueMask", GL_STENCIL_VALUE_MASK);
    d.I("stencilWriteMask", GL_STENCIL_WRITEMASK);
    d.I("stencilFail", GL_STENCIL_FAIL);
    d.I("stencilPassDepthFail", GL_STENCIL_PASS_DEPTH_FAIL);
    d.I("stencilPassDepthPass", GL_STENCIL_PASS_DEPTH_PASS);
    d.I("stencilBackFunc", GL_STENCIL_BACK_FUNC);
    d.I("stencilBackRef", GL_STENCIL_BACK_REF);
    d.I("stencilBackWriteMask", GL_STENCIL_BACK_WRITEMASK);
    d.I("unpackAlignment", GL_UNPACK_ALIGNMENT);
    d.I("packAlignment", GL_PACK_ALIGNMENT);
    d.Cap("BLEND", GL_BLEND);
    d.Cap("CULL_FACE", GL_CULL_FACE);
    d.Cap("DEPTH_TEST", GL_DEPTH_TEST);
    d.Cap("DITHER", GL_DITHER);
    d.Cap("POLYGON_OFFSET_FILL", GL_POLYGON_OFFSET_FILL);
    d.Cap("SCISSOR_TEST", GL_SCISSOR_TEST);
    d.Cap("STENCIL_TEST", GL_STENCIL_TEST);
    d.Cap("SAMPLE_ALPHA_TO_COVERAGE", GL_SAMPLE_ALPHA_TO_COVERAGE);
    d.Cap("SAMPLE_COVERAGE", GL_SAMPLE_COVERAGE);
    if (es3) d.Cap("RASTERIZER_DISCARD", kRasterizerDiscard);

    // --- atribut vertex: VAO yang sedang aktif, lalu VAO bawaan (0) bila berbeda
    GLint vao = 0;
    if (bindVao) glGetIntegerv(kVertexArrayBinding, &vao);
    d.Add("vao=%d", vao);
    d.I("elementBuffer", GL_ELEMENT_ARRAY_BUFFER_BINDING);
    DumpAttribs(d, "current", 8, es3);
    if (bindVao && vao != 0)
    {
        bindVao(0);
        d.I("elementBuffer(vao0)", GL_ELEMENT_ARRAY_BUFFER_BINDING);
        DumpAttribs(d, "vao0", 8, es3);
        bindVao((GLuint)vao);
    }
    return d.text;
}

// Cetak baris-baris yang berbeda antara dua foto state. Mengembalikan jumlah perbedaan.
inline int DiffStates(const std::string& a, const std::string& b, const char* label)
{
    std::vector<std::string> la, lb;
    auto split = [](const std::string& s, std::vector<std::string>& out) {
        size_t i = 0;
        while (i < s.size())
        {
            size_t j = s.find('\n', i);
            if (j == std::string::npos) j = s.size();
            out.push_back(s.substr(i, j - i));
            i = j + 1;
        }
    };
    split(a, la);
    split(b, lb);
    int diffs = 0;
    const size_t n = la.size() > lb.size() ? la.size() : lb.size();
    for (size_t i = 0; i < n; ++i)
    {
        const std::string sa = i < la.size() ? la[i] : "<tidak ada>";
        const std::string sb = i < lb.size() ? lb[i] : "<tidak ada>";
        if (sa != sb)
        {
            ++diffs;
            fprintf(stderr, "  [%s] BEDA:\n      sebelum: %s\n      sesudah: %s\n", label, sa.c_str(), sb.c_str());
        }
    }
    return diffs;
}

} // namespace gltest
