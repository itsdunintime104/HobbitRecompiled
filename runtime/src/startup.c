/* Startup driver: owns main(). It does what the kernel and dyld do for the
 * i386 Mach-O image, then runs the lifted entry point. Order follows
 * docs/STARTUP_REQS.md. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach-o/loader.h>
#include <sys/mman.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "handles.h"
#include "data_imports.h"
#include "diag.h"

extern char **environ;
void thunk_rt_run_atexit(void);
void data_imports_set_image(const char *path);

/* Main guest stack, 16 MB below the 0xFE000000 kernel thunk range and clear
 * of the static area at 0x00F80000 (STARTUP_REQS step 8). */
#define MAIN_STACK_LO 0xFD000000u
#define MAIN_STACK_HI 0xFE000000u
/* Return address pushed under a static constructor. Never mapped. */
#define CTOR_RET_SENTINEL 0xFFFFF000u
#define ENTRY_VA 0x00002720u

struct sect_info {
    uint32_t addr, size, flags, reserved1;
};

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint32_t g_rd(uint32_t a)
{
    uint32_t v;
    memcpy(&v, guest_ptr(a), 4);
    return v;
}

static void g_wr(uint32_t a, uint32_t v)
{
    memcpy(guest_ptr(a), &v, 4);
}

/* Copy n bytes to the guest stack below *sp and return their guest address. */
static uint32_t push_bytes(uint32_t *sp, const void *src, uint32_t n)
{
    *sp -= n;
    memcpy(guest_ptr(*sp), src, n);
    return *sp;
}

/* Find the first section of the given type in the load commands kept in the
 * header side buffer. */
static int find_section(uint32_t type, struct sect_info *out)
{
    const uint8_t *h = guest_image_header();
    uint32_t ncmds = rd32(h + 16), off = 28;
    for (uint32_t i = 0; i < ncmds && off + 8 <= GUEST_HEADER_SIZE; i++) {
        uint32_t cmd = rd32(h + off), cmdsize = rd32(h + off + 4);
        if (cmd == LC_SEGMENT) {
            uint32_t nsects = rd32(h + off + 48);
            const uint8_t *s = h + off + 56;
            for (uint32_t k = 0; k < nsects; k++, s += 68) {
                uint32_t flags = rd32(s + 56);
                if ((flags & SECTION_TYPE) == type) {
                    *out = (struct sect_info){rd32(s + 32), rd32(s + 36), flags, rd32(s + 60)};
                    return 0;
                }
            }
        }
        off += cmdsize;
    }
    return -1;
}

/* Lay out the i386 Darwin process start stack and return the guest address
 * of argc. */
static uint32_t build_start_stack(int argc, char **argv, uint32_t *gargv,
                                  uint32_t *genvp, uint32_t *gapple)
{
    uint32_t sp = MAIN_STACK_HI - 16;
    int envc = 0;
    while (environ[envc])
        envc++;

    /* Guest argv: the image path, then the host arguments after it. */
    int gargc = argc - 1;
    uint32_t *av = calloc((size_t)gargc + 1, 4), *ev = calloc((size_t)envc + 1, 4);
    for (int i = 0; i < gargc; i++)
        av[i] = push_bytes(&sp, argv[i + 1], (uint32_t)strlen(argv[i + 1]) + 1);
    for (int i = 0; i < envc; i++)
        ev[i] = push_bytes(&sp, environ[i], (uint32_t)strlen(environ[i]) + 1);
    uint32_t apple0 = push_bytes(&sp, argv[1], (uint32_t)strlen(argv[1]) + 1);

    /* argc, argv[], 0, envp[], 0, apple[], 0 from low to high address. */
    uint32_t words = 1 + (uint32_t)gargc + 1 + (uint32_t)envc + 1 + 2;
    sp = (sp - words * 4) & ~0xFu;
    uint32_t p = sp;
    g_wr(p, (uint32_t)gargc), p += 4;
    *gargv = p;
    for (int i = 0; i < gargc; i++)
        g_wr(p, av[i]), p += 4;
    g_wr(p, 0), p += 4;
    *genvp = p;
    for (int i = 0; i < envc; i++)
        g_wr(p, ev[i]), p += 4;
    g_wr(p, 0), p += 4;
    *gapple = p;
    g_wr(p, apple0), p += 4;
    g_wr(p, 0);
    free(av);
    free(ev);
    return sp;
}

static recomp_func_t lookup_or_die(uint32_t va, const char *what)
{
    recomp_func_t fn = recomp_lookup(va);
    if (fn == NULL) {
        fprintf(stderr, "startup: %s 0x%08X is not recompiled\n", what, va);
        exit(3);
    }
    return fn;
}

/* Call each __mod_init_func entry as dyld does: initializer(argc, argv,
 * envp, apple) with a return address under the arguments. */
static void run_static_ctors(uint32_t gargc, uint32_t gargv, uint32_t genvp, uint32_t gapple)
{
    struct sect_info si;
    if (find_section(S_MOD_INIT_FUNC_POINTERS, &si) != 0)
        return;
    uint32_t n = si.size / 4;
    const char *verbose = getenv("RECOMP_STARTUP_VERBOSE");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t va = g_rd(si.addr + 4 * i);
        recomp_func_t fn = lookup_or_die(va, "static constructor");
        if (verbose)
            fprintf(stderr, "startup: ctor %u/%u 0x%08X\n", i + 1, n, va);
        uint32_t saved = g_esp;
        g_esp -= 16;
        g_wr(g_esp + 0, gargc);
        g_wr(g_esp + 4, gargv);
        g_wr(g_esp + 8, genvp);
        g_wr(g_esp + 12, gapple);
        g_esp -= 4;
        g_wr(g_esp, CTOR_RET_SENTINEL);
        g_ebp = 0;
        g_seh_ebp = 0;
        fn();
        g_esp = saved;
    }
    fprintf(stderr, "startup: %u static constructors done\n", n);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <i386 image> [args...]\n", argv[0]);
        return 2;
    }
    /* Step 1. */
    if (guest_mem_init() != 0 || guest_load_image(argv[1]) != 0)
        return 1;
    /* Step 2: MEM32 and the other macros add this offset. */
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest_base;
    /* Step 3. */
    thunk_files_init();
    /* Step 4. */
    data_imports_set_image(argv[1]);
    data_imports_bind_all();
    diag_init(); /* RECOMP_HANGWATCH and friends; no-op unless set */
    /* Step 7: Darwin i386 FPU defaults for the main thread. */
    g_fp_control_word = 0x037F;
    g_mxcsr = 0x1F80;
    /* Step 6: imp__exit calls the host exit(), which runs this. */
    atexit(thunk_rt_run_atexit);
    recomp_dispatch_init();

    if (mprotect(guest_ptr(MAIN_STACK_LO), MAIN_STACK_HI - MAIN_STACK_LO,
                 PROT_READ | PROT_WRITE) != 0) {
        perror("startup: map main stack");
        return 1;
    }
    uint32_t gargv, genvp, gapple;
    uint32_t sp = build_start_stack(argc, argv, &gargv, &genvp, &gapple);
    g_esp = sp;
    run_static_ctors(g_rd(sp), gargv, genvp, gapple);

    /* Kernel entry: g_esp points at argc, no return address. */
    g_esp = sp;
    g_ebp = 0;
    g_seh_ebp = 0;
    recomp_func_t entry = lookup_or_die(ENTRY_VA, "entry");
    fprintf(stderr, "startup: calling entry 0x%08X\n", ENTRY_VA);
    entry();
    fprintf(stderr, "startup: entry returned\n");
    return 0;
}
