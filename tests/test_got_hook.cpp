// Tes runtime got_hook: memasang hook GOT pada pustaka yang benar-benar di-dlopen.
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "check.h"
#include "elf_scan.h"
#include "got_hook.h"

typedef int (*IntFn)(void);
static IntFn g_orig = nullptr;
static int Hooked(void) { return g_orig() + 1000; }

// Cari slot lewat parser yang sama, lalu lihat proteksi halamannya di /proc/self/maps.
struct FindCtx { const char* lib; const char* sym; elfscan::Result r; };
static int Cb(struct dl_phdr_info* info, size_t, void* d)
{
    FindCtx* c = (FindCtx*)d;
    const char* slash = strrchr(info->dlpi_name, '/');
    if (strcmp(slash ? slash + 1 : info->dlpi_name, c->lib) != 0) return 0;
    elfscan::FindImportSlots<elfscan::Elf64>(info->dlpi_addr, info->dlpi_phdr, info->dlpi_phnum,
                                             c->sym, elfscan::NativeArch(), &c->r);
    return 1;
}

static bool PageWritable(uintptr_t addr)
{
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return true;
    char line[512];
    bool writable = true;
    while (fgets(line, sizeof(line), f))
    {
        unsigned long lo, hi; char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3 && addr >= lo && addr < hi)
        {
            writable = perms[1] == 'w';
            break;
        }
    }
    fclose(f);
    return writable;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "pakai: %s <dir-berisi-libgottarget.so>\n", argv[0]); return 2; }
    std::string dir = argv[1];
    void* h = dlopen((dir + "/libgottarget.so").c_str(), RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen gagal: %s\n", dlerror()); return 2; }
    IntFn call    = (IntFn)dlsym(h, "target_call");
    IntFn callPtr = (IntFn)dlsym(h, "target_call_ptr");
    IntFn other   = (IntFn)dlsym(h, "target_other");
    CHECK(call && callPtr && other);

    CHECK_EQ_INT(call(), 7);
    CHECK_EQ_INT(callPtr(), 7);

    // --- kegagalan yang diharapkan ---
    int n = -1;
    CHECK_EQ_INT(gothook::Install("libtidakada.so", "dep_value", (void*)Hooked, (void**)&g_orig, &n), gothook::kModuleNotFound);
    CHECK_EQ_INT(n, 0);
    CHECK_EQ_INT(gothook::Install("libgottarget.so", "simbol_gaib", (void*)Hooked, (void**)&g_orig, &n), gothook::kSymbolNotImported);
    CHECK_EQ_INT(n, 0);
    CHECK_EQ_INT(gothook::Install(nullptr, "dep_value", (void*)Hooked, (void**)&g_orig, &n), gothook::kModuleNotFound);
    CHECK(g_orig == nullptr);
    CHECK_EQ_INT(call(), 7);               // belum ada yang berubah

    // --- slot sebelum dipasang: proteksi halaman GOT PLT (RELRO penuh -> tidak writable) ---
    FindCtx fc = { "libgottarget.so", "dep_value", {} };
    memset(&fc.r, 0, sizeof(fc.r));
    dl_iterate_phdr(Cb, &fc);
    CHECK(fc.r.count >= 1);
    uintptr_t pltSlot = 0;
    for (int i = 0; i < fc.r.count; ++i) if (fc.r.kinds[i] == elfscan::kSlotPlt) pltSlot = fc.r.slots[i];
    CHECK(pltSlot != 0);
    CHECK(fc.r.relroBegin != 0 && pltSlot >= fc.r.relroBegin && pltSlot < fc.r.relroEnd);
    CHECK(!PageWritable(pltSlot));         // RELRO: read-only sebelum hook

    // --- pasang ---
    gothook::Status s = gothook::Install("libgottarget.so", "dep_value", (void*)Hooked, (void**)&g_orig, &n);
    CHECK_EQ_INT(s, gothook::kOk);
    CHECK(n >= 1);
    printf("slot yang di-patch: %d\n", n);
    CHECK(g_orig != nullptr);
    CHECK_EQ_INT(g_orig(), 7);             // pointer asli masih fungsi asli
    CHECK_EQ_INT(call(), 1007);            // jalur PLT
    CHECK_EQ_INT(callPtr(), 1007);         // jalur pointer data
    CHECK_EQ_INT(other(), 3);              // simbol lain tidak tersentuh
    CHECK(!PageWritable(pltSlot));         // RELRO dikembalikan ke read-only setelah patch

    // --- idempotent: pemasangan kedua tidak boleh membungkus dua kali ---
    IntFn before = g_orig;
    CHECK_EQ_INT(gothook::Install("libgottarget.so", "dep_value", (void*)Hooked, (void**)&g_orig, &n), gothook::kOk);
    CHECK(g_orig == before);
    CHECK_EQ_INT(call(), 1007);

    // --- original boleh null ---
    CHECK_EQ_INT(gothook::Install("libgottarget.so", "dep_other", (void*)+[]() -> int { return 99; }, nullptr, &n), gothook::kOk);
    CHECK_EQ_INT(other(), 99);

    return TestSummary("got_hook");
}
