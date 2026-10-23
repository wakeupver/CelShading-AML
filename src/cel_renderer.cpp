#include "cel_renderer.h"

#include <GLES2/gl2ext.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace cel {
namespace {

// ---------------------------------------------------------------------------------------
// Shader (GLSL ES 1.00 - jalan di semua GPU Android sejak ES2)
// ---------------------------------------------------------------------------------------
const char* const kVertexShader =
    "attribute vec2 aPos;\n"
    "varying vec2 vUV;\n"
    "void main()\n"
    "{\n"
    "    vUV = aPos * 0.5 + 0.5;\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

// EDGE_MODE: 0 = tanpa garis, 1 = cepat (4 sampel diagonal), 2 = Sobel (8 sampel)
const char* const kFragmentShader = R"GLSL(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 vUV;

uniform sampler2D uScene;
uniform vec2  uTexel;       // 1/lebar, 1/tinggi
uniform vec4  uToonA;       // x: jumlah pita, y: kehalusan, z: lantai bayangan, w: saturasi
uniform vec4  uToonB;       // x: kecerahan, y: campuran posterisasi, z: level posterisasi
uniform vec4  uEdgeA;       // x: jarak sampel (px), y: ambang, z: kehalusan, w: kekuatan
uniform vec3  uEdgeColor;
uniform float uDebug;       // 0 normal, 1 peta garis, 2 hanya cel shading

const vec3 LUMA   = vec3(0.299, 0.587, 0.114);
const vec3 EDGE_W = vec3(0.33, 0.45, 0.22);   // bobot kanal untuk kontras warna (jumlah = 1)

vec3 toonShade(vec3 c)
{
    float lum  = dot(c, LUMA);
    float q    = lum * uToonA.x;
    float f    = floor(q);
    float fr   = q - f;
    float hw   = uToonA.y * 0.5;
    float s    = smoothstep(0.5 - hw, 0.5 + hw, fr);
    float lq   = (f + s) / uToonA.x;               // luminans terkuantisasi
    lq = mix(uToonA.z, 1.0, lq);                   // angkat bayangan
    vec3 t = c * (lq / max(lum, 0.05));            // pertahankan rona
    float g = dot(t, LUMA);
    t = mix(vec3(g), t, uToonA.w);                 // saturasi
    t *= uToonB.x;                                 // kecerahan
    vec3 p = floor(t * uToonB.z + 0.5) / uToonB.z; // posterisasi per-kanal
    t = mix(t, p, uToonB.y);
    return clamp(t, 0.0, 1.0);
}

#if EDGE_MODE == 2
float edgeStrength(vec2 uv)
{
    vec2 o = uTexel * uEdgeA.x;
    vec3 tl = texture2D(uScene, uv + vec2(-o.x,  o.y)).rgb;
    vec3 tc = texture2D(uScene, uv + vec2( 0.0,  o.y)).rgb;
    vec3 tr = texture2D(uScene, uv + vec2( o.x,  o.y)).rgb;
    vec3 ml = texture2D(uScene, uv + vec2(-o.x,  0.0)).rgb;
    vec3 mr = texture2D(uScene, uv + vec2( o.x,  0.0)).rgb;
    vec3 bl = texture2D(uScene, uv + vec2(-o.x, -o.y)).rgb;
    vec3 bc = texture2D(uScene, uv + vec2( 0.0, -o.y)).rgb;
    vec3 br = texture2D(uScene, uv + vec2( o.x, -o.y)).rgb;
    vec3 gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);
    vec3 gy = (tl + 2.0 * tc + tr) - (bl + 2.0 * bc + br);
    return sqrt(dot(gx * gx + gy * gy, EDGE_W)) * 0.25;
}
#elif EDGE_MODE == 1
float edgeStrength(vec2 uv)
{
    vec2 o = uTexel * uEdgeA.x;
    vec3 a = texture2D(uScene, uv + vec2(-o.x,  o.y)).rgb;
    vec3 b = texture2D(uScene, uv + vec2( o.x,  o.y)).rgb;
    vec3 c = texture2D(uScene, uv + vec2(-o.x, -o.y)).rgb;
    vec3 d = texture2D(uScene, uv + vec2( o.x, -o.y)).rgb;
    vec3 g1 = a - d;
    vec3 g2 = b - c;
    return sqrt(dot(g1 * g1 + g2 * g2, EDGE_W)) * 0.7071;
}
#else
float edgeStrength(vec2 uv) { return 0.0; }
#endif

