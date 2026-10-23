// check.h - kerangka tes minimal (tanpa dependensi).
#pragma once
#include <stdio.h>

static int g_checksPassed = 0;
static int g_checksFailed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) { ++g_checksPassed; }                                          \
        else { ++g_checksFailed; fprintf(stderr, "  GAGAL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                       \
    do {                                                                         \
        long long va_ = (long long)(a), vb_ = (long long)(b);                    \
        if (va_ == vb_) { ++g_checksPassed; }                                    \
        else { ++g_checksFailed; fprintf(stderr, "  GAGAL %s:%d: %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); } \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                    \
    do {                                                                         \
        double va_ = (double)(a), vb_ = (double)(b);                             \
        if ((va_ > vb_ ? va_ - vb_ : vb_ - va_) <= (eps)) { ++g_checksPassed; } \
        else { ++g_checksFailed; fprintf(stderr, "  GAGAL %s:%d: %s ~= %s  (%g vs %g)\n", __FILE__, __LINE__, #a, #b, va_, vb_); } \
    } while (0)

static inline int TestSummary(const char* name)
{
    printf("[%s] %d lulus, %d gagal\n", name, g_checksPassed, g_checksFailed);
    return g_checksFailed ? 1 : 0;
}
