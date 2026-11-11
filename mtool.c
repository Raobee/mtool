/*
 * mtool - small MMIO / PCI BAR peek & poke utility
 *
 * This is a clean-room, portable re-implementation of the commonly used
 * "mtool" command shown on the user's test bench:
 *
 *     mtool r.l 0x404040000 100
 *     mtool w.l 0x404040088 0xb600
 *
 * It only uses POSIX/Linux interfaces (open/mmap/read), contains no
 * architecture specific assembly, and works on 32-bit and 64-bit
 * little-endian Linux (x86_64, aarch64, armv7, ...).
 *
 * Two back ends are supported:
 *   1. /dev/mem          - absolute physical address (the classic way)
 *   2. sysfs resourceN   - PCI BAR, no physical address needed
 *
 * Build:
 *     make
 *     make CC=aarch64-linux-gnu-gcc
 *     make CC=arm-linux-gnueabihf-gcc
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MTOOL_VERSION "1.0.1"
#define SYS_PCI_DEVICES "/sys/bus/pci/devices"
#define MAX_BARS 6
#define MAX_WRITE_VALUES 256

/* ------------------------------------------------------------------ */
/* Endian helpers: PCIe / MMIO registers are little-endian.            */
/* ------------------------------------------------------------------ */

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define MTOOL_HOST_BIG 1
#else
#define MTOOL_HOST_BIG 0
#endif

static inline uint16_t mtool_bswap16(uint16_t v)
{
#if defined(__GNUC__)
    return __builtin_bswap16(v);
#else
    return (uint16_t)((v >> 8) | (v << 8));
#endif
}

static inline uint32_t mtool_bswap32(uint32_t v)
{
#if defined(__GNUC__)
    return __builtin_bswap32(v);
#else
    return ((v & 0x000000ffu) << 24) |
           ((v & 0x0000ff00u) << 8) |
           ((v & 0x00ff0000u) >> 8) |
           ((v & 0xff000000u) >> 24);
#endif
}

static inline uint64_t mtool_bswap64(uint64_t v)
{
#if defined(__GNUC__)
    return __builtin_bswap64(v);
#else
    return ((uint64_t)mtool_bswap32((uint32_t)v) << 32) |
           mtool_bswap32((uint32_t)(v >> 32));
#endif
}

static inline uint16_t from_le16(uint16_t v)
{
    return MTOOL_HOST_BIG ? mtool_bswap16(v) : v;
}

static inline uint32_t from_le32(uint32_t v)
{
    return MTOOL_HOST_BIG ? mtool_bswap32(v) : v;
}

static inline uint64_t from_le64(uint64_t v)
{
    return MTOOL_HOST_BIG ? mtool_bswap64(v) : v;
}

static inline uint16_t to_le16(uint16_t v)
{
    return MTOOL_HOST_BIG ? mtool_bswap16(v) : v;
}

static inline uint32_t to_le32(uint32_t v)
{
    return MTOOL_HOST_BIG ? mtool_bswap32(v) : v;
}

