#!/usr/bin/env python3
"""Bandingkan hasil parser GOT (test_elf_scan) dengan `llvm-readelf -r` untuk banyak berkas ELF.

Pemakaian: check_elf_scan.py <test_elf_scan> <dir-fixture> [berkas-ELF-nyata ...]
"""
import os
import re
import subprocess
import sys

READELF = os.environ.get("READELF", "llvm-readelf")
BOUND = re.compile(r"(JUMP_SLOT|JMP_SLOT|GLOB_DAT|ABS32|ABS64|R_X86_64_64$|R_386_32$)")

failures = 0
checks = 0
SLOT_CAP = 16        # = elfscan::Result::slots[16]


def fail(msg):
    global failures
    failures += 1
    print("  GAGAL:", msg)


def readelf_expected(path):
    """{simbol: {(offset, kind)}} dari section relokasi yang terlihat oleh readelf."""
    out = subprocess.run([READELF, "-r", "-W", path], capture_output=True, text=True).stdout
    section, exp = None, {}
    for line in out.splitlines():
        m = re.match(r"Relocation section '([^']+)'", line)
        if m:
            section = m.group(1)
            continue
        tok = line.split()
        if section is None or len(tok) < 5 or not re.fullmatch(r"[0-9a-fA-F]+", tok[0]):
            continue
        if not BOUND.search(tok[2]):
            continue
        name = tok[4].split("@")[0]
        kind = "plt" if section.endswith(".plt") else "data"
        exp.setdefault(name, set()).add((int(tok[0], 16), kind))
    return exp


def scan(scanner, path, symbols, glibc_style):
    cmd = [scanner] + (["--glibc-style"] if glibc_style else []) + [path] + sorted(symbols)
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        fail(f"{os.path.basename(path)}: scanner keluar dengan kode {out.returncode}: {out.stderr.strip()}")
        return {}, {}
    res, info, cur = {}, {}, None
    for line in out.stdout.splitlines():
        m = re.match(r"sym=(\S+) count=(\d+) dynamic=(\d) packed=(\d) relro=([0-9a-f]+)-([0-9a-f]+)", line)
        if m:
            cur = m.group(1)
            res[cur] = set()
            info[cur] = {"packed": m.group(4) == "1", "relro": (int(m.group(5), 16), int(m.group(6), 16))}
            continue
        m = re.match(r"\s+slot=([0-9a-f]+) kind=(\w+)", line)
        if m and cur:
            res[cur].add((int(m.group(1), 16), m.group(2)))
    return res, info


def compare(scanner, path, label, packed_ok=False, optional=False):
    global checks
    exp = readelf_expected(path)
    if not exp:
        if optional:
            print(f"  {label}: dilewati (tidak ada relokasi ber-simbol, mis. biner statis)")
        else:
            fail(f"{label}: readelf tidak menemukan relokasi ber-simbol")
        return
    for glibc_style in (False, True):
        mode = "glibc" if glibc_style else "bionic"
        got, info = scan(scanner, path, list(exp.keys()) + ["simbol_yang_tidak_ada"], glibc_style)
        for sym, want in exp.items():
            checks += 1
            have = got.get(sym, set())
            if packed_ok and info.get(sym, {}).get("packed"):
                want = {w for w in want if w[1] == "plt"}      # slot data ada di tabel packed
            if len(want) > SLOT_CAP:
                # parser sengaja berhenti di SLOT_CAP slot (hanya butuh beberapa untuk eglSwapBuffers);
                # hasilnya harus himpunan bagian yang valid dengan ukuran tepat SLOT_CAP
                if not (have <= want and len(have) == SLOT_CAP):
                    fail(f"{label} [{mode}] {sym}: {len(have)} slot, bukan himpunan bagian {SLOT_CAP} dari {len(want)}")
            elif have != want:
                fail(f"{label} [{mode}] {sym}: parser={sorted(have)} readelf={sorted(want)}")
        checks += 1
        if got.get("simbol_yang_tidak_ada"):
            fail(f"{label} [{mode}]: simbol palsu seharusnya tidak ketemu")
    print(f"  {label}: {len(exp)} simbol dibandingkan (bionic + glibc)")


def main():
    scanner, fixdir = sys.argv[1], sys.argv[2]
    extra = sys.argv[3:]

    print("== fixture buatan clang+lld")
    for f in sorted(os.listdir(fixdir)):
        if f.startswith("lib") and f.endswith(".so"):
            compare(scanner, os.path.join(fixdir, f), f, packed_ok=("packed" in f))

    # pastikan tipe slot yang diharapkan memang ada pada fixture utama
    for f, need in (("libarm32.so", {"plt", "data"}), ("libaarch64.so", {"plt", "data"}),
                    ("libx86_64.so", {"plt", "data"}), ("libx86_32.so", {"plt", "data"}),
                    ("libarm32_noplt.so", {"data"}), ("libaarch64_noplt.so", {"data"})):
        p = os.path.join(fixdir, f)
        if not os.path.exists(p):
            continue
        got, _ = scan(scanner, p, ["eglSwapBuffers"], False)
        kinds = {k for _, k in got.get("eglSwapBuffers", set())}
        global checks
        checks += 1
        if not need <= kinds:
            fail(f"{f}: jenis slot {kinds} tidak mencakup {need}")

    # arm32_packed: slot PLT tetap ketemu dan parser tahu ada relokasi packed
    p = os.path.join(fixdir, "libarm32_packed.so")
    if os.path.exists(p):
        got, info = scan(scanner, p, ["eglSwapBuffers"], False)
        checks += 2
        if not any(k == "plt" for _, k in got.get("eglSwapBuffers", set())):
            fail("libarm32_packed.so: slot PLT tidak ditemukan")
        if not info.get("eglSwapBuffers", {}).get("packed"):
            fail("libarm32_packed.so: flag 'packed relocs' seharusnya aktif")

    for p in extra:
        if os.path.exists(p):
            print("== biner nyata:", p)
            compare(scanner, p, os.path.basename(p), optional=True)
        else:
            print("== dilewati (tidak ada):", p)

    print(f"[elf_scan] {checks - failures} lulus, {failures} gagal")
    sys.exit(1 if failures else 0)


main()
