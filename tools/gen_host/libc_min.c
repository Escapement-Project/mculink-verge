/* Just enough C library for the QEMU harness, including the AEABI entry points clang calls. Built
 * with builtins off, so the loops are not turned back into calls to themselves. */
#include <stddef.h>
#include <stdint.h>
void* memmove(void* d, const void* s, size_t n) {
    unsigned char* dp = d;
    const unsigned char* sp = s;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}
void* memcpy(void* d, const void* s, size_t n) { return memmove(d, s, n); }
void* memset(void* d, int c, size_t n) { unsigned char* p = d; while (n--) *p++ = (unsigned char)c; return d; }
int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
void __aeabi_memcpy(void* d, const void* s, size_t n) { memmove(d, s, n); }
void __aeabi_memcpy4(void* d, const void* s, size_t n) { memmove(d, s, n); }
void __aeabi_memcpy8(void* d, const void* s, size_t n) { memmove(d, s, n); }
void __aeabi_memmove(void* d, const void* s, size_t n) { memmove(d, s, n); }
void __aeabi_memmove4(void* d, const void* s, size_t n) { memmove(d, s, n); }
void __aeabi_memmove8(void* d, const void* s, size_t n) { memmove(d, s, n); }
void __aeabi_memset(void* d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memclr(void* d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void* d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void* d, size_t n) { memset(d, 0, n); }
