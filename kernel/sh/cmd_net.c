#include "sh/cmds.h"
#include "lib/string.h"
#include "lib/format.h"
#include "net/net.h"
#include "net/netif.h"
#include "net/icmp.h"
#include "net/dhcp.h"
#include "net/dns.h"
#include "drivers/pit.h"

int cmd_ifconfig(int argc, char **argv, struct stream *in, struct stream *out) {
    (void)in;
    struct netif *nif = netif_default();

    if (!nif) {
        st_puts(out, "No network interface present.\n");
        return 1;
    }

    if (argc == 3 && strcmp(argv[1], "dns") == 0) {
        uint32_t dns;
        if (ip4_from_str(argv[2], &dns) != 0) {
            st_puts(out, "ifconfig: invalid DNS server\n");
            return 1;
        }
        netif_set_dns(dns);
        st_puts(out, "DNS server set.\n");
        return 0;
    }
    if (argc >= 3) {
        uint32_t ip, mask, gw = 0, dns = 0;
        if (ip4_from_str(argv[1], &ip) != 0 || ip4_from_str(argv[2], &mask) != 0) {
            st_puts(out, "ifconfig: invalid address\n");
            return 1;
        }
        if (argc >= 4 && ip4_from_str(argv[3], &gw) != 0) {
            st_puts(out, "ifconfig: invalid gateway\n");
            return 1;
        }
        if (argc >= 5 && ip4_from_str(argv[4], &dns) != 0) {
            st_puts(out, "ifconfig: invalid DNS server\n");
            return 1;
        }
        netif_set_addr(ip, mask, gw);
        if (argc >= 5) {
            netif_set_dns(dns);
        }
        st_puts(out, "Interface configured.\n");
        return 0;
    }

    char mac[18], ip[16], mask[16], gwstr[16], dns[16];
    mac_to_str(nif->mac, mac);
    ip4_to_str(nif->ip, ip);
    ip4_to_str(nif->netmask, mask);
    ip4_to_str(nif->gateway, gwstr);
    ip4_to_str(nif->dns_server, dns);

    st_printf(out, "%s: driver %s  link %s\n", "net0", nif->driver_name,
              nif->link_status ? nif->link_status() : "unknown");
    st_printf(out, "    ether %s\n", mac);
    st_printf(out, "    inet %s  netmask %s  gateway %s\n", ip, mask, gwstr);
    st_printf(out, "    dns %s\n", dns);
    st_printf(out, "    rx packets %u bytes %u errors %u\n", nif->rx_packets, nif->rx_bytes, nif->rx_errors);
    st_printf(out, "    tx packets %u bytes %u errors %u\n", nif->tx_packets, nif->tx_bytes, nif->tx_errors);
    return 0;
}

static int resolve_target(struct stream *out, const char *host, uint32_t *ip) {
    if (ip4_from_str(host, ip) == 0) {
        return 0;
    }
    struct netif *nif = netif_default();
    if (!nif || nif->dns_server == 0) {
        st_printf(out, "ping: cannot resolve %s (no DNS server configured)\n", host);
        return -1;
    }
    if (dns_resolve(nif->dns_server, host, 300, ip) != 0) {
        st_printf(out, "ping: could not resolve %s\n", host);
        return -1;
    }
    return 0;
}

int cmd_ping(int argc, char **argv, struct stream *in, struct stream *out) {
    (void)in;
    if (argc < 2) {
        st_puts(out, "usage: ping <host> [count]\n");
        return 1;
    }
    uint32_t dst_ip;
    if (resolve_target(out, argv[1], &dst_ip) != 0) {
        return 1;
    }
    int count = argc >= 3 ? atoi(argv[2]) : 4;
    if (count < 1) {
        count = 1;
    }

    char ipstr[16];
    ip4_to_str(dst_ip, ipstr);
    st_printf(out, "PING %s (%s)\n", argv[1], ipstr);

    int replies = 0;
    uint16_t id = (uint16_t)pit_ticks();
    for (int i = 1; i <= count; i++) {
        struct ping_result res;
        icmp_ping(dst_ip, id, (uint16_t)i, 300, &res);
        if (res.replied) {
            replies++;
            st_printf(out, "reply from %s: seq=%d time=%ums\n", ipstr, i, res.rtt_ticks * 10);
        } else {
            st_printf(out, "request timeout: seq=%d\n", i);
        }
        sleep_ms(300);
    }
    st_printf(out, "%d packets transmitted, %d received, %d%% loss\n",
              count, replies, (int)(100 - (replies * 100 / count)));
    return replies > 0 ? 0 : 1;
}

int cmd_dhcp(int argc, char **argv, struct stream *in, struct stream *out) {
    (void)argc; (void)argv; (void)in;
    struct netif *nif = netif_default();

    if (!nif) {
        st_puts(out, "No network interface present.\n");
        return 1;
    }
    st_puts(out, "Requesting a lease via DHCP...\n");

    struct dhcp_lease lease;
    if (dhcp_client_run(500, &lease) != 0) {
        st_puts(out, "dhcp: no lease obtained\n");
        return 1;
    }
    netif_set_addr(lease.ip, lease.netmask, lease.gateway);
    netif_set_dns(lease.dns_server);

    char ip[16], mask[16], gw[16], dns[16];
    ip4_to_str(lease.ip, ip);
    ip4_to_str(lease.netmask, mask);
    ip4_to_str(lease.gateway, gw);
    ip4_to_str(lease.dns_server, dns);
    st_printf(out, "Lease obtained: inet %s netmask %s gateway %s dns %s\n", ip, mask, gw, dns);
    return 0;
}

int cmd_dns(int argc, char **argv, struct stream *in, struct stream *out) {
    (void)in;
    if (argc < 2) {
        st_puts(out, "usage: dns <hostname>\n");
        return 1;
    }
    struct netif *nif = netif_default();
    if (!nif || nif->dns_server == 0) {
        st_puts(out, "dns: no DNS server configured\n");
        return 1;
    }
    uint32_t ip;
    if (dns_resolve(nif->dns_server, argv[1], 300, &ip) != 0) {
        st_printf(out, "dns: could not resolve %s\n", argv[1]);
        return 1;
    }
    char ipstr[16];
    ip4_to_str(ip, ipstr);
    st_printf(out, "%s has address %s\n", argv[1], ipstr);
    return 0;
}
