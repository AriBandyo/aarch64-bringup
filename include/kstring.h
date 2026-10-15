#ifndef KSTRING_H
#define KSTRING_H

#include <stddef.h>
#include <stdint.h>

/* The compiler may emit calls to these four even in freestanding code. */
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);

/* Parses decimal or 0x-prefixed hex. Returns 0 on success, -1 on bad input. */
int    parse_u64(const char *s, uint64_t *out);

#endif
