#include "cel_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace cel {

// ---------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------
Settings::Settings()
{
    enabled = true;

    bands           = 5.0f;
    softness        = 0.15f;
    shadowFloor     = 0.10f;
    saturation      = 1.20f;
    brightness      = 1.00f;
    posterize       = 0.00f;
    posterizeLevels = 6.0f;

    outline          = true;
    outlineWidth     = 1.0f;
    outlineThreshold = 0.20f;
    outlineSoftness  = 0.12f;
    outlineStrength  = 1.00f;
    outlineColor[0] = outlineColor[1] = outlineColor[2] = 0.0f;
    quality          = 1;

    debugView = 0;
    strncpy(gameLibrary, "libGTASA.so", sizeof(gameLibrary));
    gameLibrary[sizeof(gameLibrary) - 1] = 0;
}

static float ClampF(float v, float lo, float hi)
{
    if (!(v == v)) return lo;           // NaN
    return v < lo ? lo : (v > hi ? hi : v);
}

void Settings::Clamp()
{
    bands            = (float)(int)(ClampF(bands, 2.0f, 16.0f) + 0.5f);
    softness         = ClampF(softness, 0.002f, 1.0f);
    shadowFloor      = ClampF(shadowFloor, 0.0f, 0.6f);
    saturation       = ClampF(saturation, 0.0f, 3.0f);
    brightness       = ClampF(brightness, 0.3f, 2.0f);
    posterize        = ClampF(posterize, 0.0f, 1.0f);
    posterizeLevels  = (float)(int)(ClampF(posterizeLevels, 2.0f, 32.0f) + 0.5f);

    outlineWidth     = ClampF(outlineWidth, 0.25f, 6.0f);
    outlineThreshold = ClampF(outlineThreshold, 0.0f, 2.0f);
    outlineSoftness  = ClampF(outlineSoftness, 0.01f, 1.0f);
    outlineStrength  = ClampF(outlineStrength, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i) outlineColor[i] = ClampF(outlineColor[i], 0.0f, 1.0f);
    quality          = quality <= 0 ? 0 : 1;

    debugView        = debugView < 0 ? 0 : (debugView > 2 ? 2 : debugView);
    gameLibrary[sizeof(gameLibrary) - 1] = 0;
    if (!gameLibrary[0]) { strncpy(gameLibrary, "libGTASA.so", sizeof(gameLibrary)); gameLibrary[sizeof(gameLibrary) - 1] = 0; }
}

// ---------------------------------------------------------------------------------------
// Parser INI
// ---------------------------------------------------------------------------------------
namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Pangkas spasi di kedua ujung pada buffer tulis-bisa (menggeser pointer awal).
char* Trim(char* s)
{
    while (*s && IsSpace(*s)) ++s;
    size_t n = strlen(s);
    while (n > 0 && IsSpace(s[n - 1])) s[--n] = 0;
    return s;
}

bool ParseBool(const char* v, bool def)
{
    if (!strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") ||
        !strcasecmp(v, "on") || !strcasecmp(v, "ya"))
        return true;
    if (!strcasecmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "no") ||
        !strcasecmp(v, "off") || !strcasecmp(v, "tidak"))
        return false;
    return def;
}

