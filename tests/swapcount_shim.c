/* swapcount_shim.c - pustaka LD_PRELOAD untuk tes end-to-end.
 *
 * Membungkus eglSwapBuffers / eglDestroyContext / eglTerminate: menghitung pemanggilan lalu
 * meneruskan ke implementasi EGL asli. Karena dimuat lewat LD_PRELOAD, ia menjadi "EGL" yang
 * dilihat libfakegame.so; GOT libfakegame.so lalu menunjuk ke sini, dan hook plugin harus
 * menyimpannya sebagai fungsi "asli" yang dipanggil berantai.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <EGL/egl.h>

static unsigned g_swap, g_destroy, g_terminate;

EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
    static EGLBoolean (*real)(EGLDisplay, EGLSurface);
    if (!real) real = (EGLBoolean (*)(EGLDisplay, EGLSurface))dlsym(RTLD_NEXT, "eglSwapBuffers");
    ++g_swap;
    return real ? real(d, s) : EGL_FALSE;
}

EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c)
{
    static EGLBoolean (*real)(EGLDisplay, EGLContext);
    if (!real) real = (EGLBoolean (*)(EGLDisplay, EGLContext))dlsym(RTLD_NEXT, "eglDestroyContext");
    ++g_destroy;
    return real ? real(d, c) : EGL_FALSE;
}

EGLBoolean eglTerminate(EGLDisplay d)
{
    static EGLBoolean (*real)(EGLDisplay);
    if (!real) real = (EGLBoolean (*)(EGLDisplay))dlsym(RTLD_NEXT, "eglTerminate");
    ++g_terminate;
    return real ? real(d) : EGL_FALSE;
}

unsigned swapcount_swap(void)      { return g_swap; }
unsigned swapcount_destroy(void)   { return g_destroy; }
unsigned swapcount_terminate(void) { return g_terminate; }
