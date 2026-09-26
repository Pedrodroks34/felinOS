#ifndef FELINOS_DNS_H
#define FELINOS_DNS_H

#include <stdint.h>

int dns_resolve(uint32_t dns_server, const char *hostname, uint32_t timeout_ticks, uint32_t *out_ip);

#endif