void main()
{
    vec4 src = texture2D(uScene, vUV);
    vec3 col = toonShade(src.rgb);
    if (uDebug > 1.5) { gl_FragColor = vec4(col, src.a); return; }

    float e = smoothstep(uEdgeA.y, uEdgeA.y + uEdgeA.z, edgeStrength(vUV));
    if (uDebug > 0.5) { gl_FragColor = vec4(vec3(1.0 - e), 1.0); return; }

    col = mix(col, uEdgeColor, e * uEdgeA.w);
    gl_FragColor = vec4(col, src.a);
}
)GLSL";

// Konstanta yang tidak ada di header ES2.
const GLenum kReadFramebufferBinding = 0x8CAA;   // GL_READ_FRAMEBUFFER_BINDING (ES3)
const GLenum kReadFramebuffer        = 0x8CA8;   // GL_READ_FRAMEBUFFER (ES3)
const GLenum kDrawFramebuffer        = 0x8CA9;   // GL_DRAW_FRAMEBUFFER (ES3)
const GLenum kSamplerBinding         = 0x8919;   // GL_SAMPLER_BINDING (ES3)
const GLenum kVertexArrayBinding     = 0x85B5;   // GL_VERTEX_ARRAY_BINDING(_OES)
const GLenum kPixelUnpackBuffer      = 0x88EC;   // GL_PIXEL_UNPACK_BUFFER (ES3)
const GLenum kPixelUnpackBufferBind  = 0x88EF;   // GL_PIXEL_UNPACK_BUFFER_BINDING (ES3)

const int kMaxAttribs = 16;

// Semua kemampuan on/off yang bisa memengaruhi gambar layar-penuh kita.
const GLenum kToggles[] = {
    GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_DITHER, GL_POLYGON_OFFSET_FILL,
    GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE,
};
const int kNumToggles = (int)(sizeof(kToggles) / sizeof(kToggles[0]));

bool HasExtension(const char* list, const char* name)
{
    if (!list || !name) return false;
    const size_t n = strlen(name);
    for (const char* p = list; (p = strstr(p, name)) != nullptr; p += n)
    {
        const bool startOk = (p == list) || p[-1] == ' ';
        const bool endOk   = p[n] == ' ' || p[n] == 0;
        if (startOk && endOk) return true;
    }
    return false;
}

int ParseGlesMajor(const char* ver)
{
    if (!ver) return 2;
    const char* p = strstr(ver, "OpenGL ES");
    if (!p) return 2;
    p += 9;
    while (*p && (*p < '0' || *p > '9')) ++p;
    return (*p >= '0' && *p <= '9') ? (*p - '0') : 2;
}

GLenum DrainErrors()
{
    GLenum first = GL_NO_ERROR;
    for (int i = 0; i < 16; ++i)
    {
        GLenum e = glGetError();
        if (e == GL_NO_ERROR) break;
        if (first == GL_NO_ERROR) first = e;
    }
    return first;
}

} // namespace

// ---------------------------------------------------------------------------------------
// State GL yang disimpan
// ---------------------------------------------------------------------------------------
struct Renderer::SavedState
{
    GLint     activeTexture;
    GLint     tex2D0;
    GLint     sampler0;
    GLint     vao;
    GLint     arrayBuffer;
    GLint     drawFbo;
    GLint     readFbo;
    GLint     program;
    GLint     viewport[4];
    GLboolean colorMask[4];
    GLboolean toggles[16];
    GLint     attribEnabled[kMaxAttribs];
    GLint     a0Size, a0Type, a0Normalized, a0Stride, a0Buffer;
    void*     a0Pointer;
};