static inline uint64_t to_le64(uint64_t v)
{
    return MTOOL_HOST_BIG ? mtool_bswap64(v) : v;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

enum op {
    OP_READ,
    OP_WRITE
};

struct bar_info {
    int index;
    int assigned;
    uint64_t start;
    uint64_t end;
    uint64_t size;
    uint64_t flags;
    char path[PATH_MAX];
};

struct options {
    enum op op;
    int width;

    char file[PATH_MAX];
    int use_bar;
    char bar_bdf[64];
    int bar_index;

    char pci_bdf[64];
    int pci_enable;

    int auto_enable;
    int no_enable;
    int read_only;
    int no_ascii;
    int qword_atomic;
    int verbose;
    int quiet;
};

struct target {
    int fd;
    uint64_t base;      /* physical/display base (BAR start, else 0) */
    uint64_t limit;     /* size limit (valid when has_limit is set)    */
    int has_limit;
    int is_mmio;
    int has_bar;
    struct bar_info bar;
};

static const char *progname = "mtool";

/*
 * By default a 64-bit MMIO access is split into two 32-bit accesses.
 * Many AXI-Lite/32-bit BARs do not answer a true 64-bit access (they
 * return 0xffffffff).  --qword-atomic selects a single 64-bit access.
 */
static int g_qword_atomic = 0;

/* Original mtool style: bare numbers are hexadecimal.  --decimal opts out. */
static int g_hex_default = 1;

static void msg(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void die(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void on_sigbus(int sig)
{
    static const char text[] =
        "mtool: SIGBUS while accessing MMIO (bad address / unsupported access)\n";
    ssize_t ignored;

    (void)sig;
    ignored = write(STDERR_FILENO, text, sizeof(text) - 1);
    (void)ignored;
    _exit(1);
}

static int number_base(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return 16;
    return g_hex_default ? 16 : 10;
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end;
    unsigned long long v;

    if (s == NULL || *s == '\0' || *s == '-')
        return -1;
    errno = 0;
    v = strtoull(s, &end, number_base(s));
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    *out = (uint64_t)v;
    return 0;
}

static int parse_int(const char *s, int *out)
{
    char *end;
    long v;

    if (s == NULL || *s == '\0')
        return -1;
    errno = 0;
    v = strtol(s, &end, number_base(s));
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > INT_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

static int read_text(const char *path, char *buf, size_t bufsize)
{
    int fd;
    ssize_t n;
    size_t len;

    if (bufsize == 0)
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = read(fd, buf, bufsize - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    len = (size_t)n;
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    return 0;
}

static int read_hex_u32(const char *path, uint32_t *out)
{
    char buf[64];
    char *end;
    unsigned long v;

    if (read_text(path, buf, sizeof(buf)) != 0)
        return -1;
    errno = 0;
    v = strtoul(buf, &end, 0);
    if (errno != 0 || end == buf || *end != '\0' || v > UINT32_MAX)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static int normalize_bdf(const char *in, char *out, size_t outsize)
{
    char tmp[64];
    unsigned int domain, bus, dev, fn;
    int n = 0;
    int colons = 0;
    const char *p;

    if (in == NULL || *in == '\0')
        return -1;
    for (p = in; *p != '\0'; p++) {
        if (*p == ':')
            colons++;
    }
    if (colons == 1)
        snprintf(tmp, sizeof(tmp), "0000:%s", in);
    else if (colons == 2)
        snprintf(tmp, sizeof(tmp), "%s", in);
    else
        return -1;

    if (sscanf(tmp, "%x:%x:%x.%x%n", &domain, &bus, &dev, &fn, &n) != 4)
        return -1;
    if (tmp[n] != '\0')
        return -1;
    if (domain > 0xffffu || bus > 0xffu || dev > 0x1fu || fn > 7u)
        return -1;

    snprintf(out, outsize, "%04x:%02x:%02x.%x", domain, bus, dev, fn);
    return 0;
}

static int dev_path(const char *bdf, const char *suffix, char *out, size_t outsize)
{
    int n = snprintf(out, outsize, "%s/%s/%s", SYS_PCI_DEVICES, bdf, suffix);

    return (n < 0 || (size_t)n >= outsize) ? -1 : 0;
}

/*
 * Read the real PCI COMMAND register (offset 4 in config space).  The
 * /sys/.../enable attribute is only a driver refcount and can be stale if
 * somebody used setpci or wrote the config space directly.
 */
static int get_command(const char *bdf, uint16_t *command)
{
    char path[PATH_MAX];
    unsigned char buf[2];
    int fd;
    ssize_t n;

    if (dev_path(bdf, "config", path, sizeof(path)) != 0)
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = pread(fd, buf, sizeof(buf), 4);
    close(fd);
    if (n != 2)
        return -1;
    *command = (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
    return 0;
}

static int memory_enabled(const char *bdf)
{
    uint16_t command;

    if (get_command(bdf, &command) != 0)
        return -1;
    return (command & 0x0002u) ? 1 : 0;
}

static int set_enable(const char *bdf, int quiet, int *changed)
{
    char path[PATH_MAX];
    uint16_t command;
    int fd;
    int enabled = 0;

    if (changed != NULL)
        *changed = 0;
    if (memory_enabled(bdf) == 1)
        return 0;

    /*
     * Only touch the sysfs refcount if it is currently zero.  Writing "1"
     * when it is already nonzero just increments the refcount and does not
     * fix a stale COMMAND register.
     */
    if (dev_path(bdf, "enable", path, sizeof(path)) == 0) {
        char buf[32];
        int enable_count = -1;

        if (read_text(path, buf, sizeof(buf)) == 0)
            enable_count = atoi(buf);
        if (enable_count == 0) {
            fd = open(path, O_WRONLY | O_CLOEXEC);
            if (fd >= 0) {
                ssize_t n = write(fd, "1\n", 2);
                (void)n;
                close(fd);
            }
        }
    }
    if (memory_enabled(bdf) == 1)
        enabled = 1;

    /*
     * If enable_cnt was already nonzero, writing "1" may not touch the
     * command register.  Set PCI_COMMAND_MEMORY directly as a fallback.
     */
    if (!enabled && get_command(bdf, &command) == 0 &&
        dev_path(bdf, "config", path, sizeof(path)) == 0) {
        unsigned char buf[2];

        command |= 0x0002u;
        buf[0] = (unsigned char)(command & 0xffu);
        buf[1] = (unsigned char)(command >> 8);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = pwrite(fd, buf, sizeof(buf), 4);
            (void)n;
            close(fd);
        }
        if (memory_enabled(bdf) == 1)
            enabled = 1;
    }

    if (!enabled) {
        errno = EIO;
        return -1;
    }
    if (changed != NULL)
        *changed = 1;
    if (!quiet)
        fprintf(stderr, "%s: enabled PCI memory decoding for %s\n", progname, bdf);
    return 0;
}

static int read_bars(const char *bdf, struct bar_info bars[MAX_BARS])
{
    char path[PATH_MAX];
    char line[256];
    FILE *fp;
    int i;

    memset(bars, 0, sizeof(struct bar_info) * MAX_BARS);
    if (dev_path(bdf, "resource", path, sizeof(path)) != 0)
        return -1;
    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;

    for (i = 0; i < MAX_BARS && fgets(line, sizeof(line), fp) != NULL; i++) {
        unsigned long long start = 0, end = 0, flags = 0;

        if (sscanf(line, "%llx %llx %llx", &start, &end, &flags) != 3)
            continue;
        bars[i].index = i;
        bars[i].start = (uint64_t)start;
        bars[i].end = (uint64_t)end;
        bars[i].flags = (uint64_t)flags;
        bars[i].assigned = (start != 0 || end != 0);
        bars[i].size = (bars[i].assigned && end >= start) ? (end - start + 1) : 0;
        snprintf(bars[i].path, sizeof(bars[i].path),
                 "%s/%s/resource%d", SYS_PCI_DEVICES, bdf, i);
    }
    fclose(fp);
    return i;
}

static int read_bar_one(const char *bdf, int index, struct bar_info *out)
{
    struct bar_info bars[MAX_BARS];
    int n;

    if (index < 0 || index >= MAX_BARS)
        return -1;
    n = read_bars(bdf, bars);
    if (n < 0 || index >= n)
        return -1;
    *out = bars[index];
    return out->assigned ? 0 : -1;
}

static int cmp_str(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;

    return strcmp(*sa, *sb);
}

static int find_bar_for_addr(uint64_t addr, char *bdf_out, size_t bdf_size,
                             struct bar_info *bar_out)
{
    DIR *dir;
    struct dirent *de;

    dir = opendir(SYS_PCI_DEVICES);
    if (dir == NULL)
        return 0;

    while ((de = readdir(dir)) != NULL) {
        struct bar_info bars[MAX_BARS];
        int n, i;

        if (de->d_name[0] == '.')
            continue;
        n = read_bars(de->d_name, bars);
        if (n < 0)
            continue;
        for (i = 0; i < n; i++) {
            if (!bars[i].assigned || bars[i].size == 0)
                continue;
            if (addr >= bars[i].start && addr <= bars[i].end) {
                size_t nlen = strlen(de->d_name);
                if (nlen >= bdf_size)
                    continue;
                memcpy(bdf_out, de->d_name, nlen + 1);
                *bar_out = bars[i];
                closedir(dir);
                return 1;
            }
        }
    }
    closedir(dir);
    return 0;
}

static void hint_for_addr(uint64_t addr)
{
    char bdf[64];
    struct bar_info bar;

    if (!find_bar_for_addr(addr, bdf, sizeof(bdf), &bar))
        return;
    if (memory_enabled(bdf) == 1)
        return;
    fprintf(stderr,
            "%s: note: 0x%016" PRIx64 " lies in BAR%d of %s, but memory decoding is off.\n"
            "       enable it with: %s --pci %s   (or use --bar %s:%d)\n",
            progname, addr, bar.index, bdf, progname, bdf, bdf, bar.index);
}

/* ------------------------------------------------------------------ */
/* PCI information                                                     */
/* ------------------------------------------------------------------ */

static void flags_string(uint64_t flags, char *out, size_t outsize)
{
    size_t used = 0;

    out[0] = '\0';
#define APPEND_FLAG(bit, text)                                                   \
    do {                                                                         \
        if ((flags & (bit)) != 0) {                                              \
            int n = snprintf(out + used, outsize - used, "%s%s", used ? " " : "", text); \
            if (n > 0 && (size_t)n < outsize - used)                             \
                used += (size_t)n;                                               \
        }                                                                        \
    } while (0)

    APPEND_FLAG(0x00000100ULL, "io");
    APPEND_FLAG(0x00000200ULL, "mem");
    APPEND_FLAG(0x00002000ULL, "pref");
    APPEND_FLAG(0x00100000ULL, "64bit");
#undef APPEND_FLAG
    if (out[0] == '\0')
        snprintf(out, outsize, "none");
}

static void print_bar(const struct bar_info *bar)
{
    char flags[64];

    if (!bar->assigned) {
        printf("  BAR%d: unassigned\n", bar->index);
        return;
    }
    flags_string(bar->flags, flags, sizeof(flags));
    printf("  BAR%d: phys=0x%016" PRIx64 " size=0x%" PRIx64 " flags=0x%" PRIx64 " [%s]\n",
           bar->index, bar->start, bar->size, bar->flags, flags);
    printf("        %s\n", bar->path);
}

static void driver_name(const char *bdf, char *out, size_t outsize)
{
    char path[PATH_MAX];
    char buf[PATH_MAX];
    ssize_t n;
    char *slash;

    snprintf(out, outsize, "-");
    if (dev_path(bdf, "driver", path, sizeof(path)) != 0)
        return;
    n = readlink(path, buf, sizeof(buf) - 1);
    if (n <= 0)
        return;
    buf[n] = '\0';
    slash = strrchr(buf, '/');
    {
        const char *name = (slash != NULL) ? slash + 1 : buf;
        size_t len = strlen(name);

        if (len >= outsize)
            len = outsize - 1;
        memcpy(out, name, len);
        out[len] = '\0';
    }
}

static void show_device_detail(const char *bdf)
{
    char path[PATH_MAX];
    char drv[128], speed[64], width[64];
    uint32_t vendor = 0, device = 0, klass = 0;
    int en;
    struct bar_info bars[MAX_BARS];
    int n, i;

    if (dev_path(bdf, "vendor", path, sizeof(path)) == 0)
        (void)read_hex_u32(path, &vendor);
    if (dev_path(bdf, "device", path, sizeof(path)) == 0)
        (void)read_hex_u32(path, &device);
    if (dev_path(bdf, "class", path, sizeof(path)) == 0)
        (void)read_hex_u32(path, &klass);

    driver_name(bdf, drv, sizeof(drv));
    if (dev_path(bdf, "current_link_speed", path, sizeof(path)) != 0 ||
        read_text(path, speed, sizeof(speed)) != 0)
        snprintf(speed, sizeof(speed), "?");
    if (dev_path(bdf, "current_link_width", path, sizeof(path)) != 0 ||
        read_text(path, width, sizeof(width)) != 0)
        snprintf(width, sizeof(width), "?");

    en = memory_enabled(bdf);
    printf("%s  %04x:%04x  class=0x%06x\n", bdf, vendor, device, klass);
    printf("  driver: %s\n", drv);
    printf("  memory decoding: %s\n",
           en == 1 ? "on" : (en == 0 ? "off" : "unknown"));
    printf("  link: %s x%s\n", speed, width);

    n = read_bars(bdf, bars);
    if (n < 0) {
        printf("  BARs: cannot read %s/%s/resource\n", SYS_PCI_DEVICES, bdf);
        return;
    }
    for (i = 0; i < n; i++)
        print_bar(&bars[i]);
    if (en != 1)
        printf("  hint: %s r.l --bar %s:0 0 0x100\n", progname, bdf);
}

static void show_device_summary(const char *bdf)
{
    char path[PATH_MAX];
    char drv[128];
    uint32_t vendor = 0, device = 0, klass = 0;
    int en;

    if (dev_path(bdf, "vendor", path, sizeof(path)) == 0)
        (void)read_hex_u32(path, &vendor);
    if (dev_path(bdf, "device", path, sizeof(path)) == 0)
        (void)read_hex_u32(path, &device);
    if (dev_path(bdf, "class", path, sizeof(path)) == 0)
        (void)read_hex_u32(path, &klass);
    driver_name(bdf, drv, sizeof(drv));
    en = memory_enabled(bdf);
    printf("%-16s %04x:%04x  %06x  %-3s  %s\n", bdf, vendor, device, klass,
           en == 1 ? "on" : (en == 0 ? "off" : "?"), drv);
}

static int list_pci(void)
{
    DIR *dir;
    struct dirent *de;
    char *names[512];
    int count = 0, i;

    dir = opendir(SYS_PCI_DEVICES);
    if (dir == NULL) {
        msg("cannot open %s: %s", SYS_PCI_DEVICES, strerror(errno));
        return -1;
    }
    while ((de = readdir(dir)) != NULL && count < (int)(sizeof(names) / sizeof(names[0]))) {
        if (de->d_name[0] == '.')
            continue;
        names[count] = strdup(de->d_name);
        if (names[count] == NULL)
            break;
        count++;
    }
    closedir(dir);

    qsort(names, (size_t)count, sizeof(names[0]), cmp_str);
    printf("%-16s %-9s  %-6s  %-3s  %s\n", "BDF", "VEN:DEV", "CLASS", "MEM", "DRIVER");
    for (i = 0; i < count; i++) {
        show_device_summary(names[i]);
        free(names[i]);
    }
    if (count > 0)
        printf("\nUse \"%s pci <BDF>\" for BAR details.\n", progname);
    return 0;
}

static int show_bar_cmd(const char *bdf, int index, int enable)
{
    struct bar_info bars[MAX_BARS];
    int n, i;

    if (enable) {
        int changed = 0;
        if (set_enable(bdf, 0, &changed) != 0)
            die("cannot enable %s: %s", bdf, strerror(errno));
    }

    n = read_bars(bdf, bars);
    if (n < 0)
        die("cannot read BARs for %s: %s", bdf, strerror(errno));

    if (index >= 0) {
        if (index >= n || !bars[index].assigned)
            die("%s BAR%d is not assigned", bdf, index);
        print_bar(&bars[index]);
        printf("  hint: %s r.l --bar %s:%d 0 0x100\n", progname, bdf, index);
        return 0;
    }

    for (i = 0; i < n; i++) {
        if (bars[i].assigned)
            print_bar(&bars[i]);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* MMIO access                                                         */
/* ------------------------------------------------------------------ */

static uint64_t mmio_load(volatile uint8_t *p, int width)
{
    switch (width) {
    case 1:
        return *(volatile uint8_t *)p;
    case 2:
        return from_le16(*(volatile uint16_t *)p);
    case 4:
        return from_le32(*(volatile uint32_t *)p);
    case 8:
#if defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ >= 8)
        if (g_qword_atomic)
            return from_le64(*(volatile uint64_t *)p);
#endif
        {
            uint32_t lo = *(volatile uint32_t *)p;
            uint32_t hi = *(volatile uint32_t *)(p + 4);
            return (uint64_t)from_le32(lo) | ((uint64_t)from_le32(hi) << 32);
        }
    default:
        return 0;
    }
}

static void mmio_store(volatile uint8_t *p, int width, uint64_t value)
{
    switch (width) {
    case 1:
        *(volatile uint8_t *)p = (uint8_t)value;
        break;
    case 2:
        *(volatile uint16_t *)p = to_le16((uint16_t)value);
        break;
    case 4:
        *(volatile uint32_t *)p = to_le32((uint32_t)value);
        break;
    case 8:
#if defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ >= 8)
        if (g_qword_atomic) {
            *(volatile uint64_t *)p = to_le64(value);
            break;
        }
#endif
        *(volatile uint32_t *)p = to_le32((uint32_t)value);
        *(volatile uint32_t *)(p + 4) = to_le32((uint32_t)(value >> 32));
        break;
    default:
        break;
    }
}

static void store_le(uint8_t *p, uint64_t value, int width)
{
    int i;

    for (i = 0; i < width; i++)
        p[i] = (uint8_t)(value >> (8 * i));
}

static uint64_t get_le(const uint8_t *p, int width)
{
    uint64_t value = 0;
    int i;

    for (i = 0; i < width; i++)
        value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static void hexdump(const uint8_t *buf, size_t len, uint64_t base, int width,
                    int no_ascii)
{
    const size_t row = 16;
    size_t off;

    for (off = 0; off < len; off += row) {
        size_t n = len - off < row ? len - off : row;
        size_t e;

        printf("%016" PRIx64 ": ", base + (uint64_t)off);
        for (e = 0; e < row; e += (size_t)width) {
            if (e < n) {
                uint64_t value = get_le(buf + off + e, width);
                printf("%0*" PRIx64 " ", width * 2, value);
            } else {
                printf("%*s ", width * 2, "");
            }
        }
        if (!no_ascii) {
            putchar(' ');
            for (e = 0; e < n; e++) {
                unsigned char c = buf[off + e];
                putchar(isprint(c) ? c : '.');
            }
        }
        putchar('\n');
    }
    putchar('\n');
}

/* ------------------------------------------------------------------ */
/* Open target                                                         */
/* ------------------------------------------------------------------ */

static void die_mmap(uint64_t addr, int err)
{
    fprintf(stderr, "%s: mmap at 0x%016" PRIx64 " failed: %s\n",
            progname, addr, strerror(err));
    if (err == EPERM) {
        fprintf(stderr,
                "  /dev/mem is restricted by the kernel (CONFIG_STRICT_DEVMEM).\n"
                "  PCI BAR MMIO is normally still allowed.  For a BAR use:\n"
                "    %s r.l --bar <BDF>:<BAR> <offset> <len>\n",
                progname);
    }
    exit(1);
}

static void open_target(struct options *o, uint64_t *addr, struct target *t)
{
    memset(t, 0, sizeof(*t));
    t->fd = -1;

    if (o->file[0] != '\0') {
        int flags = (o->op == OP_WRITE) ? O_RDWR : O_RDONLY;
        struct stat st;

        t->fd = open(o->file, flags | O_CLOEXEC);
        if (t->fd < 0 && o->op == OP_READ)
            t->fd = open(o->file, O_RDONLY | O_CLOEXEC);
        if (t->fd < 0)
            die("cannot open %s: %s", o->file, strerror(errno));
        if (fstat(t->fd, &st) != 0)
            die("fstat %s: %s", o->file, strerror(errno));
        if (!S_ISREG(st.st_mode))
            die("%s is not a regular file", o->file);
        t->limit = (uint64_t)st.st_size;
        t->has_limit = 1;
        t->base = 0;
        t->is_mmio = 0;
        return;
    }

    if (o->use_bar) {
        struct bar_info bar;
        int flags;
        int changed = 0;

        if (read_bar_one(o->bar_bdf, o->bar_index, &bar) != 0)
            die("%s BAR%d is not assigned", o->bar_bdf, o->bar_index);
        if (!o->no_enable) {
            if (set_enable(o->bar_bdf, o->quiet, &changed) != 0)
                die("cannot enable %s: %s", o->bar_bdf, strerror(errno));
        }
        flags = (o->op == OP_WRITE) ? O_RDWR : O_RDONLY;
        t->fd = open(bar.path, flags | O_CLOEXEC);
        if (t->fd < 0 && o->op == OP_READ)
            t->fd = open(bar.path, O_RDONLY | O_CLOEXEC);
        if (t->fd < 0)
            die("cannot open %s: %s", bar.path, strerror(errno));
        t->limit = bar.size;
        t->has_limit = 1;
        t->base = bar.start;
        t->is_mmio = 1;
        t->has_bar = 1;
        t->bar = bar;

        /* Accept either a BAR-relative offset or the absolute BAR address. */
        if (*addr >= bar.start && *addr <= bar.end)
            *addr -= bar.start;
        return;
    }

    /* /dev/mem path. */
    if (o->pci_bdf[0] != '\0') {
        if (set_enable(o->pci_bdf, o->quiet, NULL) != 0)
            die("cannot enable %s: %s", o->pci_bdf, strerror(errno));
    }
    if (o->auto_enable) {
        char bdf[64];
        struct bar_info bar;

        if (find_bar_for_addr(*addr, bdf, sizeof(bdf), &bar)) {
            if (set_enable(bdf, o->quiet, NULL) != 0)
                die("cannot enable %s: %s", bdf, strerror(errno));
        }
    } else if (o->op == OP_READ && !o->quiet) {
        hint_for_addr(*addr);
    }

    t->fd = open("/dev/mem", (o->op == OP_WRITE ? O_RDWR : O_RDONLY) | O_SYNC | O_CLOEXEC);
    if (t->fd < 0 && o->op == OP_READ)
        t->fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (t->fd < 0) {
        if (errno == ENOENT)
            die("/dev/mem does not exist (CONFIG_DEVMEM=n?); use --bar <BDF>:<N> instead");
        die("cannot open /dev/mem: %s (are you root?)", strerror(errno));
    }
    t->limit = 0;
    t->has_limit = 0;
    t->base = 0;
    t->is_mmio = 1;
}

/* ------------------------------------------------------------------ */
/* Usage                                                               */
/* ------------------------------------------------------------------ */

static void usage(FILE *fp)
{
    fprintf(fp,
            "mtool %s - portable MMIO / PCI BAR peek & poke\n"
            "\n"
            "Usage:\n"
            "  mtool r.{b|w|l|q} ADDR [LEN] [options]\n"
            "  mtool w.{b|w|l|q} ADDR VALUE [VALUE ...] [options]\n"
            "  mtool pci [BDF] [--enable]\n"
            "  mtool bar BDF [INDEX] [--enable]\n"
            "  mtool help | version\n"
            "\n"
            "  b=8-bit  w=16-bit  l=32-bit  q=64-bit\n"
            "  LEN is a byte count, not an element count.  A read rounds the\n"
            "  start down and the end up to whole access units, e.g.\n"
            "  r.l 0x0 0x1 and r.l 0x0 0x4 both return one 32-bit word.\n"
            "  Numbers are hexadecimal by default (0x prefix optional); use\n"
            "  --decimal for base 10.  So \"r.l 0x404040000 100\" reads 0x100\n"
            "  bytes (256 bytes).\n"
            "\n"
            "Examples:\n"
            "  mtool r.l 0x404040000 100   # 0x100 bytes, 64 words\n"
            "  mtool w.l 0x404040088 0xb600\n"
            "  mtool pci\n"
            "  mtool pci 01:00.0\n"
            "  mtool r.l --bar 01:00.0:0 0 0x100\n"
            "  mtool r.l --file /tmp/dump.bin 0 0x40\n"
            "\n"
            "Options:\n"
            "  --file PATH       access a regular file instead of /dev/mem\n"
            "  --bar BDF:INDEX   access PCI BAR through sysfs resourceN\n"
            "                    (ADDR is an offset, or the absolute BAR address)\n"
            "  --pci BDF         enable PCI memory decoding before /dev/mem access\n"
            "  --auto-enable     find the BAR containing ADDR and enable its device\n"
            "  --no-enable       do not auto-enable with --bar\n"
            "  --read-only       open the backing read-only\n"
            "  --no-ascii        omit the ASCII column\n"
            "  --hex             parse bare numbers as hexadecimal (default)\n"
            "  --decimal         parse bare numbers as decimal\n"
            "  --qword-atomic    do a single 64-bit MMIO access for q.*\n"
            "                    (default: two 32-bit accesses, safer on\n"
            "                     32-bit BARs / AXI-Lite)\n"
            "  -v, --verbose     print write confirmations\n"
            "  -q, --quiet       suppress notices\n"
            "  -h, --help        this help\n"
            "\n"
            "Notes:\n"
            "  * /dev/mem needs root.  With CONFIG_STRICT_DEVMEM=y, System RAM is\n"
            "    blocked but PCI/MMIO ranges are normally still accessible.\n"
            "  * --bar uses /sys/bus/pci/devices/<BDF>/resourceN and works even\n"
            "    when CONFIG_STRICT_DEVMEM blocks RAM.  It auto-enables memory\n"
            "    decoding unless --no-enable is given.\n"
            "  * Writes are dangerous.  Double-check the address before using w.*.\n",
            MTOOL_VERSION);
}

/* ------------------------------------------------------------------ */
/* Option parsing                                                      */
/* ------------------------------------------------------------------ */

static int parse_bar_spec(const char *spec, char *bdf, size_t bdfsize, int *index)
{
    char *copy, *colon;
    long v;
    char *end;
    int rc;

    copy = strdup(spec);
    if (copy == NULL)
        return -1;
    colon = strrchr(copy, ':');
    if (colon == NULL || colon == copy) {
        free(copy);
        return -1;
    }
    *colon = '\0';
    errno = 0;
    v = strtol(colon + 1, &end, 0);
    if (errno != 0 || end == colon + 1 || *end != '\0' || v < 0 || v >= MAX_BARS) {
        free(copy);
        return -1;
    }
    rc = normalize_bdf(copy, bdf, bdfsize);
    free(copy);
    if (rc != 0)
        return -1;
    *index = (int)v;
    return 0;
}

static int parse_options(int argc, char **argv, struct options *o,
                         char **pos, int *npos)
{
    int i;
    int maxpos = 300;

    *npos = 0;
    for (i = 2; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--file") == 0) {
            if (++i >= argc)
                die("--file needs a path");
            snprintf(o->file, sizeof(o->file), "%s", argv[i]);
        } else if (strcmp(a, "--bar") == 0) {
            if (++i >= argc)
                die("--bar needs BDF:INDEX");
            if (parse_bar_spec(argv[i], o->bar_bdf, sizeof(o->bar_bdf),
                               &o->bar_index) != 0)
                die("bad --bar spec '%s' (expected BDF:INDEX)", argv[i]);
            o->use_bar = 1;
        } else if (strcmp(a, "--pci") == 0) {
            if (++i >= argc)
                die("--pci needs a BDF");
            if (normalize_bdf(argv[i], o->pci_bdf, sizeof(o->pci_bdf)) != 0)
                die("bad BDF '%s'", argv[i]);
        } else if (strcmp(a, "--enable") == 0) {
            o->pci_enable = 1;
        } else if (strcmp(a, "--no-enable") == 0) {
            o->no_enable = 1;
        } else if (strcmp(a, "--auto-enable") == 0) {
            o->auto_enable = 1;
        } else if (strcmp(a, "--read-only") == 0) {
            o->read_only = 1;
        } else if (strcmp(a, "--no-ascii") == 0) {
            o->no_ascii = 1;
        } else if (strcmp(a, "--hex") == 0) {
            g_hex_default = 1;
        } else if (strcmp(a, "--decimal") == 0) {
            g_hex_default = 0;
        } else if (strcmp(a, "--qword-atomic") == 0) {
            o->qword_atomic = 1;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            o->verbose = 1;
        } else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
            o->quiet = 1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            exit(0);
        } else if (a[0] == '-' && a[1] != '\0') {
            die("unknown option '%s' (try --help)", a);
        } else {
            if (*npos >= maxpos)
                die("too many arguments");
            pos[(*npos)++] = argv[i];
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    struct options o;
    struct target t;
    char *pos[300];
    int npos = 0;
    const char *cmd;
    enum op op;
    int width;
    uint64_t addr;
    uint64_t read_len_raw = 0;
    uint8_t *buf = NULL;
    size_t len = 0;
    void *map = MAP_FAILED;
    volatile uint8_t *ptr;
    uint64_t page_mask, map_off, delta;
    size_t maplen;
    long pagesize;
    int prot;
    int i;

    signal(SIGBUS, on_sigbus);
    memset(&o, 0, sizeof(o));
    o.width = 4;
    g_qword_atomic = 0;
    g_hex_default = 1;

    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    cmd = argv[1];

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("mtool %s\n", MTOOL_VERSION);
        return 0;
    }

    parse_options(argc, argv, &o, pos, &npos);

    if (strcmp(cmd, "pci") == 0) {
        char bdf[64];

        if (npos > 1)
            die("pci takes at most one BDF");
        if (npos == 1) {
            if (normalize_bdf(pos[0], bdf, sizeof(bdf)) != 0)
                die("bad BDF '%s'", pos[0]);
            if (o.pci_enable) {
                if (set_enable(bdf, o.quiet, NULL) != 0)
                    die("cannot enable %s: %s", bdf, strerror(errno));
            }
            show_device_detail(bdf);
        } else {
            if (o.pci_enable)
                die("--enable with \"pci\" needs a BDF");
            return list_pci() == 0 ? 0 : 1;
        }
        return 0;
    }

    if (strcmp(cmd, "bar") == 0) {
        char bdf[64];
        int index = -1;

        if (npos < 1 || npos > 2)
            die("bar needs BDF [INDEX]");
        if (normalize_bdf(pos[0], bdf, sizeof(bdf)) != 0)
            die("bad BDF '%s'", pos[0]);
        if (npos == 2) {
            if (parse_int(pos[1], &index) != 0 || index < 0 || index >= MAX_BARS)
                die("bad BAR index '%s'", pos[1]);
        }
        return show_bar_cmd(bdf, index, o.pci_enable) == 0 ? 0 : 1;
    }

    if (strlen(cmd) != 3 || cmd[1] != '.')
        die("unknown command '%s' (try --help)", cmd);

    if (cmd[0] == 'r')
        op = OP_READ;
    else if (cmd[0] == 'w')
        op = OP_WRITE;
    else
        die("unknown command '%s' (try --help)", cmd);

    switch (cmd[2]) {
    case 'b':
        width = 1;
        break;
    case 'w':
        width = 2;
        break;
    case 'l':
        width = 4;
        break;
    case 'q':
        width = 8;
        break;
    default:
        die("unknown width '%c' (use b, w, l or q)", cmd[2]);
        width = 0; /* not reached */
    }
    o.op = op;
    o.width = width;
    g_qword_atomic = o.qword_atomic;

    if (o.file[0] != '\0' && o.use_bar)
        die("--file and --bar are mutually exclusive");
    if (o.read_only && op == OP_WRITE)
        die("--read-only cannot be used with w.*");

    if (op == OP_READ) {
        if (npos < 1 || npos > 2)
            die("read usage: mtool r.%c ADDR [LEN]", cmd[2]);
    } else {
        if (npos < 2)
            die("write usage: mtool w.%c ADDR VALUE [VALUE ...]", cmd[2]);
    }

    if (parse_u64(pos[0], &addr) != 0)
        die("bad address '%s'", pos[0]);

    if (op == OP_READ) {
        if (npos == 2) {
            if (parse_u64(pos[1], &read_len_raw) != 0)
                die("bad length '%s'", pos[1]);
        } else {
            read_len_raw = (uint64_t)width;
        }
        if (read_len_raw == 0)
            die("length must be > 0");
    } else {
        uint64_t values[MAX_WRITE_VALUES];
        int nvalues = npos - 1;

        if (nvalues > MAX_WRITE_VALUES)
            die("too many values (max %d)", MAX_WRITE_VALUES);
        for (i = 0; i < nvalues; i++) {
            uint64_t v;

            if (parse_u64(pos[i + 1], &v) != 0)
                die("bad value '%s'", pos[i + 1]);
            if (width < 8) {
                uint64_t max = (1ULL << (width * 8)) - 1;
                if (v > max)
                    msg("warning: value 0x%" PRIx64 " truncated to %d bits",
                        v, width * 8);
            }
            values[i] = v;
        }
        len = (size_t)nvalues * (size_t)width;
        if (len == 0)
            die("nothing to write");

        /* Stash values in a heap buffer so open_target() can be called first. */
        buf = malloc(len);
        if (buf == NULL)
            die("out of memory");
        for (i = 0; i < nvalues; i++)
            store_le(buf + (size_t)i * (size_t)width, values[i], width);
    }

    open_target(&o, &addr, &t);

    if (op == OP_READ) {
        uint64_t req_addr = addr;
        uint64_t wmask = (uint64_t)width - 1;
        uint64_t start, end, end_aligned, nbytes;

        if (req_addr > UINT64_MAX - read_len_raw)
            die("address + length overflow");
        end = req_addr + read_len_raw;
        if (end > UINT64_MAX - wmask)
            die("address + length overflow");
        start = req_addr & ~wmask;
        end_aligned = (end + wmask) & ~wmask;
        if (end_aligned < start)
            die("address overflow");
        nbytes = end_aligned - start;
        if (nbytes > (uint64_t)SIZE_MAX)
            die("length too large");
        len = (size_t)nbytes;
        addr = start;
    }

    if (t.has_limit) {
        if (addr > t.limit || (uint64_t)len > t.limit - addr)
            die("range 0x%016" PRIx64 " + 0x%zx exceeds 0x%016" PRIx64,
                addr, len, t.limit);
    }
    if (addr > UINT64_MAX - (uint64_t)len)
        die("address overflow");

    if (t.is_mmio) {
        uint64_t step = (uint64_t)width;
        uint64_t align = (uint64_t)width;

        if (width == 8) {
#if defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ >= 8)
            if (!g_qword_atomic)
                align = 4; /* split into two 32-bit accesses */
#else
            align = 4; /* cannot do a single 64-bit access on 32-bit */
#endif
        }
        for (i = 0; (uint64_t)i * step < (uint64_t)len; i++) {
            if ((addr + (uint64_t)i * step) % align != 0)
                die("unaligned %d-bit MMIO access at 0x%016" PRIx64,
                    width * 8, addr + (uint64_t)i * step);
        }
    }

    pagesize = sysconf(_SC_PAGESIZE);
    if (pagesize <= 0)
        pagesize = 4096;
    page_mask = (uint64_t)pagesize - 1;
    map_off = addr & ~page_mask;
    delta = addr - map_off;
    maplen = (size_t)delta + len;
    if (maplen < len)
        die("mapping size overflow");

    if (op == OP_READ)
        prot = PROT_READ;
    else
        prot = PROT_READ | PROT_WRITE;

    map = mmap(NULL, maplen, prot, MAP_SHARED, t.fd, (off_t)map_off);
    if (map == MAP_FAILED)
        die_mmap(addr, errno);
    ptr = (volatile uint8_t *)map + delta;

    if (op == OP_READ) {
        uint8_t *readbuf = malloc(len == 0 ? 1 : len);

        if (readbuf == NULL)
            die("out of memory");
        if (t.is_mmio) {
            size_t k;

            for (k = 0; k < len; k += (size_t)width) {
                uint64_t v = mmio_load(ptr + k, width);
                store_le(readbuf + k, v, width);
            }
        } else {
            memcpy(readbuf, (const void *)(uintptr_t)ptr, len);
        }
        hexdump(readbuf, len, t.base + addr, width, o.no_ascii);
        free(readbuf);
    } else {
        size_t k;

        for (k = 0; k < len; k += (size_t)width) {
            uint64_t v = get_le(buf + k, width);

            if (t.is_mmio)
                mmio_store(ptr + k, width, v);
            else
                store_le((uint8_t *)(uintptr_t)(ptr + k), v, width);
            if (o.verbose)
                printf("0x%016" PRIx64 " <- 0x%0*" PRIx64 " (%d-bit)\n",
                       t.base + addr + (uint64_t)k, width * 2, v, width * 8);
        }
        if (t.is_mmio) {
            __sync_synchronize();
        } else {
            (void)msync(map, maplen, MS_SYNC);
        }
    }

    munmap(map, maplen);
    close(t.fd);
    free(buf);
    return 0;
}