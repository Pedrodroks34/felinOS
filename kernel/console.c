#include "console.h"
#include "sync.h"
#include "drivers/vga.h"
#include "drivers/serial.h"
#include "lib/format.h"
#include "lib/string.h"

#define KLOG_SIZE 8192
#define CHUNK 64

static char log_buf[KLOG_SIZE];
static size_t log_pos;
static volatile uint32_t output_epoch;
static spinlock_t console_lock = SPINLOCK_INIT("console");
static spinlock_t klog_lock = SPINLOCK_INIT("klog");
static volatile int console_panic;

/* One writer at a time keeps lines from different tasks together. Interrupt
   handlers and the panic path never wait for it: garbled output beats a
   deadlock when the code they interrupted was the one printing. */
static int console_grab(void) {
    if (console_panic || in_irq()) {
        return 0;
    }
    spin_lock(&console_lock);
    return 1;
}

static void console_drop(int held) {
    if (held) {
        spin_unlock(&console_lock);
    }
}

void console_set_panic(void) {
    console_panic = 1;
}

void console_init(void) {
    vga_initialize();
    serial_init();
    log_pos = 0;
    log_buf[0] = '\0';
}

static void put_raw(char c) {
    output_epoch++;
    vga_putchar(c);
    if (c == '\n') {
        serial_putchar('\r');
    }
    serial_putchar(c);
}

void console_putchar(char c) {
    int held = console_grab();

    put_raw(c);
    console_drop(held);
}

/* The bytes are copied to the stack before the lock is taken: the source may
   live in memory that can fault (kernel heap, user space), which must never
   happen while a spinlock is held. */
void console_write_n(const char *s, uint32_t len) {
    char tmp[CHUNK];

    while (len) {
        uint32_t n = len < CHUNK ? len : CHUNK;

        memcpy(tmp, s, n);
        int held = console_grab();
        for (uint32_t i = 0; i < n; i++) {
            put_raw(tmp[i]);
        }
        console_drop(held);
        s += n;
        len -= n;
    }
}

void console_write(const char *s) {
    char tmp[CHUNK];

    for (;;) {
        uint32_t n = 0;

        while (n < CHUNK && s[n]) {
            tmp[n] = s[n];
            n++;
        }
        if (!n) {
            return;
        }
        int held = console_grab();
        for (uint32_t i = 0; i < n; i++) {
            put_raw(tmp[i]);
        }
        console_drop(held);
        s += n;
    }
}

void console_clear(void) {
    int held = console_grab();

    vga_clear();
    serial_write("\x1b[2J\x1b[H");
    console_drop(held);
}

struct kp {
    char buf[128];
    uint32_t n;
};

static void kp_flush(struct kp *k) {
    int held = console_grab();

    for (uint32_t i = 0; i < k->n; i++) {
        put_raw(k->buf[i]);
    }
    console_drop(held);
    k->n = 0;
}

static void emit_console(void *ctx, char c) {
    struct kp *k = (struct kp *)ctx;

    k->buf[k->n++] = c;
    if (k->n == sizeof(k->buf)) {
        kp_flush(k);
    }
}

static void emit_log(void *ctx, char c) {
    if (log_pos + 1 < KLOG_SIZE) {
        log_buf[log_pos++] = c;
        log_buf[log_pos] = '\0';
    }
}

void kprintf(const char *fmt, ...) {
    struct kp k;
    va_list args;

    k.n = 0;
    va_start(args, fmt);
    fmt_vprint(emit_console, &k, fmt, args);
    va_end(args);
    kp_flush(&k);
}

void klog(const char *fmt, ...) {
    va_list args;

    /* format first: arguments may point into memory that can fault */
    char tmp[192];
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    uint32_t f = spin_lock_irqsave(&klog_lock);
    for (const char *p = tmp; *p; p++) {
        emit_log(NULL, *p);
    }
    emit_log(NULL, '\n');
    spin_unlock_irqrestore(&klog_lock, f);
}

const char *klog_buffer(void) {
    return log_buf;
}

void klog_clear(void) {
    uint32_t f = spin_lock_irqsave(&klog_lock);

    log_pos = 0;
    log_buf[0] = '\0';
    spin_unlock_irqrestore(&klog_lock, f);
}

uint32_t console_epoch(void) {
    return output_epoch;
}