// ---------------------------------------------------------------------------------------
Renderer::Renderer()
    : m_log(nullptr), m_ctx(EGL_NO_CONTEXT),
      m_vbo(0), m_tex(0), m_texW(0), m_texH(0), m_texFmt(0),
      m_failed(false), m_errStreak(0), m_lastError(GL_NO_ERROR), m_frames(0), m_loggedFirst(false)
{
    memset(&m_caps, 0, sizeof(m_caps));
    for (int v = 0; v < kVarCount; ++v)
    {
        m_prog[v] = 0;
        m_progBad[v] = false;
        for (int u = 0; u < U_COUNT; ++u) m_loc[v][u] = -1;
    }
    m_busy.clear();
}

void Renderer::Logf(int level, const char* fmt, ...)
{
    if (!m_log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_log(level, buf);
}

void Renderer::ForgetResources()
{
    // Sengaja TIDAK memanggil glDelete*: context lama sudah mati/berbeda, dan ID bekasnya
    // bisa kini dimiliki objek game di context baru.
    for (int v = 0; v < kVarCount; ++v)
    {
        m_prog[v] = 0;
        m_progBad[v] = false;
        for (int u = 0; u < U_COUNT; ++u) m_loc[v][u] = -1;
    }
    m_vbo = 0;
    m_tex = 0;
    m_texW = m_texH = 0;
    m_texFmt = 0;
    memset(&m_caps, 0, sizeof(m_caps));
    m_errStreak = 0;
    m_loggedFirst = false;
}

void Renderer::OnContextDestroyed(EGLContext ctx)
{
    if (ctx != EGL_NO_CONTEXT && ctx == m_ctx)
    {
        ForgetResources();
        m_ctx = EGL_NO_CONTEXT;
    }
}

void Renderer::OnDisplayTerminated()
{
    ForgetResources();
    m_ctx = EGL_NO_CONTEXT;
}

void Renderer::OnSwap(EGLDisplay dpy, EGLSurface surf)
{
    if (!m_cfg.enabled || m_failed) return;
    if (m_busy.test_and_set(std::memory_order_acquire)) return;   // cegah reentrancy
    ProcessFrame(dpy, surf);
    m_busy.clear(std::memory_order_release);
}

void Renderer::ProcessFrame(EGLDisplay dpy, EGLSurface surf)
{
    // Hanya proses kalau surface yang di-swap memang surface gambar context aktif.
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return;
    if (eglGetCurrentDisplay() != dpy) return;
    if (eglGetCurrentSurface(EGL_DRAW) != surf) return;

    EGLint w = 0, h = 0;
    if (!eglQuerySurface(dpy, surf, EGL_WIDTH, &w) || !eglQuerySurface(dpy, surf, EGL_HEIGHT, &h)) return;
    if (w < 16 || h < 16 || w > 8192 || h > 8192) return;

    if (ctx != m_ctx)
    {
        ForgetResources();
        m_ctx = ctx;
    }

    DrainErrors();                        // error lama milik game bukan urusan kita
    if (!m_caps.ready) QueryCaps();

    SavedState st;
    memset(&st, 0, sizeof(st));
    Capture(st);
    m_lastError = GL_NO_ERROR;
    const bool ok = Draw(w, h);
    Restore(st);
    const GLenum err = DrainErrors();
    if (err != GL_NO_ERROR) m_lastError = err;

    if (!ok || err != GL_NO_ERROR)
    {
        if (++m_errStreak >= 3)
        {
            m_failed = true;
            Logf(1, "efek dimatikan otomatis: %d frame berturut-turut gagal (GL error 0x%04X)",
                 m_errStreak, (unsigned)m_lastError);
        }
    }
    else
    {
        m_errStreak = 0;
        ++m_frames;
    }
}

void Renderer::QueryCaps()
{
    const char* ver  = (const char*)glGetString(GL_VERSION);
    const char* ext  = (const char*)glGetString(GL_EXTENSIONS);
    const char* rend = (const char*)glGetString(GL_RENDERER);

    m_caps.glMajor = ParseGlesMajor(ver);
    m_caps.maxAttribs = 8;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &m_caps.maxAttribs);
    if (m_caps.maxAttribs < 8) m_caps.maxAttribs = 8;
    if (m_caps.maxAttribs > kMaxAttribs) m_caps.maxAttribs = kMaxAttribs;

    typedef void (*Fn1)(unsigned int);
    typedef void (*Fn2)(unsigned int, unsigned int);

    m_caps.hasVao = false;
    m_caps.bindVao = nullptr;
    if (m_caps.glMajor >= 3)
        m_caps.bindVao = (Fn1)eglGetProcAddress("glBindVertexArray");
    else if (HasExtension(ext, "GL_OES_vertex_array_object"))
        m_caps.bindVao = (Fn1)eglGetProcAddress("glBindVertexArrayOES");
    m_caps.hasVao = m_caps.bindVao != nullptr;

    m_caps.hasSampler = false;
    m_caps.bindSampler = nullptr;
    if (m_caps.glMajor >= 3)
        m_caps.bindSampler = (Fn2)eglGetProcAddress("glBindSampler");
    m_caps.hasSampler = m_caps.bindSampler != nullptr;

    m_caps.ready = true;
    Logf(0, "GL: %s | %s | ES%d vao=%d sampler=%d attribs=%d",
         rend ? rend : "?", ver ? ver : "?", m_caps.glMajor,
         (int)m_caps.hasVao, (int)m_caps.hasSampler, (int)m_caps.maxAttribs);
}

