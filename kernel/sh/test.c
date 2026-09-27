#include "lib/string.h"
#include "lib/format.h"
#include "console.h"
#include "drivers/pit.h"
#include "sched.h"
#include "pmm.h"
#include "paging.h"
#include "vmm.h"
#include "fs/vfs.h"
#include "lib/heap.h"
#include "drivers/rtc.h"
#include "sh/stream.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            kprintf("  FAIL: %s\n", msg); \
            return 1; \
        } else { \
            kprintf("  PASS: %s\n", msg); \
        } \
    } while (0)

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static void run_test(const char *name, int (*test_fn)(void)) {
    kprintf("Testing %s...\n", name);
    tests_run++;
    int result = test_fn();
    if (result == 0) {
        tests_passed++;
    } else {
        tests_failed++;
    }
}

int test_pmm(void) {
    TEST_ASSERT(pmm_total_frames() > 0, "PMM has frames");
    TEST_ASSERT(pmm_free_frames() > 0, "PMM has free frames");
    
    uint32_t frame = pmm_alloc_frame();
    TEST_ASSERT(frame != 0, "Can allocate frame");
    
    pmm_free_frame(frame);
    TEST_ASSERT(pmm_free_frames() > 0, "Can free frame");
    
    return 0;
}

int test_paging(void) {
    TEST_ASSERT(paging_mapped_mb() > 0, "Paging has mapped memory");
    TEST_ASSERT(paging_table_count() > 0, "Paging has page tables");
    
    void *ptr = vmm_map_physical(0x100000, 0x1000, VM_READ | VM_WRITE, "test-map");
    TEST_ASSERT(ptr != NULL, "Can map physical memory");
    
    vmm_free(ptr);
    return 0;
}

int test_vmm(void) {
    void *ptr = vmm_alloc(4096, VM_READ | VM_WRITE, "test-alloc");
    TEST_ASSERT(ptr != NULL, "Can allocate virtual memory");
    
    vmm_free(ptr);
    
    ptr = vmm_alloc_at(0x1000000, 4096, VM_READ | VM_WRITE | VM_FIXED, "test-fixed");
    TEST_ASSERT(ptr != NULL, "Can allocate at fixed address");
    
    vmm_free(ptr);
    return 0;
}

int test_vfs(void) {
    TEST_ASSERT(vfs_mount("ramfs", NULL, "/test") == 0, "Can mount ramfs");
    
    struct vfs_file *f;
    TEST_ASSERT(vfs_open("/test/testfile.txt", VFS_O_WRITE | VFS_O_CREATE, &f) == 0, "Can create file");
    
    const char *data = "Hello, VFS!";
    TEST_ASSERT(vfs_write(f, data, 12) == 12, "Can write to file");
    vfs_close(f);
    
    TEST_ASSERT(vfs_open("/test/testfile.txt", VFS_O_READ, &f) == 0, "Can open file for reading");
    
    char buf[32];
    uint32_t n = vfs_read(f, buf, 32);
    TEST_ASSERT(n == 12, "Can read from file");
    TEST_ASSERT(memcmp(buf, data, 12) == 0, "Read data matches");
    vfs_close(f);
    
    TEST_ASSERT(vfs_unlink("/test/testfile.txt") == 0, "Can unlink file");
    TEST_ASSERT(vfs_umount("/test") == 0, "Can unmount ramfs");
    
    return 0;
}

int test_heap(void) {
    void *p = kmalloc(128);
    TEST_ASSERT(p != NULL, "Can kmalloc");
    
    void *p2 = krealloc(p, 256);
    TEST_ASSERT(p2 != NULL, "Can krealloc");
    
    kfree(p2);
    return 0;
}

int test_string(void) {
    char buf[64];
    TEST_ASSERT(strlen("hello") == 5, "strlen works");
    TEST_ASSERT(strcmp("hello", "hello") == 0, "strcmp equal");
    TEST_ASSERT(strcmp("hello", "world") != 0, "strcmp not equal");
    
    strcpy(buf, "test");
    TEST_ASSERT(strcmp(buf, "test") == 0, "strcpy works");
    
    strcat(buf, "ing");
    TEST_ASSERT(strcmp(buf, "testing") == 0, "strcat works");
    
    TEST_ASSERT(memcmp("abc", "abc", 3) == 0, "memcmp equal");
    TEST_ASSERT(memcmp("abc", "abd", 3) < 0, "memcmp less");
    
    return 0;
}

int test_format(void) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%s %d %x %c", "test", 42, 0xFF, 'X');
    TEST_ASSERT(n > 0, "snprintf returns length");
    TEST_ASSERT(strcmp(buf, "test 42 ff X") == 0, "snprintf formats correctly");
    return 0;
}

int test_timer(void) {
    uint32_t before = pit_ticks();
    sleep_ms(10);
    uint32_t after = pit_ticks();
    TEST_ASSERT(after > before, "PIT timer advances");
    return 0;
}

int test_sched(void) {
    TEST_ASSERT(sched_active() == 1, "Scheduler is active");
    TEST_ASSERT(sched_current() != NULL, "Current task exists");
    TEST_ASSERT(sched_current()->pid > 0, "Current task has PID");
    return 0;
}

int test_rtc(void) {
    struct rtc_time t;
    rtc_read(&t);
    TEST_ASSERT(t.year >= 2020 && t.year <= 2099, "RTC year is reasonable");
    TEST_ASSERT(t.month >= 1 && t.month <= 12, "RTC month is valid");
    TEST_ASSERT(t.day >= 1 && t.day <= 31, "RTC day is valid");
    return 0;
}

void run_all_tests(void) {
    kprintf("\n=== FelinOS Automated Test Suite ===\n\n");
    
    run_test("PMM", test_pmm);
    run_test("Paging", test_paging);
    run_test("VMM", test_vmm);
    run_test("VFS", test_vfs);
    run_test("Heap", test_heap);
    run_test("String", test_string);
    run_test("Format", test_format);
    run_test("Timer", test_timer);
    run_test("Scheduler", test_sched);
    run_test("RTC", test_rtc);
    
    kprintf("\n=== Results ===\n");
    kprintf("Total: %d  Passed: %d  Failed: %d\n", tests_run, tests_passed, tests_failed);
    
    if (tests_failed == 0) {
        kprintf("\nAll tests PASSED!\n");
    } else {
        kprintf("\nSome tests FAILED!\n");
    }
}