bool ParseFloat(const char* v, float* out)
{
    char* end = nullptr;
    double d = strtod(v, &end);
    if (end == v) return false;
    // izinkan koma desimal gaya Indonesia: "0,25"
    if (*end == ',' && end[1] >= '0' && end[1] <= '9')
    {
        char tmp[64];
        strncpy(tmp, v, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
        for (char* p = tmp; *p; ++p) if (*p == ',') *p = '.';
        d = strtod(tmp, &end);
    }
    *out = (float)d;
    return true;
}

// "R G B" atau "R,G,B" dengan nilai 0..255
bool ParseColor(const char* v, float out[3])
{
    float tmp[3];
    const char* p = v;
    for (int i = 0; i < 3; ++i)
    {
        while (*p && (IsSpace(*p) || *p == ',')) ++p;
        char* end = nullptr;
        long n = strtol(p, &end, 10);
        if (end == p) return false;
        tmp[i] = (float)(n < 0 ? 0 : (n > 255 ? 255 : n)) / 255.0f;
        p = end;
    }
    out[0] = tmp[0]; out[1] = tmp[1]; out[2] = tmp[2];
    return true;
}

void ApplyKey(Settings* s, const char* key, const char* val)
{
    float f;
    #define KEY(name) (!strcasecmp(key, name))
    if      (KEY("Enabled"))          s->enabled = ParseBool(val, s->enabled);
    else if (KEY("ShadeBands"))       { if (ParseFloat(val, &f)) s->bands = f; }
    else if (KEY("BandSoftness"))     { if (ParseFloat(val, &f)) s->softness = f; }
    else if (KEY("ShadowFloor"))      { if (ParseFloat(val, &f)) s->shadowFloor = f; }
    else if (KEY("Saturation"))       { if (ParseFloat(val, &f)) s->saturation = f; }
    else if (KEY("Brightness"))       { if (ParseFloat(val, &f)) s->brightness = f; }
    else if (KEY("ColorPosterize"))   { if (ParseFloat(val, &f)) s->posterize = f; }
    else if (KEY("ColorLevels"))      { if (ParseFloat(val, &f)) s->posterizeLevels = f; }
    else if (KEY("Outline"))          s->outline = ParseBool(val, s->outline);
    else if (KEY("OutlineWidth"))     { if (ParseFloat(val, &f)) s->outlineWidth = f; }
    else if (KEY("OutlineThreshold")) { if (ParseFloat(val, &f)) s->outlineThreshold = f; }
    else if (KEY("OutlineSoftness"))  { if (ParseFloat(val, &f)) s->outlineSoftness = f; }
    else if (KEY("OutlineStrength"))  { if (ParseFloat(val, &f)) s->outlineStrength = f; }
    else if (KEY("OutlineColor"))     ParseColor(val, s->outlineColor);
    else if (KEY("Quality"))          { if (ParseFloat(val, &f)) s->quality = (int)f; }
    else if (KEY("DebugView"))        { if (ParseFloat(val, &f)) s->debugView = (int)f; }
    else if (KEY("GameLibrary"))
    {
        if (val[0]) { strncpy(s->gameLibrary, val, sizeof(s->gameLibrary) - 1); s->gameLibrary[sizeof(s->gameLibrary) - 1] = 0; }
    }
    // kunci tak dikenal diabaikan begitu saja
    #undef KEY
}

} // namespace

void ParseIni(const char* text, size_t len, Settings* s)
{
    if (!text || !s) return;
    char line[256];
    size_t pos = 0;
    while (pos < len)
    {
        // ambil satu baris
        size_t n = 0;
        while (pos < len && text[pos] != '\n')
        {
            if (n < sizeof(line) - 1) line[n++] = text[pos];
            ++pos;
        }
        if (pos < len) ++pos;   // lewati '\n'
        line[n] = 0;

        char* l = Trim(line);
        if (!*l || *l == ';' || *l == '#' || *l == '[') continue;

        char* eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = Trim(l);
        char* val = eq + 1;
        // buang komentar di ujung nilai
        for (char* p = val; *p; ++p)
            if (*p == ';' || *p == '#') { *p = 0; break; }
        val = Trim(val);
        if (!*key) continue;
        ApplyKey(s, key, val);
    }
    s->Clamp();
}

bool LoadFile(const char* path, Settings* s)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    const size_t cap = 32768;                 // berkas INI normal < 4 KB
    char* buf = (char*)malloc(cap);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    Settings tmp = *s;
    ParseIni(buf, n, &tmp);
    *s = tmp;
    free(buf);
    return true;
}

