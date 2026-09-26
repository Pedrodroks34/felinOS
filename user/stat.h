#ifndef USER_STAT_H
#define USER_STAT_H
#include <stdint.h>

#define S_FILE 1
#define S_DIR  2
#define S_CHR  3
#define S_BLK  4

struct stat {
    uint8_t type;
    uint16_t mode;
    uint64_t size;
    uint32_t mtime;
    uint32_t ino;
};

struct dirent {
    char name[64];
    uint8_t type;
};

#endif
