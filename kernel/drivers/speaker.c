#include "drivers/speaker.h"
#include "drivers/pit.h"
#include "io.h"
#include "sync.h"

static spinlock_t speaker_lock = SPINLOCK_INIT("speaker");

static void speaker_on(uint32_t frequency) {
    if (frequency == 0) {
        return;
    }
    uint32_t divisor = 1193182 / frequency;

    uint32_t f = spin_lock_irqsave(&speaker_lock);
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(divisor & 0xFF));
    outb(0x42, (uint8_t)((divisor >> 8) & 0xFF));

    uint8_t tmp = inb(0x61);
    if ((tmp & 3) != 3) {
        outb(0x61, tmp | 3);
    }
    spin_unlock_irqrestore(&speaker_lock, f);
}

static void speaker_off(void) {
    uint32_t f = spin_lock_irqsave(&speaker_lock);
    outb(0x61, (uint8_t)(inb(0x61) & 0xFC));
    spin_unlock_irqrestore(&speaker_lock, f);
}

void speaker_beep(uint32_t frequency, uint32_t duration_ms) {
    speaker_on(frequency);
    sleep_ms(duration_ms);
    speaker_off();
}
