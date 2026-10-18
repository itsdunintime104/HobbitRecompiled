#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gen_segments.h"
#include "guest_mem.h"

static uint8_t g_image_header[GUEST_HEADER_SIZE];

const uint8_t *guest_image_header(void)
{
    return g_image_header;
}

static int read_full(int fd, void *dst, uint64_t len, uint64_t off)
{
    uint8_t *p = dst;
    while (len > 0) {
        ssize_t got = pread(fd, p, (size_t)len, (off_t)off);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            perror("pread");
            return -1;
        }
        if (got == 0) {
            fprintf(stderr, "pread: unexpected end of file\n");
            return -1;
        }
        p += got;
        off += (uint64_t)got;
        len -= (uint64_t)got;
    }
    return 0;
}

static int map_segment(int fd, const guest_segment_t *seg)
{
    uint64_t page = (uint64_t)getpagesize();
    uint64_t start = seg->vmaddr & ~(page - 1);
    uint64_t end = ((uint64_t)seg->vmaddr + seg->vmsize + page - 1) & ~(page - 1);
    if (start < GUEST_LOW_RESERVED)
        start = GUEST_LOW_RESERVED;
    if (end <= start)
        return 0;

    if (mprotect(guest_ptr((uint32_t)start), (size_t)(end - start), PROT_READ | PROT_WRITE) != 0) {
        fprintf(stderr, "mprotect %s: %s\n", seg->name, strerror(errno));
        return -1;
    }

    /* Copy file bytes only for guest addresses at or above the reserved page. */
    uint64_t skip = 0;
    if (seg->vmaddr < GUEST_LOW_RESERVED)
        skip = GUEST_LOW_RESERVED - seg->vmaddr;
    if (skip >= seg->filesize)
        return 0;
    return read_full(fd, guest_ptr((uint32_t)(seg->vmaddr + skip)), seg->filesize - skip,
                     (uint64_t)seg->fileoff + skip);
}

int guest_load_image(const char *path)
{
    if (g_guest_base == NULL) {
        fprintf(stderr, "guest_load_image: guest_mem_init not called\n");
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }

    int rc = read_full(fd, g_image_header, GUEST_HEADER_SIZE, 0);
    for (unsigned i = 0; rc == 0 && i < GUEST_SEGMENT_COUNT; i++) {
        const guest_segment_t *seg = &GUEST_SEGMENTS[i];
        if (strcmp(seg->name, "__PAGEZERO") == 0 || strcmp(seg->name, "__LINKEDIT") == 0)
            continue;
        rc = map_segment(fd, seg);
    }
    close(fd);
    return rc;
}
