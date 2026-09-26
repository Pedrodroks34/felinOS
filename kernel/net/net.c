#include "net/net.h"
#include "lib/string.h"
#include "lib/format.h"

const mac_addr_t mac_broadcast = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
const mac_addr_t mac_zero = { 0, 0, 0, 0, 0, 0 };

int mac_is_zero(const uint8_t *m) {
    return memcmp(m, mac_zero, MAC_LEN) == 0;
}

int mac_is_broadcast(const uint8_t *m) {
    return memcmp(m, mac_broadcast, MAC_LEN) == 0;
}

int mac_equal(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, MAC_LEN) == 0;
}

void mac_copy(uint8_t *dst, const uint8_t *src) {
    memcpy(dst, src, MAC_LEN);
}

void mac_to_str(const uint8_t *m, char *out) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

void ip4_to_str(uint32_t ip_host, char *out) {
    snprintf(out, 16, "%u.%u.%u.%u",
             (ip_host >> 24) & 0xFF, (ip_host >> 16) & 0xFF,
             (ip_host >> 8) & 0xFF, ip_host & 0xFF);
}

int ip4_from_str(const char *s, uint32_t *out_host) {
    uint32_t parts[4] = { 0, 0, 0, 0 };
    int part = 0;
    int have_digit = 0;

    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            parts[part] = parts[part] * 10 + (uint32_t)(*p - '0');
            have_digit = 1;
            if (parts[part] > 255) {
                return -1;
            }
        } else if (*p == '.') {
            if (!have_digit || part >= 3) {
                return -1;
            }
            part++;
            have_digit = 0;
        } else if (*p == '\0') {
            break;
        } else {
            return -1;
        }
    }
    if (part != 3 || !have_digit) {
        return -1;
    }
    *out_host = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 0;
}

static uint32_t checksum_add(const void *data, uint32_t len, uint32_t sum) {
    const uint8_t *p = (const uint8_t *)data;

    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += (uint32_t)(p[0] << 8);
    }
    return sum;
}

static uint16_t checksum_fold(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

uint16_t net_checksum(const void *data, uint32_t len, uint32_t seed) {
    return checksum_fold(checksum_add(data, len, seed));
}

uint16_t net_checksum2(const void *a, uint32_t alen, const void *b, uint32_t blen, uint32_t seed) {
    uint32_t sum = checksum_add(a, alen, seed);
    sum = checksum_add(b, blen, sum);
    return checksum_fold(sum);
}
