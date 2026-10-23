// Di-include paksa (-include) saat mengompilasi main.cpp di PC x86-64.
// <mod/amlmod.h> hanya mau dikompilasi untuk ARM, jadi kita pura-pura ARM32 SETELAH semua
// header sistem yang dibutuhkan sudah ter-include (agar header sistem tidak ikut tertipu).
#pragma once
#include <stdio.h>
#include <ctype.h>
#include <cstring>
#include <stdlib.h>
#include <signal.h>
#include <math.h>
#include <algorithm>
#include <stdint.h>
#include <type_traits>
#include <initializer_list>
#include <vector>
#include <dlfcn.h>
#include <jni.h>
#include <atomic>
#include <stdarg.h>
#include <time.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#ifndef __arm__
#define __arm__ 1
#endif
