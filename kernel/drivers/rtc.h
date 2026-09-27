#ifndef FELINOS_RTC_H
#define FELINOS_RTC_H

#include <stdint.h>

struct rtc_time {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
};

void rtc_read(struct rtc_time *t);
int rtc_write(const struct rtc_time *t);
uint32_t rtc_unix(void);
void unix_to_time(uint32_t stamp, struct rtc_time *t);
int day_of_week(int year, int month, int day);
int days_in_month(int year, int month);
const char *month_name(int month);
const char *weekday_name(int wd);

#endif
