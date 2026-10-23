LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE        := CelShading
LOCAL_CPP_EXTENSION := .cpp

LOCAL_SRC_FILES := \
    src/main.cpp \
    src/cel_plugin.cpp \
    src/cel_renderer.cpp \
    src/cel_config.cpp \
    src/got_hook.cpp \
    mod/logger.cpp

# "mod/" (header AML) dan "src/" dicari dari akar proyek.
LOCAL_C_INCLUDES := $(LOCAL_PATH) $(LOCAL_PATH)/src

LOCAL_CFLAGS   += -O2 -DNDEBUG -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter
LOCAL_CPPFLAGS += -std=c++17 -fvisibility-inlines-hidden

LOCAL_LDLIBS   := -llog -lEGL -lGLESv2
# --exclude-libs: simbol libc++ statis tidak diekspor (hindari bentrok dengan libc++ milik game).
# max-page-size=16384: kompatibel dengan Android 15+ yang memakai halaman 16 KB.
LOCAL_LDFLAGS  += -Wl,--no-undefined -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384

include $(BUILD_SHARED_LIBRARY)
