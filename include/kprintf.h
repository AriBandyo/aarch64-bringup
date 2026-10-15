#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdarg.h>
#include <stddef.h>

typedef void (*console_putc_fn)(char c);

void console_set_output(console_putc_fn fn);
void console_enable_locking(void);      /* call once the MMU is on */
void console_putc(char c);
void console_puts(const char *s);

int  kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int  ksnprintf(char *buf, size_t size, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));

/* Serialized across cores; safe to call from any CPU. */
int  kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* kprintf with a "[    1.234567] " uptime prefix, like Linux printk. */
int  klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Lock-free variant for panic paths, where the console lock may be held. */
int  kprintf_unlocked(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
