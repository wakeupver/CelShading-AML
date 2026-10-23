// got_hook.h - hook fungsi impor lewat penggantian entri GOT/PLT sebuah modul.
//
// Dipakai untuk mencegat eglSwapBuffers yang dipanggil libGTASA.so TANPA perlu offset
// per-versi game: slot dicari lewat tabel relokasi dinamis ELF (elf_scan.h).
#pragma once

namespace gothook {

enum Status
{
    kOk               = 0,   // minimal satu slot kini menunjuk ke replacement
    kModuleNotFound   = 1,   // tidak ada modul ter-load dengan nama itu
    kSymbolNotImported = 2,  // modul ada, tetapi tidak mengimpor simbol itu
    kPatchFailed      = 3,   // slot ketemu, mprotect/tulis gagal
};

// Cari modul yang NAMA BERKAS-nya sama dengan `moduleFile` (mis. "libGTASA.so"), lalu
// ganti semua slot GOT yang terikat ke `symbol` dengan `replacement`.
//   *original   : diisi alamat fungsi sebelumnya (hanya kalau ada slot yang diubah).
//   *patchedOut : jumlah slot yang diubah (boleh null).
Status Install(const char* moduleFile, const char* symbol, void* replacement,
               void** original, int* patchedOut = nullptr);

const char* StatusText(Status s);

} // namespace gothook
