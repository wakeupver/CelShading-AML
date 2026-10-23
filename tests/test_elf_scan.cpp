// CLI uji parser GOT (elf_scan.h): memetakan segmen PT_LOAD sebuah berkas ELF ke memori
// seperti loader (tanpa menjalankan apa pun), lalu mencari slot GOT untuk simbol-simbol
// yang diminta. Keluarannya dibandingkan dengan `llvm-readelf -r` oleh check_elf_scan.py.
//
//   test_elf_scan [--glibc-style] <berkas.so> <simbol>...
//
// --glibc-style : meniru glibc, yaitu menambahkan load-bias ke nilai d_ptr di section
//                 dinamis (bionic membiarkannya sebagai vaddr). Parser harus tahan keduanya.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "elf_scan.h"

using namespace elfscan;

static std::vector<uint8_t> ReadFile(const char* path)
{
    std::vector<uint8_t> data;
    FILE* f = fopen(path, "rb");
    if (!f) return data;
    uint8_t buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    fclose(f);
    return data;
}

template <class E>
static int Run(std::vector<uint8_t>& file, bool glibcStyle, int argc, char** argv, int firstSym, const Arch& arch)
{
    typedef typename E::Phdr Phdr;
    typedef typename E::Dyn  Dyn;

    const bool is32 = sizeof(Dyn) == 8;      // Elf32::Dyn = 8 byte, Elf64::Dyn = 16 byte

    // e_phoff / e_phnum: offset dalam Ehdr berbeda untuk 32/64-bit
    uint64_t phoff; unsigned phnum;
    if (is32)
    {
        uint32_t v; memcpy(&v, &file[28], 4); phoff = v;
        uint16_t n; memcpy(&n, &file[44], 2); phnum = n;
    }
    else
    {
        memcpy(&phoff, &file[32], 8);
        uint16_t n; memcpy(&n, &file[56], 2); phnum = n;
    }
    const Phdr* ph = (const Phdr*)&file[phoff];

    uint64_t maxEnd = 0;
    for (unsigned i = 0; i < phnum; ++i)
        if (ph[i].p_type == PT_LOAD_ && ph[i].p_vaddr + ph[i].p_memsz > maxEnd) maxEnd = ph[i].p_vaddr + ph[i].p_memsz;
    size_t size = (size_t)((maxEnd + 0xfff) & ~0xfffull) + 0x1000;

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (is32) flags |= MAP_32BIT;     // supaya bias muat di 32-bit
    uint8_t* image = (uint8_t*)mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (image == MAP_FAILED) { perror("mmap"); return 2; }
    for (unsigned i = 0; i < phnum; ++i)
        if (ph[i].p_type == PT_LOAD_)
            memcpy(image + ph[i].p_vaddr, &file[ph[i].p_offset], (size_t)ph[i].p_filesz);

    const uintptr_t bias = (uintptr_t)image;

    if (glibcStyle)
    {
        for (unsigned i = 0; i < phnum; ++i)
        {
            if (ph[i].p_type != PT_DYNAMIC_) continue;
            Dyn* d = (Dyn*)(image + ph[i].p_vaddr);
            for (; (int64_t)d->d_tag != DT_NULL_; ++d)
            {
                int64_t t = (int64_t)d->d_tag;
                // tag bertipe pointer (bukan ukuran/flag)
                if (t == 3 /*PLTGOT*/ || t == 4 /*HASH*/ || t == 5 || t == 6 || t == 7 || t == 12 /*INIT*/ ||
                    t == 13 /*FINI*/ || t == 17 || t == 23 || t == 25 || t == 26 || t == 0x6ffffef5 /*GNU_HASH*/)
                    d->d_val += bias;
            }
        }
    }

    for (int a = firstSym; a < argc; ++a)
    {
        Result r;
        memset(&r, 0, sizeof(r));
        FindImportSlots<E>(bias, ph, phnum, argv[a], arch, &r);
        printf("sym=%s count=%d dynamic=%d packed=%d relro=%llx-%llx\n", argv[a], r.count,
               (int)r.hasDynamic, (int)r.hasPackedRelocs,
               (unsigned long long)(r.relroBegin ? r.relroBegin - bias : 0),
               (unsigned long long)(r.relroEnd ? r.relroEnd - bias : 0));
        for (int i = 0; i < r.count; ++i)
            printf("  slot=%llx kind=%s\n", (unsigned long long)(r.slots[i] - bias), r.kinds[i] == kSlotPlt ? "plt" : "data");
    }
    munmap(image, size);
    return 0;
}

int main(int argc, char** argv)
{
    int i = 1;
    bool glibcStyle = false;
    if (i < argc && !strcmp(argv[i], "--glibc-style")) { glibcStyle = true; ++i; }
    if (argc - i < 2) { fprintf(stderr, "pakai: %s [--glibc-style] berkas simbol...\n", argv[0]); return 2; }

    std::vector<uint8_t> file = ReadFile(argv[i]);
    if (file.size() < 64 || memcmp(file.data(), "\x7f" "ELF", 4) != 0) { fprintf(stderr, "bukan ELF: %s\n", argv[i]); return 2; }
    const int cls = file[4];
    uint16_t machine; memcpy(&machine, &file[18], 2);

    Arch arch = kArchX86;
    if (machine == 40)  arch = kArchArm32;
    if (machine == 183) arch = kArchAarch64;
    printf("file=%s class=%d machine=%u\n", argv[i], cls == 1 ? 32 : 64, machine);

    if (cls == 1) return Run<Elf32>(file, glibcStyle, argc, argv, i + 1, arch);
    return Run<Elf64>(file, glibcStyle, argc, argv, i + 1, arch);
}
