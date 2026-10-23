// main.cpp - adaptor tipis antara AML (Android Mod Loader) dan logika plugin.
//
// Seluruh pekerjaan nyata ada di cel_plugin.cpp / cel_renderer.cpp / got_hook.cpp, yang
// tidak bergantung pada AML sehingga bisa diuji di PC.
#include <mod/amlmod.h>
#include <mod/logger.h>

#include "cel_plugin.h"

// GUID, nama, versi, penulis. (Tanpa MYMODCFG: konfigurasi dikelola cel_config.cpp
// karena berkas INI-nya berkomentar dan bisa dimuat ulang saat game berjalan.)
MYMOD(net.celshading.gtasa, Cel Shading + Outline, 1.0.0, CelShading)

static void HostLog(int level, const char* msg)
{
    if (level >= 1) logger->Error("%s", msg);
    else            logger->Info("%s", msg);
}

static bool HostInlineHook(void* target, void* replacement, void** original)
{
    return aml->Hook(target, replacement, original);
}

ON_MOD_LOAD()
{
    logger->SetTag("CelShading");

    cel::Host host;
    host.log        = HostLog;
    host.configDir  = aml->GetConfigPath();   // .../Android/data/<paket>/configs/
    host.inlineHook = HostInlineHook;

    if (cel::PluginInit(host))
        logger->Info("siap. Edit %s untuk mengatur efek.", cel::PluginConfigPath());
}
