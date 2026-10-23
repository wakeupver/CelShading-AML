// Tes parser/penulis INI (tanpa GPU, tanpa Android).
#include "check.h"
#include "cel_config.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>

using namespace cel;

static void Parse(const char* text, Settings* s) { ParseIni(text, strlen(text), s); }

static std::string ReadAll(const char* path)
{
    std::string out;
    FILE* f = fopen(path, "rb");
    if (!f) return out;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

int main(int argc, char** argv)
{
    // 1) INI bawaan harus menghasilkan persis nilai bawaan Settings().
    {
        Settings def, parsed;
        Parse(DefaultIniText(), &parsed);
        CHECK(parsed.enabled == def.enabled);
        CHECK_NEAR(parsed.bands, def.bands, 1e-6);
        CHECK_NEAR(parsed.softness, def.softness, 1e-6);
        CHECK_NEAR(parsed.shadowFloor, def.shadowFloor, 1e-6);
        CHECK_NEAR(parsed.saturation, def.saturation, 1e-6);
        CHECK_NEAR(parsed.brightness, def.brightness, 1e-6);
        CHECK_NEAR(parsed.posterize, def.posterize, 1e-6);
        CHECK_NEAR(parsed.posterizeLevels, def.posterizeLevels, 1e-6);
        CHECK(parsed.outline == def.outline);
        CHECK_NEAR(parsed.outlineWidth, def.outlineWidth, 1e-6);
        CHECK_NEAR(parsed.outlineThreshold, def.outlineThreshold, 1e-6);
        CHECK_NEAR(parsed.outlineSoftness, def.outlineSoftness, 1e-6);
        CHECK_NEAR(parsed.outlineStrength, def.outlineStrength, 1e-6);
        for (int i = 0; i < 3; ++i) CHECK_NEAR(parsed.outlineColor[i], def.outlineColor[i], 1e-6);
        CHECK_EQ_INT(parsed.quality, def.quality);
        CHECK_EQ_INT(parsed.debugView, def.debugView);
        CHECK(strcmp(parsed.gameLibrary, def.gameLibrary) == 0);
    }

    // 2) config/CelShading.ini di repo harus identik dengan teks bawaan di kode.
    if (argc > 1)
    {
        std::string onDisk = ReadAll(argv[1]);
        CHECK(!onDisk.empty());
        CHECK(onDisk == std::string(DefaultIniText()));
    }

    // 3) beragam bentuk penulisan
    {
        Settings s;
        Parse("[Apa Saja]\n"
              "  enabled = OFF   ; komentar di ujung\n"
              "# komentar penuh\n"
              "SHADEBANDS=3\r\n"
              "bandsoftness = 0,25\n"           // koma desimal
              "OutlineColor = 255, 128 ,0\n"
              "outlinewidth=2.5#komentar\n"
              "KunciAsing = 123\n"               // tak dikenal -> diabaikan
              "baris tanpa sama dengan\n"
              "GameLibrary = libGTAVC.so\n", &s);
        CHECK(!s.enabled);
        CHECK_NEAR(s.bands, 3.0, 1e-6);
        CHECK_NEAR(s.softness, 0.25, 1e-6);
        CHECK_NEAR(s.outlineColor[0], 1.0, 1e-6);
        CHECK_NEAR(s.outlineColor[1], 128.0 / 255.0, 1e-6);
        CHECK_NEAR(s.outlineColor[2], 0.0, 1e-6);
        CHECK_NEAR(s.outlineWidth, 2.5, 1e-6);
        CHECK(strcmp(s.gameLibrary, "libGTAVC.so") == 0);
        CHECK_NEAR(s.saturation, 1.20, 1e-6);    // tidak disebut -> tetap bawaan
    }

    // 4) clamp & nilai sampah
    {
        Settings s;
        Parse("ShadeBands = 999\n BandSoftness = -5\n Saturation = 1e9\n Brightness = abc\n"
              "OutlineWidth = nan\n OutlineThreshold = -1\n Quality = 7\n DebugView = -3\n"
              "OutlineColor = 999 -5 300\n ColorLevels = 1\n", &s);
        CHECK_NEAR(s.bands, 16.0, 1e-6);
        CHECK(s.softness >= 0.002f && s.softness <= 0.0021f);
        CHECK_NEAR(s.saturation, 3.0, 1e-6);
        CHECK_NEAR(s.brightness, 1.0, 1e-6);          // "abc" tak terbaca -> bawaan
        CHECK_NEAR(s.outlineWidth, 0.25, 1e-6);       // NaN -> batas bawah
        CHECK_NEAR(s.outlineThreshold, 0.0, 1e-6);
        CHECK_EQ_INT(s.quality, 1);
        CHECK_EQ_INT(s.debugView, 0);
        CHECK_NEAR(s.outlineColor[0], 1.0, 1e-6);
        CHECK_NEAR(s.outlineColor[1], 0.0, 1e-6);
        CHECK_NEAR(s.outlineColor[2], 1.0, 1e-6);
        CHECK_NEAR(s.posterizeLevels, 2.0, 1e-6);
    }

    // 5) baris sangat panjang & berkas tanpa newline di akhir tidak boleh bikin crash
    {
        std::string big = "ShadeBands = 4\n" + std::string(5000, 'x') + "\nOutlineWidth = 3";
        Settings s;
        Parse(big.c_str(), &s);
        CHECK_NEAR(s.bands, 4.0, 1e-6);
        CHECK_NEAR(s.outlineWidth, 3.0, 1e-6);
        Parse("", &s);
        ParseIni(nullptr, 0, &s);
    }

    // 6) tulis berkas bawaan lalu baca kembali; berkas hilang -> false
    {
        char path[] = "/tmp/celcfg_test_XXXXXX";
        int fd = mkstemp(path);
        CHECK(fd >= 0);
        close(fd);
        CHECK(WriteDefaultFile(path));
        Settings s;
        s.bands = 9;
        CHECK(LoadFile(path, &s));
        CHECK_NEAR(s.bands, 5.0, 1e-6);          // nilai dari berkas menimpa
        CHECK(ReadAll(path) == std::string(DefaultIniText()));
        unlink(path);
        Settings t; t.bands = 7;
        CHECK(!LoadFile("/tmp/tidak/ada/sama/sekali.ini", &t));
        CHECK_NEAR(t.bands, 7.0, 1e-6);          // tidak berubah
    }

    return TestSummary("config");
}
