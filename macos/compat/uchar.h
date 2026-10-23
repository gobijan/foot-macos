#pragma once

/*
 * Apple's libc does not ship C11 <uchar.h>. On macOS, wchar_t is
 * 32 bits wide and holds plain Unicode code points in UTF-8 locales,
 * so the char32_t conversions map directly onto the wchar_t ones.
 */

#include <stdint.h>
#include <wchar.h>

#if !defined(__cplusplus) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
typedef uint_least16_t char16_t;
typedef uint_least32_t char32_t;
#endif

_Static_assert(sizeof(wchar_t) == sizeof(char32_t), "wchar_t is not 32 bits");

static inline size_t
mbrtoc32(char32_t *restrict pc32, const char *restrict s, size_t n,
         mbstate_t *restrict ps)
{
    return mbrtowc((wchar_t *)pc32, s, n, ps);
}

static inline size_t
c32rtomb(char *restrict s, char32_t c32, mbstate_t *restrict ps)
{
    return wcrtomb(s, (wchar_t)c32, ps);
}
