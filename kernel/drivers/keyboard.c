#include <stdint.h>
#include "drivers/keyboard.h"
#include "idt.h"
#include "io.h"
#include "sched.h"

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64
#define BUFFER_SIZE     256

static const char layout_normal[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '-', '=', '\b','\t','q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'','`', 0,  '\\','z', 'x', 'c', 'v', 'b', 'n',
    'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,   0,   0
};

static const char layout_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*',
    '(', ')', '_', '+', '\b','\t','Q', 'W', 'E', 'R',
    'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~', 0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N',
    'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,   0,   0
};

static int buffer[BUFFER_SIZE];
static volatile uint32_t buf_head;
static volatile uint32_t buf_tail;

static int shift_down;
static int ctrl_down;
static int alt_down;
static int caps_on;
static int extended;

static int (*break_hook)(void);

void keyboard_set_break_hook(int (*hook)(void)) {
    break_hook = hook;
}

int keyboard_pending(void) {
    return buf_head != buf_tail;
}

static void push_key(int key) {
    if (key == 3 && break_hook && break_hook()) {
        return;
    }
    uint32_t next = (buf_head + 1) % BUFFER_SIZE;
    if (next != buf_tail) {
        buffer[buf_head] = key;
        buf_head = next;
    }
    sched_wake_input();
}

static void handle_extended(uint8_t code) {
    int released = code & 0x80;
    uint8_t key = code & 0x7F;

    if (key == 0x1D) {
        ctrl_down = !released;
        return;
    }
    if (key == 0x38) {
        alt_down = !released;
        return;
    }
    if (released) {
        return;
    }

    switch (key) {
        case 0x48: push_key(KEY_UP); break;
        case 0x50: push_key(KEY_DOWN); break;
        case 0x4B: push_key(KEY_LEFT); break;
        case 0x4D: push_key(KEY_RIGHT); break;
        case 0x47: push_key(KEY_HOME); break;
        case 0x4F: push_key(KEY_END); break;
        case 0x49: push_key(KEY_PGUP); break;
        case 0x51: push_key(KEY_PGDN); break;
        case 0x52: push_key(KEY_INSERT); break;
        case 0x53: push_key(KEY_DELETE); break;
        case 0x1C: push_key('\n'); break;
        case 0x35: push_key('/'); break;
        default: break;
    }
}

static void keyboard_callback(struct regs *r) {
    uint8_t code = inb(KBD_DATA_PORT);

    if (code == 0xE0) {
        extended = 1;
        return;
    }
    if (extended) {
        extended = 0;
        handle_extended(code);
        return;
    }

    if (code & 0x80) {
        uint8_t key = code & 0x7F;
        if (key == 0x2A || key == 0x36) {
            shift_down = 0;
        } else if (key == 0x1D) {
            ctrl_down = 0;
        } else if (key == 0x38) {
            alt_down = 0;
        }
        return;
    }

    switch (code) {
        case 0x2A:
        case 0x36:
            shift_down = 1;
            return;
        case 0x1D:
            ctrl_down = 1;
            return;
        case 0x38:
            alt_down = 1;
            return;
        case 0x3A:
            caps_on = !caps_on;
            return;
        default:
            break;
    }

    if (code >= 0x3B && code <= 0x44) {
        push_key(KEY_F1 + (code - 0x3B));
        return;
    }

    if (code >= 128) {
        return;
    }

    char c = shift_down ? layout_shift[code] : layout_normal[code];
    if (!c) {
        return;
    }

    if (caps_on && c >= 'a' && c <= 'z') {
        c = (char)(c - 32);
    } else if (caps_on && shift_down && c >= 'A' && c <= 'Z') {
        c = (char)(c + 32);
    }

    if (ctrl_down) {
        if (c >= 'a' && c <= 'z') {
            push_key(c - 'a' + 1);
            return;
        }
        if (c >= 'A' && c <= 'Z') {
            push_key(c - 'A' + 1);
            return;
        }
    }

    push_key((int)(unsigned char)c);
}

void keyboard_init(void) {
    buf_head = 0;
    buf_tail = 0;
    shift_down = 0;
    ctrl_down = 0;
    alt_down = 0;
    caps_on = 0;
    extended = 0;
    irq_install_handler(1, keyboard_callback);
}

int keyboard_poll(void) {
    if (buf_head == buf_tail) {
        return -1;
    }
    int key = buffer[buf_tail];
    buf_tail = (buf_tail + 1) % BUFFER_SIZE;
    return key;
}

int keyboard_shift_down(void) {
    return shift_down;
}

int keyboard_ctrl_down(void) {
    return ctrl_down;
}

int keyboard_alt_down(void) {
    return alt_down;
}

int keyboard_caps_on(void) {
    return caps_on;
}
