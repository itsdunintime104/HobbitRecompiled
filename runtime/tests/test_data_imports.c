/* Unit test for host function addresses and data-import binding. Builds a fake
 * image header and symbol tables, binds slots in guest memory, and checks them.
 * Prints "ok" and exits 0 when every check passes. */
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "data_imports.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "handles.h"
#include "host_funcs.h"
#include "recomp_types.h"

#define SLOT_BASE 0x00F10000u
#define NL_SLOTS 8u
#define RUNE_RUNETYPE_OFF 52u
#define RUNE_MAPLOWER_OFF 1076u
#define RUNE_MAPUPPER_OFF 2100u
#define CTYPE_ALPHA 0x00000100u
#define LOCAL_SLOT 0x80000000u

int data_imports_bind_image(const uint8_t *hdr, const char *path);

static int g_failures;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL line %d: ", __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
            g_failures++; \
        } \
    } while (0)

/* ---- Run-time hooks the glue leaves to generated code ------------------- */
recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

static int g_real_calls;
static void real_fn(void)
{
    g_real_calls++;
    g_eax = 0x1234;
    g_esp += 4;
}
static void other_fn(void)
{
}
static void placeholder_fn(void)
{
}
static void pure_virtual_fn(void)
{
}

/* The import table the generated file would define: sorted by name. */
const struct {
    const char *name;
    recomp_func_t fn;
} g_import_table[] = {
    {"___cxa_pure_virtual", pure_virtual_fn},
    {"_placeholder_thunk", placeholder_fn},
    {"_real_thunk", real_fn},
};
const unsigned g_import_count = 3;

int import_func_is_placeholder(recomp_func_t fn)
{
    return fn == placeholder_fn;
}

/* ---- Fake image --------------------------------------------------------- */
static void put32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static uint32_t get32(uint32_t a)
{
    uint32_t v;
    memcpy(&v, guest_ptr(a), 4);
    return v;
}

/* Bind one slot per name, at SLOT_BASE + 4 * index. A NULL name makes a
 * LOCAL indirect entry. Returns bind_image's unbound count. */
static int bind_names(const char *const *names, unsigned n, const char *path)
{
    static uint8_t hdr[GUEST_HEADER_SIZE];
    memset(hdr, 0, sizeof hdr);
    uint32_t seg_size = 56 + 68, symtab_off = 28 + seg_size, dysym_off = symtab_off + 24;
    put32(hdr + 16, 3);
    put32(hdr + 28, 1);
    put32(hdr + 32, seg_size);
    put32(hdr + 28 + 48, 1);
    uint8_t *sect = hdr + 28 + 56;
    strcpy((char *)sect, "__nl_symbol_ptr");
    put32(sect + 32, SLOT_BASE);
    put32(sect + 36, 4 * n);
    put32(sect + 60, 0);
    put32(hdr + symtab_off, 2);
    put32(hdr + symtab_off + 4, 24);
    put32(hdr + dysym_off, 0xB);
    put32(hdr + dysym_off + 4, 80);

    /* File layout: nlist at 0 (12 bytes each), strings, indirect table. */
    uint8_t file[4096] = {0};
    uint32_t stroff = 12 * n, strlen_used = 1, indoff = 2048;
    for (unsigned i = 0; i < n; i++) {
        if (names[i] == NULL) {
            put32(file + indoff + 4 * i, LOCAL_SLOT);
            continue;
        }
        put32(file + 12 * i, strlen_used);
        strcpy((char *)file + stroff + strlen_used, names[i]);
        strlen_used += (uint32_t)strlen(names[i]) + 1;
        put32(file + indoff + 4 * i, i);
    }
    put32(hdr + symtab_off + 8, 0);
    put32(hdr + symtab_off + 12, n);
    put32(hdr + symtab_off + 16, stroff);
    put32(hdr + symtab_off + 20, strlen_used);
    put32(hdr + dysym_off + 56, indoff);
    put32(hdr + dysym_off + 60, n);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, file, sizeof file) != (ssize_t)sizeof file) {
        perror("fake image");
        exit(1);
    }
    close(fd);
    return data_imports_bind_image(hdr, path);
}

static void test_host_funcs(void)
{
    uint32_t a = host_func_va(real_fn), b = host_func_va(other_fn);
    CHECK(a >= HOST_FUNC_VA_BASE && a < HOST_FUNC_VA_BASE + HOST_FUNC_MAX * HOST_FUNC_VA_STEP,
          "address 0x%08X in range", a);
    CHECK(a != b, "distinct functions, distinct addresses");
    CHECK(host_func_va(real_fn) == a && host_func_va(other_fn) == b, "idempotent");
    CHECK(guest_resolve(a) == real_fn && guest_resolve(b) == other_fn, "guest_resolve finds both");
    CHECK(guest_resolve(a + 1) == NULL, "unaligned address");
    CHECK(guest_resolve(b + HOST_FUNC_VA_STEP) == NULL, "unregistered address");
    CHECK(guest_resolve(0x00401000u) == NULL, "ordinary address");
    CHECK(import_func_by_name("_real_thunk") == real_fn, "import_func_by_name");
    CHECK(import_func_by_name("_nothing") == NULL, "unknown name");

    g_esp = SLOT_BASE + 0x2000;
    g_esp -= 4;
    guest_resolve(a)();
    CHECK(g_eax == 0x1234 && g_real_calls == 1, "resolved function ran");
}

