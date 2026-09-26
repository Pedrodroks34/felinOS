#include "lib/string.h"
#include "lib/heap.h"

void *memset(void *dst, int c, size_t n) {
    uint8_t *p = (uint8_t *)dst;
    while (n--) {
        *p++ = (uint8_t)c;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    while (n--) {
        if (*x != *y) {
            return (int)*x - (int)*y;
        }
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower(*a) == tolower(*b)) {
        a++;
        b++;
    }
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {
    }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

size_t strlcpy(char *dst, const char *src, size_t n) {
    size_t len = strlen(src);
    if (n) {
        size_t copy = (len >= n) ? n - 1 : len;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    while ((*d++ = *src++)) {
    }
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) {
            last = s;
        }
        s++;
    }
    return (char *)last;
}

char *strstr(const char *hay, const char *needle) {
    if (!*needle) {
        return (char *)hay;
    }
    size_t nl = strlen(needle);
    while (*hay) {
        if (strncmp(hay, needle, nl) == 0) {
            return (char *)hay;
        }
        hay++;
    }
    return NULL;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)kmalloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

int isdigit(int c) {
    return c >= '0' && c <= '9';
}

int isalpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

int isprint(int c) {
    return c >= 32 && c < 127;
}

int toupper(int c) {
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

int tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

long strtol(const char *s, char **end, int base) {
    long result = 0;
    int neg = 0;

    while (isspace(*s)) {
        s++;
    }
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if ((base == 16 || base == 0) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    }
    if (base == 0) {
        base = (*s == '0') ? 8 : 10;
    }

    while (*s) {
        int digit;
        if (isdigit(*s)) {
            digit = *s - '0';
        } else if (isalpha(*s)) {
            digit = tolower(*s) - 'a' + 10;
        } else {
            break;
        }
        if (digit >= base) {
            break;
        }
        result = result * base + digit;
        s++;
    }

    if (end) {
        *end = (char *)s;
    }
    return neg ? -result : result;
}

int atoi(const char *s) {
    return (int)strtol(s, NULL, 10);
}

uint32_t strtou32(const char *s, int base) {
    return (uint32_t)strtol(s, NULL, base);
}

void utoa(uint32_t value, char *buf, int base) {
    static const char digits[] = "0123456789abcdef";
    char tmp[36];
    int i = 0;

    if (base < 2 || base > 16) {
        buf[0] = '\0';
        return;
    }
    if (value == 0) {
        tmp[i++] = '0';
    }
    while (value) {
        tmp[i++] = digits[value % (uint32_t)base];
        value /= (uint32_t)base;
    }
    int j = 0;
    while (i--) {
        buf[j++] = tmp[i];
    }
    buf[j] = '\0';
}

void itoa(int32_t value, char *buf, int base) {
    if (value < 0 && base == 10) {
        *buf++ = '-';
        utoa((uint32_t)(-value), buf, base);
        return;
    }
    utoa((uint32_t)value, buf, base);
}
