#!/bin/sh
# Menghasilkan pustaka ELF NYATA (dibuat clang + lld, linker yang sama dengan NDK) untuk
# beberapa arsitektur, tanpa butuh libc/NDK. Dipakai tes parser GOT (test_elf_scan).
#
# Setiap pustaka mengimpor eglSwapBuffers & eglDestroyContext:
#   game_frame()   memanggil keduanya lewat PLT          -> R_*_JUMP_SLOT
#   g_swap_ptr     pointer data yang diinisialisasi      -> R_*_ABS / R_*_GLOB_DAT
set -eu
OUT="${1:-build/fixtures}"
mkdir -p "$OUT"
cat > "$OUT/fix.c" <<'EOC'
extern int eglSwapBuffers(void*, void*);
extern int eglDestroyContext(void*, void*);
int (*volatile g_swap_ptr)(void*, void*) = eglSwapBuffers;
int game_frame(void* a, void* b)  { return eglSwapBuffers(a, b) + eglDestroyContext(a, b); }
int game_frame2(void* a, void* b) { return g_swap_ptr(a, b); }
EOC

# nama | target clang | flag tambahan compile | flag tambahan link
build() {
    name="$1"; target="$2"; cflags="$3"; ldflags="$4"
    clang --target="$target" -c -fPIC -ffreestanding -nostdinc -O1 $cflags "$OUT/fix.c" -o "$OUT/$name.o"
    ld.lld -shared -z now -z relro -soname "lib$name.so" $ldflags -o "$OUT/lib$name.so" "$OUT/$name.o"
    echo "dibuat: $OUT/lib$name.so"
}

build arm32           armv7a-linux-androideabi21 ""          ""
build arm32_noplt     armv7a-linux-androideabi21 "-fno-plt"  ""
build arm32_packed    armv7a-linux-androideabi21 ""          "--pack-dyn-relocs=android"
build aarch64         aarch64-linux-android21    ""          ""
build aarch64_noplt   aarch64-linux-android21    "-fno-plt"  ""
build aarch64_lazy    aarch64-linux-android21    ""          "-z lazy"
build x86_32          i686-linux-android21       ""          ""
build x86_64          x86_64-linux-android21     ""          ""