// ---------------------------------------------------------------------------------------
// Simpan / pulihkan state
// ---------------------------------------------------------------------------------------
void Renderer::Capture(SavedState& s)
{
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
    glActiveTexture(GL_TEXTURE0);                    // kita memakai unit 0
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex2D0);

    s.sampler0 = 0;
    if (m_caps.hasSampler) glGetIntegerv(kSamplerBinding, &s.sampler0);

    // Bekerja di VAO bawaan (0). Kalau game memakai VAO, bind-kan kembali nanti.
    s.vao = 0;
    if (m_caps.hasVao)
    {
        glGetIntegerv(kVertexArrayBinding, &s.vao);
        if (s.vao != 0) m_caps.bindVao(0);
    }

    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrayBuffer);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.drawFbo);
    s.readFbo = s.drawFbo;
    if (m_caps.glMajor >= 3) glGetIntegerv(kReadFramebufferBinding, &s.readFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);

    for (int i = 0; i < kNumToggles; ++i) s.toggles[i] = glIsEnabled(kToggles[i]);

    for (int i = 0; i < m_caps.maxAttribs; ++i)
    {
        s.attribEnabled[i] = 0;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s.attribEnabled[i]);
    }
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE,           &s.a0Size);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE,           &s.a0Type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,     &s.a0Normalized);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE,         &s.a0Stride);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &s.a0Buffer);
    s.a0Pointer = nullptr;
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &s.a0Pointer);
}

void Renderer::Restore(const SavedState& s)
{
    // 1) atribut vertex (VAO bawaan masih aktif kalau tadi sempat dialihkan)
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s.a0Buffer);
    glVertexAttribPointer(0, s.a0Size, (GLenum)s.a0Type, s.a0Normalized ? GL_TRUE : GL_FALSE,
                          s.a0Stride, s.a0Pointer);
    for (int i = 0; i < m_caps.maxAttribs; ++i)
    {
        if (s.attribEnabled[i]) glEnableVertexAttribArray((GLuint)i);
        else                    glDisableVertexAttribArray((GLuint)i);
    }
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s.arrayBuffer);
    if (s.vao != 0) m_caps.bindVao((unsigned int)s.vao);

    // 2) program, framebuffer
    glUseProgram((GLuint)s.program);
    if (m_caps.glMajor >= 3 && s.readFbo != s.drawFbo)
    {
        glBindFramebuffer(kReadFramebuffer, (GLuint)s.readFbo);
        glBindFramebuffer(kDrawFramebuffer, (GLuint)s.drawFbo);
    }
    else
    {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)s.drawFbo);
    }

    // 3) tekstur unit 0, lalu unit aktif semula
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)s.tex2D0);
    if (m_caps.hasSampler) m_caps.bindSampler(0, (unsigned int)s.sampler0);
    glActiveTexture((GLenum)s.activeTexture);

    // 4) viewport, mask, toggle
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    for (int i = 0; i < kNumToggles; ++i)
    {
        if (s.toggles[i]) glEnable(kToggles[i]);
        else              glDisable(kToggles[i]);
    }
}

