#include "drivers/input.h"
#include "drivers/serial.h"
#include "io.h"
#include "sched.h"

static int serial_wait_byte(void) {
    for (uint32_t i = 0; i < 200000; i++) {
        int c = serial_read_nonblock();
        if (c >= 0) {
            return c;
        }
        io_wait();
    }
    return -1;
}

static int parse_serial_escape(void) {
    int c = serial_wait_byte();
    if (c < 0) {
        return 27;
    }
    if (c != '[' && c != 'O') {
        return c;
    }

    int d = serial_wait_byte();
    if (d < 0) {
        return 27;
    }

    switch (d) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case 'P': return KEY_F1;
        case 'Q': return KEY_F2;
        case 'R': return KEY_F3;
        case 'S': return KEY_F4;
        default: break;
    }

    if (d >= '1' && d <= '8') {
        int e = serial_wait_byte();
        if (e == '~') {
            switch (d) {
                case '1': return KEY_HOME;
                case '2': return KEY_INSERT;
                case '3': return KEY_DELETE;
                case '4': return KEY_END;
                case '5': return KEY_PGUP;
                case '6': return KEY_PGDN;
                default: break;
            }
        }
    }
    return 27;
}

static int serial_translate(int c) {
    if (c == 27) {
        return parse_serial_escape();
    }
    if (c == 127) {
        return '\b';
    }
    if (c == '\r') {
        return '\n';
    }
    return c;
}

int input_poll(void) {
    int key = keyboard_poll();
    if (key >= 0) {
        return key;
    }
    int sc = serial_read_nonblock();
    if (sc >= 0) {
        return serial_translate(sc);
    }
    return -1;
}

int input_getkey(void) {
    for (;;) {
        int key = input_poll();
        if (key >= 0) {
            return key;
        }
        if (!sched_can_sleep()) {
            hlt();
            continue;
        }
        uint32_t f = irq_save();
        if (!keyboard_pending() && !serial_pending()) {
            sched_wait_on_timeout(sched_input_channel(), "input", 2);
        }
        irq_restore(f);
    }
}

void input_init(void) {
    keyboard_set_break_hook(sched_break);
    serial_set_break_hook(sched_break);
    serial_irq_init();
}

void input_flush(void) {
    while (input_poll() >= 0) {
    }
}
