#include "cel_plugin.h"

#include <EGL/egl.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cel_config.h"
#include "got_hook.h"

namespace cel {
namespace {

typedef EGLBoolean (*SwapFn)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*DestroyContextFn)(EGLDisplay, EGLContext);
typedef EGLBoolean (*TerminateFn)(EGLDisplay);

const char* const kIniName = "CelShading.ini";

Renderer         g_renderer;
Host             g_host = { nullptr, nullptr, nullptr };
Settings         g_settings;
char             g_iniPath[512];
bool             g_installed = false;

SwapFn           g_origSwap      = nullptr;
DestroyContextFn g_origDestroy   = nullptr;
TerminateFn      g_origTerminate = nullptr;

// --- live reload ---
int64_t g_nextCheckMs   = 0;
time_t  g_appliedMtime  = 0;
off_t   g_appliedSize   = -1;
bool    g_pendingValid  = false;
time_t  g_pendingMtime  = 0;
off_t   g_pendingSize   = -1;

void Logf(int level, const char* fmt, ...)
{
    if (!g_host.log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_host.log(level, buf);
}

int64_t NowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void LogSettings(const Settings& s, const char* prefix)
{
    Logf(0, "%s: enabled=%d bands=%.0f soft=%.2f sat=%.2f | outline=%d width=%.2f thr=%.2f quality=%d debug=%d",
         prefix, (int)s.enabled, s.bands, s.softness, s.saturation,
         (int)s.outline, s.outlineWidth, s.outlineThreshold, s.quality, s.debugView);
}

// Dipanggil dari thread render tiap frame; hanya benar-benar bekerja ~1x per detik.
void CheckReload()
{
    const int64_t now = NowMs();
    if (now < g_nextCheckMs) return;
    g_nextCheckMs = now + 1000;

    struct stat st;
    if (stat(g_iniPath, &st) != 0) return;
    if (st.st_mtime == g_appliedMtime && st.st_size == g_appliedSize)
    {
        g_pendingValid = false;
        return;
    }
    // Berkas berubah. Tunggu sampai stabil selama satu siklus (hindari membaca berkas
    // yang sedang ditulis editor).
    if (!g_pendingValid || g_pendingMtime != st.st_mtime || g_pendingSize != st.st_size)
    {
        g_pendingValid = true;
        g_pendingMtime = st.st_mtime;
        g_pendingSize  = st.st_size;
        return;
    }

    Settings s;                       // mulai dari nilai bawaan: kunci yang dihapus kembali default
    if (LoadFile(g_iniPath, &s))
    {
        strncpy(s.gameLibrary, g_settings.gameLibrary, sizeof(s.gameLibrary));   // butuh restart
        s.gameLibrary[sizeof(s.gameLibrary) - 1] = 0;
        g_settings = s;
        g_renderer.SetSettings(s);
        LogSettings(s, "konfigurasi dimuat ulang");
    }
    g_appliedMtime = st.st_mtime;
    g_appliedSize  = st.st_size;
    g_pendingValid = false;
}

// --- fungsi pengganti yang dipanggil game ---
EGLBoolean HookedSwap(EGLDisplay dpy, EGLSurface surf)
{
    CheckReload();
    g_renderer.OnSwap(dpy, surf);
    SwapFn fn = __atomic_load_n(&g_origSwap, __ATOMIC_ACQUIRE);
    return fn ? fn(dpy, surf) : EGL_TRUE;
}

EGLBoolean HookedDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
    g_renderer.OnContextDestroyed(ctx);
    DestroyContextFn fn = __atomic_load_n(&g_origDestroy, __ATOMIC_ACQUIRE);
    return fn ? fn(dpy, ctx) : EGL_FALSE;
}

EGLBoolean HookedTerminate(EGLDisplay dpy)
{
    g_renderer.OnDisplayTerminated();
    TerminateFn fn = __atomic_load_n(&g_origTerminate, __ATOMIC_ACQUIRE);
    return fn ? fn(dpy) : EGL_FALSE;
}

// Hook cadangan: pasang langsung di dalam libEGL.so (memakai hook inline milik host).
bool InstallInlineFallback()
{
    if (!g_host.inlineHook)
    {
        Logf(1, "tidak ada hook inline dari host; jalur cadangan tidak tersedia");
        return false;
    }
    void* lib = dlopen("libEGL.so", RTLD_NOW);
    if (!lib) lib = dlopen("libEGL.so.1", RTLD_NOW);
    void* fn = lib ? dlsym(lib, "eglSwapBuffers") : nullptr;
    if (!fn)
    {
        Logf(1, "eglSwapBuffers tidak ditemukan di libEGL.so");
        return false;
    }
    void* orig = nullptr;
    if (!g_host.inlineHook(fn, (void*)&HookedSwap, &orig) || !orig)
    {
        Logf(1, "hook inline eglSwapBuffers gagal");
        return false;
    }
    __atomic_store_n(&g_origSwap, (SwapFn)orig, __ATOMIC_RELEASE);
    Logf(0, "hook cadangan terpasang langsung di libEGL.so (eglSwapBuffers)");
    return true;
}

} // namespace

