// fakegame.cpp - "game palsu" untuk tes end-to-end.
//
// Pustaka bersama yang meniru libGTASA.so dari sisi yang relevan bagi plugin: ia MENGIMPOR
// eglSwapBuffers / eglDestroyContext / eglTerminate lewat tabel PLT/GOT-nya sendiri dan
// menggambar adegan 3D sintetis. Plugin diminta mem-hook "libfakegame.so" persis seperti
// ia mem-hook "libGTASA.so".
//
// Dibangun dengan -Wl,-z,relro,-z,now agar GOT-nya berperilaku seperti di bionic Android
// (semua relokasi diselesaikan saat load, GOT dilindungi RELRO).
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "scene.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

static scene::Scene g_scene;

EXPORT int fakegame_init(void)
{
    return g_scene.Init() ? 1 : 0;
}

// Satu frame "game": gambar adegan (belum swap).
EXPORT void fakegame_draw(int w, int h)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_scene.Draw(w, h);
}

// Panggilan swap seperti di engine game: lewat import eglSwapBuffers milik pustaka ini.
EXPORT int fakegame_swap(EGLDisplay dpy, EGLSurface surf)
{
    return eglSwapBuffers(dpy, surf) ? 1 : 0;
}

EXPORT int fakegame_destroy_context(EGLDisplay dpy, EGLContext ctx)
{
    return eglDestroyContext(dpy, ctx) ? 1 : 0;
}

EXPORT int fakegame_terminate(EGLDisplay dpy)
{
    return eglTerminate(dpy) ? 1 : 0;
}