// ---------------------------------------------------------------------------------------
// Sumber daya GL
// ---------------------------------------------------------------------------------------
bool Renderer::EnsureProgram(int variant)
{
    if (m_prog[variant]) return true;
    if (m_progBad[variant]) return false;

    char prefix[64];
    snprintf(prefix, sizeof(prefix), "#define EDGE_MODE %d\n", variant);

    char info[512];
    info[0] = 0;

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &kVertexShader, nullptr);
    glCompileShader(vs);
    GLint ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        glGetShaderInfoLog(vs, sizeof(info), nullptr, info);
        Logf(1, "vertex shader gagal dikompilasi: %s", info);
        glDeleteShader(vs);
        m_progBad[variant] = true;
        return false;
    }

    const char* fsSrc[2] = { prefix, kFragmentShader };
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 2, fsSrc, nullptr);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        glGetShaderInfoLog(fs, sizeof(info), nullptr, info);
        Logf(1, "fragment shader (varian %d) gagal dikompilasi: %s", variant, info);
        glDeleteShader(vs);
        glDeleteShader(fs);
        m_progBad[variant] = true;
        return false;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        glGetProgramInfoLog(prog, sizeof(info), nullptr, info);
        Logf(1, "program (varian %d) gagal di-link: %s", variant, info);
        glDeleteProgram(prog);
        m_progBad[variant] = true;
        return false;
    }

    m_prog[variant] = prog;
    m_loc[variant][U_SCENE]      = glGetUniformLocation(prog, "uScene");
    m_loc[variant][U_TEXEL]      = glGetUniformLocation(prog, "uTexel");
    m_loc[variant][U_TOON_A]     = glGetUniformLocation(prog, "uToonA");
    m_loc[variant][U_TOON_B]     = glGetUniformLocation(prog, "uToonB");
    m_loc[variant][U_EDGE_A]     = glGetUniformLocation(prog, "uEdgeA");
    m_loc[variant][U_EDGE_COLOR] = glGetUniformLocation(prog, "uEdgeColor");
    m_loc[variant][U_DEBUG]      = glGetUniformLocation(prog, "uDebug");
    return true;
}

// Pilih varian sesuai pengaturan; kalau gagal kompilasi turun ke varian yang lebih ringan.
int Renderer::ChooseVariant()
{
    int want = !m_cfg.outline ? (int)kVarNone : (m_cfg.quality ? (int)kVarSobel : (int)kVarFast);
    for (int v = want; v >= 0; --v)
    {
        if (EnsureProgram(v))
        {
            if (v != want)
                Logf(1, "varian shader %d tidak bisa dipakai, turun ke varian %d", want, v);
            return v;
        }
    }
    return -1;
}

bool Renderer::EnsureGeometry()
{
    if (m_vbo) return true;
    static const GLfloat kTriangle[6] = { -1.0f, -1.0f,  3.0f, -1.0f,  -1.0f, 3.0f };
    glGenBuffers(1, &m_vbo);
    if (!m_vbo) return false;
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kTriangle), kTriangle, GL_STATIC_DRAW);
    return true;
}

