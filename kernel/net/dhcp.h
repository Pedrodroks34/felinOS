#ifndef FELINOS_DHCP_H
#define FELINOS_DHCP_H

#include <stdint.h>

struct dhcp_lease {
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns_server;
    uint32_t server_id;
    uint32_t lease_seconds;
};

int dhcp_client_run(uint32_t timeout_ticks, struct dhcp_lease *out);

#endif
