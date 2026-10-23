// elf_scan.h - mencari slot GOT/PLT milik simbol impor pada ELF yang sudah ter-load.
//
// Header ini sengaja MANDIRI: tidak memakai <elf.h>/<link.h>, tidak memakai libc sama
// sekali. Struktur ELF didefinisikan sendiri untuk kelas 32-bit dan 64-bit, sehingga:
//   * hasilnya identik di Android (bionic), glibc, dan musl,
//   * parser bisa diuji di PC terhadap gambar ELF ARM32/ARM64 (tests/test_elf_scan.cpp).
//
// Hanya MEMBACA memori. Penulisan/patch dilakukan di got_hook.cpp.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace elfscan {

// ---------------------------------------------------------------------------------------
// Definisi ELF minimal
// ---------------------------------------------------------------------------------------
struct Elf32
{
    struct Phdr { uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align; };
    struct Dyn  { int32_t d_tag; uint32_t d_val; };
    struct Sym  { uint32_t st_name, st_value, st_size; uint8_t st_info, st_other; uint16_t st_shndx; };
    struct Rel  { uint32_t r_offset, r_info; };
    struct Rela { uint32_t r_offset, r_info; int32_t r_addend; };
    static uint32_t RelSym(uint32_t info)  { return info >> 8; }
    static uint32_t RelType(uint32_t info) { return info & 0xffu; }
};

struct Elf64
{
    struct Phdr { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; };
    struct Dyn  { int64_t d_tag; uint64_t d_val; };
    struct Sym  { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; };
    struct Rel  { uint64_t r_offset, r_info; };
    struct Rela { uint64_t r_offset, r_info; int64_t r_addend; };
    static uint32_t RelSym(uint64_t info)  { return (uint32_t)(info >> 32); }
    static uint32_t RelType(uint64_t info) { return (uint32_t)(info & 0xffffffffu); }
};

enum : uint32_t
{
    PT_LOAD_     = 1,
    PT_DYNAMIC_  = 2,
    PT_GNU_RELRO_ = 0x6474e552u,
};

enum : int64_t
{
    DT_NULL_     = 0,
    DT_PLTRELSZ_ = 2,
    DT_STRTAB_   = 5,
    DT_SYMTAB_   = 6,
    DT_RELA_     = 7,
    DT_RELASZ_   = 8,
    DT_REL_      = 17,
    DT_RELSZ_    = 18,
    DT_PLTREL_   = 20,
    DT_JMPREL_   = 23,
    DT_ANDROID_REL_  = 0x6000000f,
    DT_ANDROID_RELA_ = 0x60000011,
    DT_RELR_     = 36,
};

// Nomor tipe relokasi yang mengikat simbol ke sebuah slot data (bukan JUMP_SLOT).
struct Arch
{
    uint32_t relGlobDat;
    uint32_t relAbs;
};

static const Arch kArchArm32   = { 21u, 2u };      // R_ARM_GLOB_DAT, R_ARM_ABS32
static const Arch kArchAarch64 = { 1025u, 257u };  // R_AARCH64_GLOB_DAT, R_AARCH64_ABS64
static const Arch kArchX86     = { 6u, 1u };       // R_386_GLOB_DAT / R_X86_64_GLOB_DAT, *_32 / *_64

inline Arch NativeArch()
{
#if defined(__aarch64__)
    return kArchAarch64;
#elif defined(__arm__)
    return kArchArm32;
#else
    return kArchX86;
#endif
}

enum SlotKind : uint8_t
{
    kSlotPlt  = 0,   // dari DT_JMPREL  (.rel(a).plt  - R_*_JUMP_SLOT)
    kSlotData = 1,   // dari DT_REL/RELA (.rel(a).dyn - GLOB_DAT / ABS)
};

struct Result
{
    enum { kMaxSlots = 16 };
    uintptr_t slots[kMaxSlots];     // alamat ABSOLUT entri GOT
    uint8_t   kinds[kMaxSlots];
    int       count;
    uintptr_t relroBegin;           // rentang PT_GNU_RELRO (absolut), 0 jika tidak ada
    uintptr_t relroEnd;
    bool      hasDynamic;           // PT_DYNAMIC ditemukan
    bool      hasPackedRelocs;      // DT_ANDROID_REL(A) / DT_RELR ada (tidak di-parse)
};

