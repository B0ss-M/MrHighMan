/* Omni Sampler: keep the .so loadable on glibc < 2.38 (the catalog's limit is 2.36).
 * g++ always defines _GNU_SOURCE, and with it glibc 2.38+ headers bind strtol/strtoul/sscanf (and atoi, inline) to
 * __isoc23_*@GLIBC_2.38 (docs/NOTES.md, "glibc 2.38+ toolchains"). This file is plain C without _GNU_SOURCE, so its
 * strtol & co. are the classic symbols; defining the __isoc23_ names here (hidden, inside the .so) resolves the C++
 * code's calls locally. Only difference: C23 also accepts a "0b" binary prefix, which nothing here relies on. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define HIDDEN __attribute__((visibility("hidden")))

HIDDEN long __isoc23_strtol(const char *s, char **end, int base) { return strtol(s, end, base); }
HIDDEN unsigned long __isoc23_strtoul(const char *s, char **end, int base) { return strtoul(s, end, base); }
HIDDEN long long __isoc23_strtoll(const char *s, char **end, int base) { return strtoll(s, end, base); }
HIDDEN unsigned long long __isoc23_strtoull(const char *s, char **end, int base) { return strtoull(s, end, base); }
HIDDEN int __isoc23_sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