Renderer*   PluginRenderer()   { return &g_renderer; }
const char* PluginConfigPath() { return g_iniPath; }

bool PluginInit(const Host& host)
{
    if (g_installed) return true;
    g_host = host;
    g_renderer.SetLogger(host.log);

    // --- konfigurasi ---
    const char* dir = host.configDir ? host.configDir : "";
    const size_t dl = strlen(dir);
    snprintf(g_iniPath, sizeof(g_iniPath), "%s%s%s", dir,
             (dl > 0 && dir[dl - 1] != '/') ? "/" : "", kIniName);
    if (dl > 0) mkdir(dir, 0777);      // biasanya sudah dibuat AML; abaikan error

    Settings s;
    if (access(g_iniPath, F_OK) != 0)
    {
        if (WriteDefaultFile(g_iniPath)) Logf(0, "berkas konfigurasi dibuat: %s", g_iniPath);
        else                             Logf(1, "gagal membuat %s (pakai nilai bawaan)", g_iniPath);
    }
    if (!LoadFile(g_iniPath, &s))
        Logf(1, "gagal membaca %s (pakai nilai bawaan)", g_iniPath);

    struct stat st;
    if (stat(g_iniPath, &st) == 0) { g_appliedMtime = st.st_mtime; g_appliedSize = st.st_size; }
    g_settings = s;
    g_renderer.SetSettings(s);
    LogSettings(s, "konfigurasi");

    // --- hook eglSwapBuffers pada pustaka game (lewat tabel GOT) ---
    int patched = 0;
    gothook::Status r = gothook::Install(s.gameLibrary, "eglSwapBuffers", (void*)&HookedSwap,
                                         (void**)&g_origSwap, &patched);
    bool ok = (r == gothook::kOk);
    if (ok)
    {
        Logf(0, "hook eglSwapBuffers terpasang pada %s (%d slot GOT)", s.gameLibrary, patched);

        // Pelengkap: tahu kapan context dihancurkan supaya ID objek GL lama tidak dipakai lagi.
        int n = 0;
        if (gothook::Install(s.gameLibrary, "eglDestroyContext", (void*)&HookedDestroyContext,
                             (void**)&g_origDestroy, &n) != gothook::kOk)
            Logf(0, "eglDestroyContext tidak diimpor %s (dilewati)", s.gameLibrary);
        if (gothook::Install(s.gameLibrary, "eglTerminate", (void*)&HookedTerminate,
                             (void**)&g_origTerminate, &n) != gothook::kOk)
            Logf(0, "eglTerminate tidak diimpor %s (dilewati)", s.gameLibrary);
    }
    else
    {
        Logf(1, "hook GOT %s gagal: %s", s.gameLibrary, gothook::StatusText(r));
        ok = InstallInlineFallback();
    }

    g_installed = ok;
    if (!ok) Logf(1, "plugin TIDAK aktif: eglSwapBuffers tidak bisa di-hook");
    return ok;
}

} // namespace cel