static void test_bind(const char *path)
{
    data_import_register("_fake_var", 0xABCD0010u);
    data_import_register("_replaced", 1);
    data_import_register("_replaced", 0xABCD0020u);
    uint32_t block = data_import_alloc(20), block2 = data_import_alloc(1);
    CHECK(block % 16 == 0 && block2 == block + 32, "alloc is 16-byte aligned: 0x%X 0x%X", block,
          block2);
    CHECK(get32(block + 16) == 0, "alloc is zeroed");

    const char *names[] = {"_fake_var", "_unregistered", "_real_thunk", NULL,
                           "_placeholder_thunk", "_replaced"};
    memset(guest_ptr(SLOT_BASE), 0, NL_SLOTS * 4);
    int unbound = bind_names(names, 6, path);
    CHECK(unbound == 2, "unbound count %d", unbound);
    CHECK(get32(SLOT_BASE) == 0xABCD0010u, "registered slot 0x%X", get32(SLOT_BASE));
    CHECK(get32(SLOT_BASE + 4) == 0, "unregistered slot stays 0");
    CHECK(get32(SLOT_BASE + 8) == host_func_va(real_fn), "real thunk slot 0x%X", get32(SLOT_BASE + 8));
    CHECK(get32(SLOT_BASE + 12) == 0, "local slot untouched");
    CHECK(get32(SLOT_BASE + 16) == 0, "placeholder thunk stays unbound");
    CHECK(get32(SLOT_BASE + 20) == 0xABCD0020u, "later register wins");
}

static void test_core(const char *path)
{
    data_imports_core_init();
    const char *names[] = {"___stack_chk_guard", "_mach_task_self_", "_bootstrap_port",
                           "__DefaultRuneLocale", "___stdinp", "___stdoutp", "___stderrp"};
    memset(guest_ptr(SLOT_BASE), 0, NL_SLOTS * 4);
    CHECK(bind_names(names, 7, path) == 0, "core names all bound");

    CHECK(get32(get32(SLOT_BASE)) != 0, "stack guard is non-zero");
    CHECK(get32(get32(SLOT_BASE + 4)) != 0, "mach_task_self_ holds a port");
    CHECK(get32(SLOT_BASE + 16) == GUEST_STDINP_ADDR, "stdin slot");
    CHECK(get32(SLOT_BASE + 20) == GUEST_STDOUTP_ADDR, "stdout slot");
    CHECK(get32(SLOT_BASE + 24) == GUEST_STDERRP_ADDR, "stderr slot");

    uint32_t rune = get32(SLOT_BASE + 12);
    for (unsigned c = 0; c < 128; c++) {
        uint32_t type = get32(rune + RUNE_RUNETYPE_OFF + 4 * c);
        CHECK(((type & CTYPE_ALPHA) != 0) == (isalpha((int)c) != 0), "isalpha(%u) via guest table", c);
        CHECK(get32(rune + RUNE_MAPLOWER_OFF + 4 * c) == (uint32_t)tolower((int)c), "tolower(%u)", c);
        CHECK(get32(rune + RUNE_MAPUPPER_OFF + 4 * c) == (uint32_t)toupper((int)c), "toupper(%u)", c);
    }
    CHECK((get32(rune + RUNE_RUNETYPE_OFF + 4 * 'a') & CTYPE_ALPHA) != 0, "isalpha('a')");
    CHECK((get32(rune + RUNE_RUNETYPE_OFF + 4 * '1') & CTYPE_ALPHA) == 0, "!isalpha('1')");
}

/* Bind the real game image: external relocations of symbol ___cxa_pure_virtual
 * (pure-virtual vtable slots) take the thunk address; the CFString isa words
 * (___CFConstantStringClassReference, not registered) stay as they were. */
#define PURE_VIRTUAL_SLOT_FIRST 0x01200944u
#define PURE_VIRTUAL_SLOT_LAST 0x01687308u
#define CFSTRING_ISA_SLOT 0x016876C4u

static int test_real_image(const char *path)
{
    if (guest_mem_init() != 0 || guest_load_image(path) != 0)
        return 1;
    uint32_t first0 = get32(PURE_VIRTUAL_SLOT_FIRST), last0 = get32(PURE_VIRTUAL_SLOT_LAST);
    uint32_t isa0 = get32(CFSTRING_ISA_SLOT);
    data_imports_bind_image(guest_image_header(), path);
    uint32_t thunk = host_func_va(pure_virtual_fn);
    CHECK(get32(PURE_VIRTUAL_SLOT_FIRST) == thunk + first0, "first vtable slot 0x%X",
          get32(PURE_VIRTUAL_SLOT_FIRST));
    CHECK(get32(PURE_VIRTUAL_SLOT_LAST) == thunk + last0, "last vtable slot 0x%X",
          get32(PURE_VIRTUAL_SLOT_LAST));
    CHECK(get32(CFSTRING_ISA_SLOT) == isa0, "unbound extrel slot unchanged");
    if (g_failures) {
        fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--real") == 0)
        return test_real_image(argv[2]);
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch file> | --real <image>\n", argv[0]);
        return 2;
    }
    if (guest_mem_init() != 0)
        return 1;
    if (mprotect(guest_ptr(SLOT_BASE - 0x10000), 0x30000, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 1;
    }
    thunk_files_init();
    test_host_funcs();
    test_bind(argv[1]);
    test_core(argv[1]);
    if (g_failures) {
        fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
