# Tes untuk plugin CelShading yang berjalan di PC / CI (bukan di Android).
#
#   make -C tests            semua tes yang bisa dijalankan di mesin ini
#   make -C tests config     parser INI + berkas config/CelShading.ini == teks bawaan di kode
#   make -C tests got        parser ELF (vs llvm-readelf) + hook GOT sungguhan pada pustaka hasil dlopen
#   make -C tests gl         renderer di Mesa software (EGL surfaceless): ES 3.x dan ES 2.0
#   make -C tests e2e        end-to-end dengan "game palsu" (hook, reload INI, context hilang, jalur cadangan)
#   make -C tests clean
#
# Kebutuhan:  g++ dan gcc (glibc, Linux x86-64);  tes "gl" dan "e2e" butuh Mesa
#             (apt install libegl1 libgles2 libgl1-mesa-dri libegl-mesa0)
#             dan header Khronos EGL/GLES2 (apt install libegl-dev libgles-dev,
#             atau arahkan GL_INC ke salinan https://github.com/KhronosGroup/EGL-Registry
#             dan https://github.com/KhronosGroup/OpenGL-Registry (folder "api")).
#             Pemeriksaan ELF lintas-arsitektur butuh clang + ld.lld + llvm-readelf (dilewati bila tidak ada).
#
#   make -C tests gl GL_INC="-I/jalur/gl/api -I/jalur/egl/api"

ROOT    := ..
SRC     := $(ROOT)/src
BUILD   := build

CXX     ?= g++
CC      ?= gcc
GL_INC  ?=
INC     := -I$(SRC) -I. $(GL_INC)
CXXFLAGS_TEST := -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti
# Pakai nama berkas runtime langsung supaya tidak butuh symlink paket -dev (libEGL.so).
GLLIBS  := -l:libEGL.so.1 -l:libGLESv2.so.2
GLENV   := EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1
# Biner x86-64 nyata (glibc) yang ikut dibandingkan dengan llvm-readelf; yang tidak ada dilewati.
ELF_REAL ?= /lib/x86_64-linux-gnu/libstdc++.so.6 /lib/x86_64-linux-gnu/libEGL.so.1 \
            /lib/x86_64-linux-gnu/libGLESv2.so.2 /lib/x86_64-linux-gnu/libgbm.so.1

.PHONY: all test config got elf gl e2e clean
all test: config got elf gl e2e
	@echo "== semua tes selesai"

$(BUILD):
	@mkdir -p $(BUILD)

# ----------------------------------------------------------------- config
$(BUILD)/test_config: test_config.cpp check.h $(SRC)/cel_config.cpp $(SRC)/cel_config.h | $(BUILD)
	$(CXX) $(CXXFLAGS_TEST) -I$(SRC) -I. test_config.cpp $(SRC)/cel_config.cpp -o $@

config: $(BUILD)/test_config
	$(BUILD)/test_config $(ROOT)/config/CelShading.ini

# ----------------------------------------------------------------- hook GOT
$(BUILD)/libgotdep.so: gotfix_dep.c | $(BUILD)
	$(CC) -shared -fPIC -Wl,-z,relro,-z,now -Wl,-soname,libgotdep.so $< -o $@

$(BUILD)/libgottarget.so: gotfix_target.c $(BUILD)/libgotdep.so
	$(CC) -shared -fPIC -Wl,-z,relro,-z,now -Wl,-rpath,'$$ORIGIN' $< -L$(BUILD) -lgotdep -o $@

$(BUILD)/test_got_hook: test_got_hook.cpp check.h $(SRC)/got_hook.cpp $(SRC)/got_hook.h $(SRC)/elf_scan.h | $(BUILD)
	$(CXX) $(CXXFLAGS_TEST) -I$(SRC) -I. test_got_hook.cpp $(SRC)/got_hook.cpp -ldl -o $@

$(BUILD)/test_elf_scan: test_elf_scan.cpp $(SRC)/elf_scan.h | $(BUILD)
	$(CXX) $(CXXFLAGS_TEST) -I$(SRC) -I. test_elf_scan.cpp -o $@

got: $(BUILD)/libgottarget.so $(BUILD)/test_got_hook
	$(BUILD)/test_got_hook $(BUILD)

elf: $(BUILD)/test_elf_scan
	@if command -v clang >/dev/null 2>&1 && command -v ld.lld >/dev/null 2>&1 && command -v llvm-readelf >/dev/null 2>&1; then \
	    set -e; sh make_fixtures.sh $(BUILD)/fixtures >/dev/null; \
	    python3 -I check_elf_scan.py $(BUILD)/test_elf_scan $(BUILD)/fixtures $(ELF_REAL); \
	else \
	    echo "[elf] clang / ld.lld / llvm-readelf tidak ditemukan - pemeriksaan ELF lintas-arsitektur dilewati"; \
	fi

# ----------------------------------------------------------------- renderer (GL)
$(BUILD)/test_renderer: test_renderer.cpp gl_test_support.h scene.h check.h $(SRC)/cel_renderer.cpp $(SRC)/cel_renderer.h $(SRC)/cel_config.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_TEST) $(INC) test_renderer.cpp $(SRC)/cel_renderer.cpp $(SRC)/cel_config.cpp \
	    -Wl,--wrap=glCopyTexSubImage2D -Wl,--wrap=glGetError $(GLLIBS) -o $@

gl: $(BUILD)/test_renderer
	$(GLENV) $(BUILD)/test_renderer
	MESA_GLES_VERSION_OVERRIDE=2.0 $(GLENV) $(BUILD)/test_renderer --es2

# ----------------------------------------------------------------- end-to-end
$(BUILD)/libfakegame.so: fakegame.cpp scene.h | $(BUILD)
	$(CXX) -shared -fPIC -std=c++17 -O1 -fvisibility=hidden -Wl,-z,relro,-z,now $(INC) fakegame.cpp $(GLLIBS) -o $@

$(BUILD)/libswapcount_shim.so: swapcount_shim.c | $(BUILD)
	$(CC) -shared -fPIC -O1 $(GL_INC) swapcount_shim.c -ldl -o $@

$(BUILD)/test_e2e: test_e2e.cpp gl_test_support.h check.h $(SRC)/cel_plugin.cpp $(SRC)/cel_renderer.cpp $(SRC)/cel_config.cpp $(SRC)/got_hook.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS_TEST) $(INC) test_e2e.cpp $(SRC)/cel_plugin.cpp $(SRC)/cel_renderer.cpp \
	    $(SRC)/cel_config.cpp $(SRC)/got_hook.cpp $(GLLIBS) -ldl -o $@

# Tiap skenario di proses sendiri (PluginInit memegang state global). "normal" memakai
# LD_PRELOAD agar pemanggilan berantai ke eglSwapBuffers/eglDestroyContext/eglTerminate asli ikut dihitung.
e2e: $(BUILD)/test_e2e $(BUILD)/libfakegame.so $(BUILD)/libswapcount_shim.so
	@set -e; \
	GAME=$$(pwd)/$(BUILD)/libfakegame.so; SHIM=$$(pwd)/$(BUILD)/libswapcount_shim.so; \
	for s in normal config-create fallback-ok fallback-fail nohook; do \
	    rm -rf $(BUILD)/e2e-$$s; \
	    LD_PRELOAD=$$SHIM env $(GLENV) $(BUILD)/test_e2e $$s $$(pwd)/$(BUILD)/e2e-$$s $$GAME; \
	done

clean:
	rm -rf $(BUILD)
