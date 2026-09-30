#ifndef FELINOS_RTL8139_H
#define FELINOS_RTL8139_H

#include <stdint.h>

int rtl8139_init(void);
int rtl8139_present(void);
const char *rtl8139_link_status(void);

#endif
