// cel_renderer.h - post-process layar penuh (cel shading + outline) untuk OpenGL ES 2.0+.
//
// Dipanggil tepat SEBELUM eglSwapBuffers: menyalin framebuffer bawaan ke tekstur, lalu
// menggambar ulang satu segitiga layar-penuh dengan fragment shader cel shading/outline.
// Seluruh state GL yang disentuh disimpan lalu dipulihkan persis seperti semula, karena
// engine game (RenderWare) menyimpan cache state-nya sendiri.
#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <atomic>

#include "cel_config.h"

namespace cel {

// level: 0 = info, 1 = error
typedef void (*LogFn)(int level, const char* msg);

class Renderer
{
public:
    Renderer();

    void SetLogger(LogFn fn) { m_log = fn; }
    void SetSettings(const Settings& s) { m_cfg = s; }
    const Settings& GetSettings() const { return m_cfg; }

    // Panggil di thread yang memegang context GL, tepat sebelum eglSwapBuffers asli.
    void OnSwap(EGLDisplay dpy, EGLSurface surf);

    // Beri tahu kalau context/display dihancurkan: ID objek GL lama tidak boleh dipakai
    // lagi (ID bisa dipakai ulang oleh objek milik game di context baru).
    void OnContextDestroyed(EGLContext ctx);
    void OnDisplayTerminated();

    bool     DisabledByError() const { return m_failed; }
    unsigned FramesProcessed() const { return m_frames; }

private:
    enum Variant { kVarNone = 0, kVarFast = 1, kVarSobel = 2, kVarCount = 3 };
    enum Uniform { U_SCENE, U_TEXEL, U_TOON_A, U_TOON_B, U_EDGE_A, U_EDGE_COLOR, U_DEBUG, U_COUNT };

    struct Caps
    {
        bool  ready;
        int   glMajor;
        GLint maxAttribs;
        bool  hasVao;
        void (*bindVao)(unsigned int);
        bool  hasSampler;
        void (*bindSampler)(unsigned int, unsigned int);
    };

    struct SavedState;

    void Logf(int level, const char* fmt, ...);
    void ProcessFrame(EGLDisplay dpy, EGLSurface surf);
    void QueryCaps();
    void ForgetResources();

    void Capture(SavedState& s);
    void Restore(const SavedState& s);

    bool Draw(int w, int h);
    int  ChooseVariant();
    bool EnsureProgram(int variant);
    bool EnsureGeometry();
    bool EnsureTexture(int w, int h, GLenum fmt);

    LogFn    m_log;
    Settings m_cfg;

    EGLContext m_ctx;
    Caps       m_caps;

    GLuint m_prog[kVarCount];
    GLint  m_loc[kVarCount][U_COUNT];
    bool   m_progBad[kVarCount];
    GLuint m_vbo;
    GLuint m_tex;
    int    m_texW, m_texH;
    GLenum m_texFmt;

    bool     m_failed;
    int      m_errStreak;
    GLenum   m_lastError;   // error GL terakhir yang tercatat saat gagal (untuk pesan log)
    unsigned m_frames;
    bool     m_loggedFirst;

    std::atomic_flag m_busy;
};

} // namespace cel
