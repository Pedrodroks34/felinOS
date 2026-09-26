#ifndef FELINOS_E1000_H
#define FELINOS_E1000_H

#include <stdint.h>

int e1000_init(void);
int e1000_present(void);
const char *e1000_link_status(void);

#endif