namespace detail {

inline bool StrEq(const char* a, const char* b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

inline void AddSlot(Result* out, uintptr_t addr, SlotKind kind)
{
    for (int i = 0; i < out->count; ++i)
        if (out->slots[i] == addr) return;
    if (out->count >= Result::kMaxSlots) return;
    out->slots[out->count]  = addr;
    out->kinds[out->count]  = (uint8_t)kind;
    out->count++;
}

} // namespace detail

// Cari semua slot GOT yang terikat ke simbol `sym` pada modul yang di-load di `bias`.
//   bias   : load bias (dlpi_addr). Untuk ELF non-PIE bernilai 0.
//   phdrs  : pointer ke tabel program header (dlpi_phdr), bertipe E::Phdr.
//   phnum  : jumlah program header.
template <class E>
inline void FindImportSlots(uintptr_t bias, const void* phdrs, unsigned phnum,
                            const char* sym, const Arch& arch, Result* out)
{
    typedef typename E::Phdr Phdr;
    typedef typename E::Dyn  Dyn;
    typedef typename E::Sym  Sym;
    typedef typename E::Rel  Rel;
    typedef typename E::Rela Rela;

    out->count = 0;
    out->relroBegin = out->relroEnd = 0;
    out->hasDynamic = false;
    out->hasPackedRelocs = false;
    if (!phdrs || !sym || !*sym) return;

    const Phdr* ph = (const Phdr*)phdrs;
    uintptr_t dynAddr = 0;
    size_t    dynBytes = 0;
    for (unsigned i = 0; i < phnum; ++i)
    {
        if (ph[i].p_type == PT_DYNAMIC_)
        {
            dynAddr  = bias + (uintptr_t)ph[i].p_vaddr;
            dynBytes = (size_t)ph[i].p_memsz;
        }
        else if (ph[i].p_type == PT_GNU_RELRO_)
        {
            out->relroBegin = bias + (uintptr_t)ph[i].p_vaddr;
            out->relroEnd   = out->relroBegin + (uintptr_t)ph[i].p_memsz;
        }
    }
    if (!dynAddr || dynBytes < sizeof(Dyn)) return;
    out->hasDynamic = true;

    // Nilai d_ptr: bionic membiarkannya sebagai vaddr (belum ditambah bias), glibc/musl
    // menambahkan bias di tempat. Vaddr selalu < bias pada pustaka hasil mmap, jadi
    // "kalau lebih kecil dari bias -> tambahkan bias" aman untuk keduanya.
    auto fix = [bias](uint64_t v) -> uintptr_t
    {
        uintptr_t p = (uintptr_t)v;
        if (p < bias) p += bias;
        return p;
    };

    uintptr_t symtab = 0, strtab = 0, jmprel = 0, rel = 0, rela = 0;
    size_t    pltrelsz = 0, relsz = 0, relasz = 0;
    int64_t   pltrelType = 0;

    const Dyn* d = (const Dyn*)dynAddr;
    const size_t dn = dynBytes / sizeof(Dyn);
    for (size_t i = 0; i < dn; ++i)
    {
        const int64_t tag = (int64_t)d[i].d_tag;
        if (tag == DT_NULL_) break;
        switch (tag)
        {
            case DT_SYMTAB_:   symtab   = fix(d[i].d_val);        break;
            case DT_STRTAB_:   strtab   = fix(d[i].d_val);        break;
            case DT_JMPREL_:   jmprel   = fix(d[i].d_val);        break;
            case DT_PLTRELSZ_: pltrelsz = (size_t)d[i].d_val;     break;
            case DT_PLTREL_:   pltrelType = (int64_t)d[i].d_val;  break;
            case DT_REL_:      rel      = fix(d[i].d_val);        break;
            case DT_RELSZ_:    relsz    = (size_t)d[i].d_val;     break;
            case DT_RELA_:     rela     = fix(d[i].d_val);        break;
            case DT_RELASZ_:   relasz   = (size_t)d[i].d_val;     break;
            case DT_ANDROID_REL_:
            case DT_ANDROID_RELA_:
            case DT_RELR_:     out->hasPackedRelocs = true;        break;
            default: break;
        }
    }

    if (!symtab || !strtab) return;

    // Satu fungsi pemindai untuk REL maupun RELA (hanya beda ukuran entri & addend).
    struct Scanner
    {
        static void Run(uintptr_t table, size_t bytes, bool isRela, bool anyType, SlotKind kind,
                        uintptr_t symtabAddr, uintptr_t strtabAddr, uintptr_t biasAddr,
                        const char* wanted, const Arch& a, Result* res)
        {
            if (!table || !bytes) return;
            const size_t ent = isRela ? sizeof(Rela) : sizeof(Rel);
            const size_t cnt = bytes / ent;
            for (size_t i = 0; i < cnt; ++i)
            {
                const uintptr_t e = table + i * ent;
                // Kedua struct berawalan sama: r_offset lalu r_info.
                const Rel* r = (const Rel*)e;
                const uint32_t symIdx = E::RelSym(r->r_info);
                if (symIdx == 0) continue;                     // RELATIVE dan sejenisnya
                const uint32_t type = E::RelType(r->r_info);
                if (!anyType && type != a.relGlobDat && type != a.relAbs) continue;
                const Sym* s = (const Sym*)symtabAddr + symIdx;
                const char* name = (const char*)strtabAddr + s->st_name;
                if (detail::StrEq(name, wanted))
                    detail::AddSlot(res, biasAddr + (uintptr_t)r->r_offset, kind);
            }
        }
    };

    // 1) tabel PLT (selalu berisi JUMP_SLOT; tipe tak perlu dicek)
    if (jmprel && pltrelsz)
    {
        const bool isRela = (pltrelType == DT_RELA_);
        Scanner::Run(jmprel, pltrelsz, isRela, true, kSlotPlt, symtab, strtab, bias, sym, arch, out);
    }
    // 2) tabel relokasi data (GLOB_DAT/ABS) - dipakai kalau alamat fungsi diambil / -fno-plt
    Scanner::Run(rel,  relsz,  false, false, kSlotData, symtab, strtab, bias, sym, arch, out);
    Scanner::Run(rela, relasz, true,  false, kSlotData, symtab, strtab, bias, sym, arch, out);
}

} // namespace elfscan