bool Renderer::EnsureTexture(int w, int h, GLenum fmt)
{
    if (!m_tex)
    {
        glGenTextures(1, &m_tex);
        if (!m_tex) return false;
        glBindTexture(GL_TEXTURE_2D, m_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_texW = m_texH = 0;
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D, m_tex);
    }
    if (m_texW != w || m_texH != h || m_texFmt != fmt)
    {
        // Kalau game meninggalkan PIXEL_UNPACK_BUFFER terpasang (ES3), data "nullptr" di bawah
        // dibaca sebagai offset 0 di buffer itu dan bisa menghasilkan GL_INVALID_OPERATION.
        GLint pbo = 0;
        if (m_caps.glMajor >= 3)
        {
            glGetIntegerv(kPixelUnpackBufferBind, &pbo);
            if (pbo) glBindBuffer(kPixelUnpackBuffer, 0);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, nullptr);
        if (pbo) glBindBuffer(kPixelUnpackBuffer, (GLuint)pbo);
        m_texW = w;
        m_texH = h;
        m_texFmt = fmt;
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// Gambar satu frame
// ---------------------------------------------------------------------------------------
bool Renderer::Draw(int w, int h)
{
    const int variant = ChooseVariant();
    if (variant < 0)
    {
        m_failed = true;
        Logf(1, "tidak ada varian shader yang bisa dipakai di GPU ini; efek dimatikan");
        return false;
    }
    if (!EnsureGeometry()) return false;

    // 1) framebuffer bawaan -> tekstur
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLint alphaBits = 0;
    glGetIntegerv(GL_ALPHA_BITS, &alphaBits);
    const GLenum fmt = alphaBits > 0 ? GL_RGBA : GL_RGB;
    glActiveTexture(GL_TEXTURE0);
    if (!EnsureTexture(w, h, fmt)) return false;
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    {
        // Semua error lama sudah dibuang di awal frame, jadi apa pun yang muncul di sini milik kita.
        // Kalau salinan gagal, JANGAN menimpa layar dengan isi tekstur yang basi: biarkan frame asli.
        const GLenum e = glGetError();
        if (e != GL_NO_ERROR) { m_lastError = e; return false; }
    }

    // 2) state netral untuk gambar layar penuh
    if (m_caps.hasSampler) m_caps.bindSampler(0, 0);
    for (int i = 0; i < kNumToggles; ++i) glDisable(kToggles[i]);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, w, h);

    glUseProgram(m_prog[variant]);
    const GLint* loc = m_loc[variant];
    glUniform1i(loc[U_SCENE], 0);
    glUniform2f(loc[U_TEXEL], 1.0f / (float)w, 1.0f / (float)h);
    glUniform4f(loc[U_TOON_A], m_cfg.bands, m_cfg.softness, m_cfg.shadowFloor, m_cfg.saturation);
    glUniform4f(loc[U_TOON_B], m_cfg.brightness, m_cfg.posterize, m_cfg.posterizeLevels, 0.0f);

    // Jarak sampel diskalakan terhadap tinggi layar: 1.0 = 1 px pada layar 720.
    float offset = m_cfg.outlineWidth * ((float)h / 720.0f);
    if (offset < 0.75f) offset = 0.75f;
    if (offset > 8.0f)  offset = 8.0f;
    glUniform4f(loc[U_EDGE_A], offset, m_cfg.outlineThreshold, m_cfg.outlineSoftness, m_cfg.outlineStrength);
    glUniform3f(loc[U_EDGE_COLOR], m_cfg.outlineColor[0], m_cfg.outlineColor[1], m_cfg.outlineColor[2]);
    glUniform1f(loc[U_DEBUG], (float)m_cfg.debugView);

    // 3) satu segitiga yang menutupi seluruh layar
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    for (int i = 1; i < m_caps.maxAttribs; ++i) glDisableVertexAttribArray((GLuint)i);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void*)0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (!m_loggedFirst)
    {
        m_loggedFirst = true;
        Logf(0, "efek aktif: %dx%d, format salin=%s, varian shader=%d",
             w, h, fmt == GL_RGBA ? "RGBA" : "RGB", variant);
    }
    return true;
}

} // namespace cel
