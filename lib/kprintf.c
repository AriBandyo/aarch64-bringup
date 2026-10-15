/*
 * Freestanding printf for the console.
 *
 * Supports: %d %i %u %x %X %p %s %c %%, flags '-' '0', field width,
 * and length modifiers l, ll, z.
 */
#include <stdint.h>
#include "kprintf.h"
#include "spinlock.h"
#include "sysreg.h"
#include "timer.h"

static console_putc_fn out_fn;
static spinlock_t console_lock = SPINLOCK_INIT;
static int console_locking;     /* off until the MMU is on (see below) */

void console_set_output(console_putc_fn fn)
{
    out_fn = fn;
}

/*
 * Exclusive loads/stores (used by the spinlock) are only guaranteed to work
 * on Normal memory. Before the MMU is enabled every data access is treated
 * as Device memory, so the lock stays disabled until then. That is safe
 * because only the boot CPU runs before the MMU is on.
 */
void console_enable_locking(void)
{
    console_locking = 1;
}

void console_putc(char c)
{
    if (!out_fn)
        return;
    if (c == '\n')
        out_fn('\r');
    out_fn(c);
}

void console_puts(const char *s)
{
    while (*s)
        console_putc(*s++);
}

/* ---- formatting core ---------------------------------------------------- */

struct sink {
    char  *buf;
    size_t size;
    size_t len;
};

static void sink_put(struct sink *s, char c)
{
    if (s->len + 1 < s->size)
        s->buf[s->len] = c;
    s->len++;
}

static void put_number(struct sink *s, uint64_t v, unsigned base, int upper,
                       int negative, int width, int zero_pad, int left)
{
    char tmp[24];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);

    int total = n + (negative ? 1 : 0);
    int pad = width > total ? width - total : 0;

    if (!left && !zero_pad)
        while (pad-- > 0)
            sink_put(s, ' ');
    if (negative)
        sink_put(s, '-');
    if (!left && zero_pad)
        while (pad-- > 0)
            sink_put(s, '0');
    while (n)
        sink_put(s, tmp[--n]);
    if (left)
        while (pad-- > 0)
            sink_put(s, ' ');
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct sink s = { buf, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            sink_put(&s, *fmt);
            continue;
        }
        fmt++;

        int left = 0, zero_pad = 0, width = 0, lng = 0;
        for (;; fmt++) {
            if (*fmt == '-')
                left = 1;
            else if (*fmt == '0')
                zero_pad = 1;
            else
                break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'z') {
            lng++;
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            put_number(&s, mag, 10, 0, v < 0, width, zero_pad, left);
            break;
        }
        case 'u':
            put_number(&s, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned),
                       10, 0, 0, width, zero_pad, left);
            break;
        case 'x':
        case 'X':
            put_number(&s, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned),
                       16, *fmt == 'X', 0, width, zero_pad, left);
            break;
        case 'p':
            sink_put(&s, '0');
            sink_put(&s, 'x');
            put_number(&s, (uint64_t)(uintptr_t)va_arg(ap, void *), 16, 0, 0,
                       16, 1, 0);
            break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str)
                str = "(null)";
            int n = 0;
            while (str[n])
                n++;
            int pad = width > n ? width - n : 0;
            if (!left)
                while (pad-- > 0)
                    sink_put(&s, ' ');
            while (*str)
                sink_put(&s, *str++);
            if (left)
                while (pad-- > 0)
                    sink_put(&s, ' ');
            break;
        }
        case 'c':
            sink_put(&s, (char)va_arg(ap, int));
            break;
        case '%':
            sink_put(&s, '%');
            break;
        case '\0':
            fmt--;          /* trailing '%': stop cleanly */
            break;
        default:
            sink_put(&s, '%');
            sink_put(&s, *fmt);
            break;
        }
    }
    if (s.size)
        s.buf[s.len < s.size ? s.len : s.size - 1] = '\0';
    return (int)s.len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- console output ----------------------------------------------------- */

static int vprint(const char *prefix, const char *fmt, va_list ap, int lock)
{
    char line[256];
    int n = kvsnprintf(line, sizeof(line), fmt, ap);
    uint64_t flags = 0;

    lock = lock && console_locking;
    if (lock) {
        flags = irq_save();
        spin_lock(&console_lock);
    }
    if (prefix)
        console_puts(prefix);
    console_puts(line);
    if (lock) {
        spin_unlock(&console_lock);
        irq_restore(flags);
    }
    return n;
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vprint(NULL, fmt, ap, 1);
    va_end(ap);
    return n;
}

int kprintf_unlocked(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vprint(NULL, fmt, ap, 0);
    va_end(ap);
    return n;
}

int klog(const char *fmt, ...)
{
    char prefix[24];
    uint64_t us = uptime_us();
    ksnprintf(prefix, sizeof(prefix), "[%5lu.%06lu] ",
              us / 1000000, us % 1000000);

    va_list ap;
    va_start(ap, fmt);
    int n = vprint(prefix, fmt, ap, 1);
    va_end(ap);
    return n;
}
