// test_e2e.cpp - uji menyeluruh: PluginInit -> hook GOT pada "game palsu" -> render -> reload INI.
//
// Pemakaian:  test_e2e SKENARIO DIR_UJI [PUSTAKA_GAME]
//   SKENARIO: normal | config-create | fallback-ok | fallback-fail | nohook
// Tiap skenario HARUS jalan di proses sendiri (PluginInit memegang state global).
// Untuk memeriksa pemanggilan berantai ke fungsi EGL asli, jalankan dengan
//   LD_PRELOAD=libswapcount_shim.so   (tanpa itu, pemeriksaan hitungan dilewati).
#include "check.h"
#include "gl_test_support.h"
#include "cel_config.h"
#include "cel_plugin.h"
#include "cel_renderer.h"

#include <dlfcn.h>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

using namespace gltest;

// ------------------------------------------------------------------ utilitas
static std::vector<std::string> g_log;
static void LogSink(int level, const char* msg)
{
    g_log.push_back(std::string(level ? "E: " : "I: ") + msg);
    printf("      [plugin %s] %s\n", level ? "ERR " : "info", msg);
}
static bool LogHas(const char* needle)
{
    for (const std::string& s : g_log)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

static long long NowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void SleepMs(int ms) { usleep((useconds_t)ms * 1000); }

static bool WriteFile(const std::string& path, const std::string& text)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok;
}
static std::string ReadFile(const std::string& path)
{
    std::string s;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

// Penghitung dari libswapcount_shim.so (kalau dimuat lewat LD_PRELOAD).
struct Counters
{
    unsigned (*swap)(void) = nullptr;
    unsigned (*destroy)(void) = nullptr;
    unsigned (*terminate)(void) = nullptr;
    bool ok() const { return swap && destroy && terminate; }
    Counters()
    {
        swap = (unsigned (*)(void))dlsym(RTLD_DEFAULT, "swapcount_swap");
        destroy = (unsigned (*)(void))dlsym(RTLD_DEFAULT, "swapcount_destroy");
        terminate = (unsigned (*)(void))dlsym(RTLD_DEFAULT, "swapcount_terminate");
    }
};

// API game palsu
struct FakeGame
{
    void* lib = nullptr;
    int (*init)(void) = nullptr;
    void (*draw)(int, int) = nullptr;
    int (*swap)(EGLDisplay, EGLSurface) = nullptr;
    int (*destroyContext)(EGLDisplay, EGLContext) = nullptr;
    int (*terminate)(EGLDisplay) = nullptr;

    bool Load(const char* path)
    {
        lib = dlopen(path, RTLD_NOW);
        if (!lib) { fprintf(stderr, "dlopen(%s): %s\n", path, dlerror()); return false; }
        init = (int (*)(void))dlsym(lib, "fakegame_init");
        draw = (void (*)(int, int))dlsym(lib, "fakegame_draw");
        swap = (int (*)(EGLDisplay, EGLSurface))dlsym(lib, "fakegame_swap");
        destroyContext = (int (*)(EGLDisplay, EGLContext))dlsym(lib, "fakegame_destroy_context");
        terminate = (int (*)(EGLDisplay))dlsym(lib, "fakegame_terminate");
        return init && draw && swap && destroyContext && terminate;
    }
};

// ------------------------------------------------------------------ skenario: normal
static int ScenarioNormal(const std::string& dir, const char* gamePath)
{
    printf("== skenario normal: hook GOT pada libfakegame.so\n");
    const std::string cfgDir = dir + "/configs/";
    mkdir(dir.c_str(), 0777);
    mkdir(cfgDir.c_str(), 0777);
    const std::string ini = cfgDir + "CelShading.ini";
    CHECK(WriteFile(ini, "[CelShading]\nGameLibrary = libfakegame.so\n"));

    Env env;
    CHECK(CreateEnv(env, 640, 360, false, 3));
    FakeGame game;
    CHECK(game.Load(gamePath));
    if (!game.draw || env.ctx == EGL_NO_CONTEXT) return TestSummary("e2e-normal");
    CHECK(game.init() == 1);
    Counters cnt;
    printf("   pemeriksaan pemanggilan berantai (LD_PRELOAD shim): %s\n", cnt.ok() ? "aktif" : "DILEWATI");

    auto frame = [&]() {
        game.draw(env.w, env.h);
        game.swap(env.dpy, env.surf);
        return ReadScreen(env.w, env.h);
    };
    // Referensi: renderer terpisah (di luar hook) dengan pengaturan dari teks INI yang sama.
    auto reference = [&](const std::string& iniText) {
        cel::Settings s;
        cel::ParseIni(iniText.data(), iniText.size(), &s);
        cel::Renderer ref;
        ref.SetSettings(s);
        game.draw(env.w, env.h);
        ref.OnSwap(env.dpy, env.surf);
        return ReadScreen(env.w, env.h);
    };
    // Terus menggambar frame sampai hasilnya == `want` (atau waktu habis). Mengembalikan ms, atau -1.
    auto waitFor = [&](const Image& want, int timeoutMs) -> long long {
        const long long t0 = NowMs();
        while (NowMs() - t0 < timeoutMs)
        {
            if (frame().SameAs(want)) return NowMs() - t0;
            SleepMs(30);
        }
        return -1;
    };

    // --- 1) sebelum plugin dipasang: gambar mentah
    game.draw(env.w, env.h);
    const Image raw = ReadScreen(env.w, env.h);
    unsigned swapsBefore = cnt.ok() ? cnt.swap() : 0;
    frame();
    CHECK(!cnt.ok() || cnt.swap() == swapsBefore + 1);      // game memanggil eglSwapBuffers sekali per frame

    // --- 2) pasang plugin
    cel::Host host;
    host.log = LogSink;
    host.configDir = cfgDir.c_str();
    host.inlineHook = nullptr;
    CHECK(cel::PluginInit(host));
    CHECK(cel::PluginInit(host));                            // kedua kali: tidak melakukan apa-apa
    CHECK(LogHas("hook eglSwapBuffers terpasang pada libfakegame.so"));
    CHECK(!LogHas("eglDestroyContext tidak diimpor"));
    CHECK(!LogHas("eglTerminate tidak diimpor"));
    CHECK(!LogHas("TIDAK aktif"));
    CHECK(std::string(cel::PluginConfigPath()) == ini);

    // --- 3) frame lewat game -> sama persis dengan renderer referensi, dan swap asli tetap dipanggil
    const Image defaultRef = reference("");
    CHECK(!defaultRef.SameAs(raw));
    swapsBefore = cnt.ok() ? cnt.swap() : 0;
    Image out = frame();
    CHECK(out.SameAs(defaultRef));
    CHECK_EQ_INT(cel::PluginRenderer()->FramesProcessed(), 1);
    if (cnt.ok()) CHECK_EQ_INT(cnt.swap(), swapsBefore + 1);  // hook meneruskan ke eglSwapBuffers asli, tepat sekali
    printf("   frame lewat hook == referensi: %s\n", out.SameAs(defaultRef) ? "ya" : "TIDAK");

    // Panggilan eglSwapBuffers dari luar game (mis. dari tes ini) tidak boleh disentuh plugin.
    const unsigned processed = cel::PluginRenderer()->FramesProcessed();
    eglSwapBuffers(env.dpy, env.surf);
    CHECK_EQ_INT(cel::PluginRenderer()->FramesProcessed(), processed);

    // --- 4) reload langsung
    struct Step { const char* name; std::string text; };
    const Step steps[] = {
        { "Enabled=0 (efek mati)",      "Enabled = 0\n" },
        { "3 pita, garis merah tebal",  "ShadeBands = 3\nOutlineWidth = 2\nOutlineColor = 255 0 0\n" },
        { "berkas rusak -> default",    "\x01\x02 ### bukan ini\n=== [[[ \n;;\n" },
    };
    for (const Step& st : steps)
    {
        SleepMs(1200);                       // mtime berkas hanya berresolusi 1 detik
        CHECK(WriteFile(ini, st.text));
        const Image want = (st.text.find("Enabled = 0") != std::string::npos) ? raw : reference(st.text);
        const long long ms = waitFor(want, 12000);
        printf("   reload \"%s\": %s (%lld ms)\n", st.name, ms >= 0 ? "diterapkan" : "TIDAK diterapkan", ms);
        CHECK(ms >= 0);
        CHECK(ms < 6000);
    }
    CHECK(LogHas("konfigurasi dimuat ulang"));

    // berkas dihapus -> pengaturan terakhir tetap dipakai, game tidak terganggu
    const Image keep = reference("");            // keadaan terakhir = default (dari berkas rusak)
    unlink(ini.c_str());
    for (int i = 0; i < 70; ++i) { frame(); SleepMs(30); }   // > 2 detik
    CHECK(frame().SameAs(keep));
    // berkas dibuat lagi -> diterapkan lagi
    SleepMs(1200);
    CHECK(WriteFile(ini, "Enabled = 0\n"));
    CHECK(waitFor(raw, 12000) >= 0);

    // --- 5) context dihancurkan oleh game -> hook eglDestroyContext meneruskan + renderer melepas ID lama
    SleepMs(1200);
    CHECK(WriteFile(ini, "ShadeBands = 4\n"));
    const Image bands4 = reference("ShadeBands = 4\n");
    CHECK(waitFor(bands4, 12000) >= 0);

    const unsigned destroyBefore = cnt.ok() ? cnt.destroy() : 0;
    EGLContext oldCtx = env.ctx;
    eglMakeCurrent(env.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    CHECK(game.destroyContext(env.dpy, oldCtx) == 1);
    if (cnt.ok()) CHECK_EQ_INT(cnt.destroy(), destroyBefore + 1);
    env.ctx = EGL_NO_CONTEXT;
    CHECK(CreateContext(env));
    CHECK(eglMakeCurrent(env.dpy, env.surf, env.surf, env.ctx) == EGL_TRUE);
    CHECK(game.init() == 1);
    const Image afterLoss = frame();
    CHECK(afterLoss.SameAs(reference("ShadeBands = 4\n")));
    CHECK(!afterLoss.SameAs(raw));
    printf("   setelah context diganti, efek tetap benar: %s\n", afterLoss.SameAs(reference("ShadeBands = 4\n")) ? "ya" : "TIDAK");

    // --- 6) eglTerminate dari game diteruskan
    const unsigned termBefore = cnt.ok() ? cnt.terminate() : 0;
    eglMakeCurrent(env.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    CHECK(game.terminate(env.dpy) == 1);
    if (cnt.ok()) CHECK_EQ_INT(cnt.terminate(), termBefore + 1);
    return TestSummary("e2e-normal");
}

// ------------------------------------------------------------------ skenario: config-create
static int ScenarioConfigCreate(const std::string& dir)
{
    printf("== skenario config-create: berkas INI dibuat otomatis\n");
    const std::string cfgDir = dir + "/configs/";
    mkdir(dir.c_str(), 0777);
    // sengaja TIDAK membuat cfgDir: plugin harus membuatnya
    cel::Host host;
    host.log = LogSink;
    host.configDir = cfgDir.c_str();
    host.inlineHook = nullptr;
    const bool ok = cel::PluginInit(host);       // libGTASA.so tidak ada di proses tes -> tidak aktif
    CHECK(!ok);
    CHECK(LogHas("berkas konfigurasi dibuat"));
    const std::string created = ReadFile(cfgDir + "CelShading.ini");
    CHECK(created == cel::DefaultIniText());
    CHECK(LogHas("hook GOT libGTASA.so gagal"));
    CHECK(LogHas("plugin TIDAK aktif"));
    return TestSummary("e2e-config-create");
}

// ------------------------------------------------------------------ skenario: jalur cadangan (hook inline)
static void*    g_mockTarget = nullptr;
static void*    g_mockRepl = nullptr;
static unsigned g_mockSwaps = 0;
static EGLBoolean MockOriginalSwap(EGLDisplay, EGLSurface) { ++g_mockSwaps; return EGL_TRUE; }

static bool MockHookOk(void* target, void* replacement, void** original)
{
    g_mockTarget = target;
    g_mockRepl = replacement;
    *original = (void*)&MockOriginalSwap;
    return true;
}
static bool MockHookFail(void*, void*, void**) { return false; }

static int ScenarioFallback(const std::string& dir, const char* gamePath, bool succeed, bool provideHook)
{
    printf("== skenario fallback (hook %s)\n", provideHook ? (succeed ? "berhasil" : "gagal") : "tidak tersedia");
    const std::string cfgDir = dir + "/configs/";
    mkdir(dir.c_str(), 0777);
    mkdir(cfgDir.c_str(), 0777);
    CHECK(WriteFile(cfgDir + "CelShading.ini", "GameLibrary = libtidak-ada.so\n"));

    Env env;
    CHECK(CreateEnv(env, 640, 360, false, 3));
    FakeGame game;
    CHECK(game.Load(gamePath));
    CHECK(game.init() == 1);

    cel::Host host;
    host.log = LogSink;
    host.configDir = cfgDir.c_str();
    host.inlineHook = provideHook ? (succeed ? MockHookOk : MockHookFail) : nullptr;
    const bool ok = cel::PluginInit(host);
    CHECK(LogHas("hook GOT libtidak-ada.so gagal"));

    if (!provideHook)
    {
        CHECK(!ok);
        CHECK(LogHas("tidak ada hook inline dari host"));
        CHECK(LogHas("plugin TIDAK aktif"));
        return TestSummary("e2e-nohook");
    }
    if (!succeed)
    {
        CHECK(!ok);
        CHECK(LogHas("hook inline eglSwapBuffers gagal"));
        CHECK(LogHas("plugin TIDAK aktif"));
        return TestSummary("e2e-fallback-fail");
    }

    CHECK(ok);
    CHECK(LogHas("hook cadangan terpasang"));
    // target harus alamat eglSwapBuffers asli di libEGL
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) egl = dlopen("libEGL.so.1", RTLD_NOW);
    CHECK(egl != nullptr);
    CHECK(g_mockTarget != nullptr && g_mockTarget == dlsym(egl, "eglSwapBuffers"));
    CHECK(g_mockRepl != nullptr);

    // "game" memanggil fungsi pengganti (yang dipasang oleh hook inline)
    typedef EGLBoolean (*SwapFn)(EGLDisplay, EGLSurface);
    SwapFn hooked = (SwapFn)g_mockRepl;
    game.draw(env.w, env.h);
    const Image raw = ReadScreen(env.w, env.h);
    game.draw(env.w, env.h);
    CHECK(hooked(env.dpy, env.surf) == EGL_TRUE);
    CHECK_EQ_INT(g_mockSwaps, 1);                          // diteruskan ke "asli" tepat sekali
    const Image out = ReadScreen(env.w, env.h);
    CHECK(!out.SameAs(raw));                               // efek diterapkan lewat jalur cadangan
    CHECK_EQ_INT(cel::PluginRenderer()->FramesProcessed(), 1);
    return TestSummary("e2e-fallback-ok");
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "pemakaian: %s SKENARIO DIR_UJI [PUSTAKA_GAME]\n", argv[0]);
        return 2;
    }
    const std::string scenario = argv[1], dir = argv[2];
    const char* gamePath = argc > 3 ? argv[3] : nullptr;
    if (GetDisplay() == EGL_NO_DISPLAY)
    {
        printf("[e2e] EGL tidak tersedia di mesin ini - tes dilewati\n");
        return 0;
    }
    if (scenario == "normal" && gamePath) return ScenarioNormal(dir, gamePath);
    if (scenario == "config-create") return ScenarioConfigCreate(dir);
    if (scenario == "fallback-ok" && gamePath) return ScenarioFallback(dir, gamePath, true, true);
    if (scenario == "fallback-fail" && gamePath) return ScenarioFallback(dir, gamePath, false, true);
    if (scenario == "nohook" && gamePath) return ScenarioFallback(dir, gamePath, false, false);
    fprintf(stderr, "skenario tidak dikenal / argumen kurang: %s\n", scenario.c_str());
    return 2;
}
