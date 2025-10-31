/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Base64 encoding with runtime CPU feature detection
 */

#include "base64.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

/* Include architecture-specific implementations */
#include "arch/neon-base64.h"
#include "arch/sse-base64.h"

#if defined(__aarch64__) || defined(__arm__)
/* ARM/ARM64: Check for NEON support */

#if defined(__linux__)
#include <sys/auxv.h>

#ifndef HWCAP_NEON
#define HWCAP_NEON (1 << 12)
#endif

#ifndef HWCAP_ASIMD
#define HWCAP_ASIMD (1 << 1)
#endif

static bool cpu_has_neon(void)
{
    unsigned long hwcaps = getauxval(AT_HWCAP);

#if defined(__aarch64__)
    /* AArch64: NEON (Advanced SIMD) is mandatory */
    return (hwcaps & HWCAP_ASIMD) != 0;
#else
    /* ARMv7: Check NEON feature bit */
    return (hwcaps & HWCAP_NEON) != 0;
#endif
}

#elif defined(__APPLE__)
#include <sys/sysctl.h>

static bool cpu_has_neon(void)
{
#if defined(__aarch64__)
    /* Apple Silicon (M1/M2/M3): NEON is always available */
    return true;
#else
    /* iOS ARMv7: Check hw.optional.neon */
    int has_neon = 0;
    size_t len = sizeof(has_neon);
    if (sysctlbyname("hw.optional.neon", &has_neon, &len, NULL, 0) == 0)
        return has_neon != 0;
    return false;
#endif
}

#else
/* Other platforms: assume NEON is available if compiled with NEON support */
static bool cpu_has_neon(void)
{
#if defined(__ARM_NEON)
    return true;
#else
    return false;
#endif
}
#endif /* __linux__ / __APPLE__ */

#else
/* Non-ARM platforms: no NEON */
static bool cpu_has_neon(void)
{
    return false;
}
#endif /* __aarch64__ || __arm__ */

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
/* x86/x86_64: Check for SSE/SSSE3 support */

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif

static bool cpu_has_ssse3(void)
{
#ifdef _MSC_VER
    int cpu_info[4];
    __cpuid(cpu_info, 1);
    /* ECX bit 9: SSSE3 */
    return (cpu_info[2] & (1 << 9)) != 0;
#else
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        /* ECX bit 9: SSSE3 */
        return (ecx & (1 << 9)) != 0;
    }
    return false;
#endif
}

#else
/* Non-x86 platforms: no SSSE3 */
static bool cpu_has_ssse3(void)
{
    return false;
}
#endif /* __x86_64__ || _M_X64 || __i386__ || _M_IX86 */

/* Runtime selection of best implementation
 *
 * Thread-safe initialization using POSIX pthread_once().
 * Guarantees that CPU feature detection runs exactly once,
 * even with concurrent calls from multiple threads.
 */
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static base64_encode_func_t best_impl = NULL;

static void initialize_best_impl(void)
{
    /* Priority: NEON > SSE/SSSE3 > Scalar */
    if (cpu_has_neon()) {
#if defined(__aarch64__) || defined(__ARM_NEON)
        /* Use NEON SIMD implementation (simdutf algorithm) */
        best_impl = base64_encode_neon;
#else
        /* NEON not available, fallback to scalar */
        best_impl = base64_encode_scalar;
#endif
    } else if (cpu_has_ssse3()) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
        /* Use SSE/SSSE3 SIMD implementation */
        best_impl = base64_encode_sse;
#else
        /* SSE not available, fallback to scalar */
        best_impl = base64_encode_scalar;
#endif
    } else {
        /* Fallback to basic scalar */
        best_impl = base64_encode_scalar;
    }
}

static base64_encode_func_t select_best_impl(void)
{
    pthread_once(&init_once, initialize_best_impl);
    return best_impl;
}

/* Unified API: automatically selects best implementation */
size_t base64_encode_auto(const uint8_t *restrict input,
                          size_t input_len,
                          uint8_t *restrict output)
{
    base64_encode_func_t impl = select_best_impl();
    return impl(input, input_len, output);
}

/* Get the name of the active implementation (for debugging) */
const char *base64_get_impl_name(void)
{
    base64_encode_func_t impl = select_best_impl();

#if defined(__aarch64__) || defined(__ARM_NEON)
    if (impl == base64_encode_neon)
        return "NEON";
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
    if (impl == base64_encode_sse)
        return "SSE/SSSE3";
#endif
    if (impl == base64_encode_scalar)
        return "Scalar";
    return "Unknown";
}
