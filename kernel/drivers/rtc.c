#include "drivers/rtc.h"
#include "io.h"
#include "sync.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static const char *months[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const char *weekdays[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

static spinlock_t cmos_lock = SPINLOCK_INIT("cmos");

static uint8_t cmos_read(uint8_t reg) {
    uint32_t f = spin_lock_irqsave(&cmos_lock);
    outb(CMOS_ADDR, reg);
    io_wait();
    uint8_t v = inb(CMOS_DATA);
    spin_unlock_irqrestore(&cmos_lock, f);
    return v;
}

static int update_in_progress(void) {
    return cmos_read(0x0A) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

void rtc_read(struct rtc_time *t) {
    uint32_t guard = 1000000;
    while (update_in_progress() && guard--) {
    }

    uint8_t second = cmos_read(0x00);
    uint8_t minute = cmos_read(0x02);
    uint8_t hour   = cmos_read(0x04);
    uint8_t day    = cmos_read(0x07);
    uint8_t month  = cmos_read(0x08);
    uint8_t year   = cmos_read(0x09);
    uint8_t status = cmos_read(0x0B);

    if (!(status & 0x04)) {
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour = (uint8_t)(((hour & 0x0F) + (((hour & 0x70) / 16) * 10)) | (hour & 0x80));
        day = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year = bcd_to_bin(year);
    }

    if (!(status & 0x02) && (hour & 0x80)) {
        hour = (uint8_t)(((hour & 0x7F) + 12) % 24);
    }

    t->second = second;
    t->minute = minute;
    t->hour = hour;
    t->day = day;
    t->month = month;
    t->year = (uint16_t)(2000 + year);
}

int days_in_month(int year, int month) {
    static const int table[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) {
        return 30;
    }
    if (month == 2) {
        int leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return table[month - 1];
}

static uint32_t days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + (int)doe - 719468);
}

static void civil_from_days(uint32_t days, int *y, int *m, int *d) {
    int z = (int)days + 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy = (int)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp + (mp < 10 ? 3 : -9);
    *y = yy + (mm <= 2);
    *m = (int)mm;
    *d = (int)dd;
}

uint32_t rtc_unix(void) {
    struct rtc_time t;
    rtc_read(&t);
    uint32_t days = days_from_civil(t.year, t.month, t.day);
    return days * 86400u + t.hour * 3600u + t.minute * 60u + t.second;
}

void unix_to_time(uint32_t stamp, struct rtc_time *t) {
    uint32_t days = stamp / 86400u;
    uint32_t rem = stamp % 86400u;
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    t->year = (uint16_t)y;
    t->month = (uint8_t)m;
    t->day = (uint8_t)d;
    t->hour = (uint8_t)(rem / 3600u);
    t->minute = (uint8_t)((rem % 3600u) / 60u);
    t->second = (uint8_t)(rem % 60u);
}

int day_of_week(int year, int month, int day) {
    uint32_t days = days_from_civil(year, month, day);
    return (int)((days + 4) % 7);
}

const char *month_name(int month) {
    if (month < 1 || month > 12) {
        return "?";
    }
    return months[month - 1];
}

const char *weekday_name(int wd) {
    if (wd < 0 || wd > 6) {
        return "?";
    }
    return weekdays[wd];
}