// ---------------------------------------------------------------------------------------
// Berkas bawaan
// ---------------------------------------------------------------------------------------
static const char kDefaultIni[] =
"; =====================================================================\n"
";  CelShading.ini  -  Cel Shading + Outline untuk GTA San Andreas (AML)\n"
";\n"
";  File ini dibaca ulang otomatis kira-kira tiap 1-2 detik saat game\n"
";  berjalan: ubah nilai, simpan, lalu lihat hasilnya langsung.\n"
";  Baris berawalan ; atau # adalah komentar.\n"
"; =====================================================================\n"
"\n"
"[CelShading]\n"
"\n"
"; Nyalakan / matikan seluruh efek.  1 = nyala, 0 = mati\n"
"Enabled = 1\n"
"\n"
"; ------------------------- CEL SHADING -------------------------------\n"
"; Jumlah tingkat terang-gelap. Makin kecil makin tegas/kartun.  (2 - 16)\n"
"ShadeBands = 5\n"
"\n"
"; Kehalusan peralihan antar pita. 0.01 = tajam, 1 = sangat halus.  (0.01 - 1)\n"
"BandSoftness = 0.15\n"
"\n"
"; Kecerahan minimum area tergelap supaya bayangan tidak hitam pekat.  (0 - 0.6)\n"
"ShadowFloor = 0.10\n"
"\n"
"; Saturasi warna. 1 = asli, di atas 1 = lebih mencolok.  (0 - 3)\n"
"Saturation = 1.20\n"
"\n"
"; Pengali kecerahan akhir.  (0.3 - 2)\n"
"Brightness = 1.00\n"
"\n"
"; Posterisasi warna per-kanal (palet terbatas). 0 = mati, 1 = penuh.  (0 - 1)\n"
"ColorPosterize = 0.00\n"
"\n"
"; Jumlah level per kanal untuk posterisasi.  (2 - 32)\n"
"ColorLevels = 6\n"
"\n"
"; ---------------------------- OUTLINE --------------------------------\n"
"; Nyalakan garis tepi.  1 = nyala, 0 = mati\n"
"Outline = 1\n"
"\n"
"; Tebal garis, dalam piksel pada layar setinggi 720 (otomatis diskalakan\n"
"; ke resolusi layar kamu).  (0.25 - 6)\n"
"OutlineWidth = 1.0\n"
"\n"
"; Ambang kontras warna. Kecil = banyak garis (termasuk detail tekstur),\n"
"; besar = hanya tepi objek yang jelas.  (0 - 2)\n"
"; Muncul bintik/garis berlebihan di tekstur?  Naikkan ke 0.30.\n"
"; Garis di tepi objek kurang?  Turunkan ke 0.14.\n"
"OutlineThreshold = 0.20\n"
"\n"
"; Kelembutan tepi garis.  (0.01 - 1)\n"
"OutlineSoftness = 0.12\n"
"\n"
"; Kepekatan garis. 1 = penuh.  (0 - 1)\n"
"OutlineStrength = 1.00\n"
"\n"
"; Warna garis: R G B, masing-masing 0 - 255.  (0 0 0 = hitam)\n"
"OutlineColor = 0 0 0\n"
"\n"
"; Kualitas deteksi tepi.  1 = bagus (Sobel, 8 sampel)  0 = cepat (4 sampel)\n"
"; Pakai 0 kalau FPS turun di HP kamu.\n"
"Quality = 1\n"
"\n"
"; --------------------------- LANJUTAN --------------------------------\n"
"; Mode tuning: 0 = normal, 1 = tampilkan hanya peta garis (putih = tanpa garis),\n"
"; 2 = tampilkan cel shading saja tanpa garis.\n"
"DebugView = 0\n"
"\n"
"; Pustaka game yang di-hook. Hanya ubah kalau game-nya bukan GTA SA\n"
"; (butuh restart game agar berlaku).\n"
"GameLibrary = libGTASA.so\n";

const char* DefaultIniText() { return kDefaultIni; }

bool WriteDefaultFile(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const size_t n = sizeof(kDefaultIni) - 1;
    const bool ok = fwrite(kDefaultIni, 1, n, f) == n;
    fclose(f);
    return ok;
}

} // namespace cel
