// cel_plugin.h - logika plugin yang TIDAK bergantung pada AML (supaya bisa diuji di PC).
//
// main.cpp hanya menjadi adaptor tipis: mengisi struct Host dengan fungsi AML lalu
// memanggil PluginInit().
#pragma once

#include "cel_renderer.h"

namespace cel {

struct Host
{
    // level: 0 = info, 1 = error
    void (*log)(int level, const char* msg);

    // Direktori konfigurasi (mis. ".../Android/data/com.rockstargames.gtasa/configs/").
    const char* configDir;

    // Opsional: hook inline untuk jalur cadangan (aml->Hook). Boleh null.
    bool (*inlineHook)(void* target, void* replacement, void** original);
};

// Pasang plugin. true kalau hook eglSwapBuffers berhasil dipasang.
// Aman dipanggil berulang (pemanggilan kedua tidak melakukan apa-apa).
bool PluginInit(const Host& host);

// Untuk tes/diagnostik.
Renderer*   PluginRenderer();
const char* PluginConfigPath();

} // namespace cel
