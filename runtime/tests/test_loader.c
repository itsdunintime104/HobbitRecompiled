/* Usage:
 *   test_loader <bin>                      load and self-check
 *   test_loader <bin> [--dump A N | --probe A | --header OFF N]...
 * Addresses are hex. Each option prints one line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"

static int self_check(void)
{
    uint8_t b;
    if (guest_mem_probe(0, &b) == 0) {
        fprintf(stderr, "guest 0 is readable\n");
        return 1;
    }
    const uint8_t *hdr = guest_image_header();
    if (hdr[0] != 0xce || hdr[1] != 0xfa || hdr[2] != 0xed || hdr[3] != 0xfe) {
        fprintf(stderr, "bad Mach-O magic in header buffer\n");
        return 1;
    }
    if (guest_mem_probe(0x4000, &b) != 0) {
        fprintf(stderr, "guest 0x4000 is not readable\n");
        return 1;
    }
    printf("ok\n");
    return 0;
}

static uint32_t parse_u32(const char *s)
{
    return (uint32_t)strtoul(s, NULL, 16);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <bin> [options]\n", argv[0]);
        return 2;
    }
    if (guest_mem_init() != 0 || guest_load_image(argv[1]) != 0)
        return 1;
    printf("base=%p\n", (void *)g_guest_base);

    if (argc == 2)
        return self_check();

    for (int i = 2; i < argc;) {
        if (strcmp(argv[i], "--dump") == 0 && i + 2 < argc) {
            guest_mem_dump(parse_u32(argv[i + 1]), parse_u32(argv[i + 2]));
            i += 3;
        } else if (strcmp(argv[i], "--probe") == 0 && i + 1 < argc) {
            uint8_t b;
            if (guest_mem_probe(parse_u32(argv[i + 1]), &b) == 0)
                printf("%02x\n", b);
            else
                printf("fault\n");
            i += 2;
        } else if (strcmp(argv[i], "--header") == 0 && i + 2 < argc) {
            uint32_t off = parse_u32(argv[i + 1]);
            uint32_t n = parse_u32(argv[i + 2]);
            if ((uint64_t)off + n > GUEST_HEADER_SIZE) {
                fprintf(stderr, "header range out of bounds\n");
                return 2;
            }
            const uint8_t *hdr = guest_image_header();
            for (uint32_t k = 0; k < n; k++)
                printf("%02x", hdr[off + k]);
            printf("\n");
            i += 3;
        } else {
            fprintf(stderr, "bad option: %s\n", argv[i]);
            return 2;
        }
    }
    return 0;
}
