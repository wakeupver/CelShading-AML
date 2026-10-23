// cel_config.h - pengaturan plugin + pembaca/penulis berkas INI (tanpa dependensi AML).
#pragma once

#include <stddef.h>

namespace cel {

struct Settings
{
    // --- umum ---
    bool  enabled;

    // --- cel shading ---
    float bands;            // jumlah pita terang-gelap
    float softness;         // kehalusan transisi antar pita (0..1)
    float shadowFloor;      // kecerahan minimum pita tergelap
    float saturation;       // pengali saturasi
    float brightness;       // pengali kecerahan akhir
    float posterize;        // campuran posterisasi per-kanal (0..1)
    float posterizeLevels;  // jumlah level per kanal untuk posterisasi

    // --- outline ---
    bool  outline;
    float outlineWidth;     // tebal garis (piksel pada tinggi layar 720; otomatis diskalakan)
    float outlineThreshold; // ambang kontras warna agar terbentuk garis
    float outlineSoftness;  // lebar transisi di atas ambang
    float outlineStrength;  // 0..1, seberapa pekat garis
    float outlineColor[3];  // RGB 0..1
    int   quality;          // 0 = cepat (4 sampel), 1 = bagus (Sobel 8 sampel)

    // --- lanjutan ---
    int   debugView;        // 0 normal, 1 hanya peta garis, 2 hanya cel shading
    char  gameLibrary[64];  // nama berkas pustaka game yang di-hook

    Settings();
    void Clamp();           // paksa semua nilai ke rentang aman
};

// Menimpa field di `s` untuk setiap kunci yang ada di `text`; kunci yang tidak ada
// dibiarkan. Hasil akhirnya selalu di-Clamp().
void ParseIni(const char* text, size_t len, Settings* s);

// Baca berkas INI. false kalau berkas tidak bisa dibuka (s tidak diubah).
bool LoadFile(const char* path, Settings* s);

// Tulis berkas INI bawaan (berkomentar). true kalau berhasil.
bool WriteDefaultFile(const char* path);

const char* DefaultIniText();

} // namespace cel
