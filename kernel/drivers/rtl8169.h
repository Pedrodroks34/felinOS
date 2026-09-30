#ifndef FELINOS_RTL8169_H
#define FELINOS_RTL8169_H

#include <stdint.h>

int rtl8169_init(void);
int rtl8169_present(void);
const char *rtl8169_link_status(void);

#endif
