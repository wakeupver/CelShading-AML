#include "got_hook.h"
#include "elf_scan.h"

#include <link.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

namespace gothook {
namespace {

struct ScanContext
{
    const char*     moduleFile;
    const char*     symbol;
    bool            moduleFound;
    elfscan::Result result;
};

// Bandingkan nama berkas (bagian setelah '/' terakhir) dengan `file`.
bool BaseNameEquals(const char* path, const char* file)
{
    const char* slash = strrchr(path, '/');
    const char* base  = slash ? slash + 1 : path;
    // Pustaka yang di-load langsung dari APK bernama ".../base.apk!/lib/arm64-v8a/libX.so",
    // sehingga bagian setelah '/' terakhir tetap nama berkas aslinya.
    return strcmp(base, file) == 0;
}

int PhdrCallback(struct dl_phdr_info* info, size_t /*size*/, void* data)
{
    ScanContext* ctx = (ScanContext*)data;
    if (!info->dlpi_name || !info->dlpi_name[0]) return 0;
    if (!BaseNameEquals(info->dlpi_name, ctx->moduleFile)) return 0;

    ctx->moduleFound = true;
    if (sizeof(void*) == 8)
        elfscan::FindImportSlots<elfscan::Elf64>((uintptr_t)info->dlpi_addr, info->dlpi_phdr,
                                                 info->dlpi_phnum, ctx->symbol,
                                                 elfscan::NativeArch(), &ctx->result);
    else
        elfscan::FindImportSlots<elfscan::Elf32>((uintptr_t)info->dlpi_addr, info->dlpi_phdr,
                                                 info->dlpi_phnum, ctx->symbol,
                                                 elfscan::NativeArch(), &ctx->result);
    return 1; // berhenti iterasi
}

// Tulis satu slot. Mengembalikan false kalau mprotect gagal.
bool PatchSlot(uintptr_t slotAddr, void* replacement, const elfscan::Result& r)
{
    void** slot = (void**)slotAddr;
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) return false;
    const uintptr_t page = slotAddr & ~((uintptr_t)pageSize - 1);
    const bool inRelro = r.relroBegin && slotAddr >= r.relroBegin && slotAddr < r.relroEnd;

    if (mprotect((void*)page, (size_t)pageSize, PROT_READ | PROT_WRITE) != 0)
        return false;

    __atomic_store_n(slot, replacement, __ATOMIC_RELEASE);

    // Kembalikan proteksi RELRO (read-only) kalau memang awalnya begitu.
    if (inRelro)
        mprotect((void*)page, (size_t)pageSize, PROT_READ);
    return true;
}

} // namespace

Status Install(const char* moduleFile, const char* symbol, void* replacement,
               void** original, int* patchedOut)
{
    if (patchedOut) *patchedOut = 0;
    if (!moduleFile || !symbol || !replacement) return kModuleNotFound;

    ScanContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.moduleFile = moduleFile;
    ctx.symbol     = symbol;
    dl_iterate_phdr(PhdrCallback, &ctx);

    if (!ctx.moduleFound)       return kModuleNotFound;
    if (ctx.result.count == 0)  return kSymbolNotImported;

    // Patch dilakukan SETELAH dl_iterate_phdr selesai (lock loader sudah dilepas).
    // `*original` DIISI DULU, baru slot ditimpa: begitu hook aktif, thread lain yang
    // memanggil fungsi ini sudah pasti bisa meneruskan ke fungsi aslinya.
    int  patched  = 0;
    bool haveOrig = false;
    for (int i = 0; i < ctx.result.count; ++i)
    {
        void** slot = (void**)ctx.result.slots[i];
        void*  cur  = *slot;
        if (cur == replacement) { ++patched; continue; }   // sudah terpasang
        if (!haveOrig)
        {
            if (original) __atomic_store_n(original, cur, __ATOMIC_RELEASE);
            haveOrig = true;
        }
        if (PatchSlot(ctx.result.slots[i], replacement, ctx.result))
            ++patched;
    }
    if (patchedOut) *patchedOut = patched;
    return patched > 0 ? kOk : kPatchFailed;
}

const char* StatusText(Status s)
{
    switch (s)
    {
        case kOk:                return "ok";
        case kModuleNotFound:    return "modul tidak ditemukan";
        case kSymbolNotImported: return "simbol tidak diimpor oleh modul";
        case kPatchFailed:       return "gagal menulis slot GOT (mprotect)";
    }
    return "?";
}

} // namespace gothook
