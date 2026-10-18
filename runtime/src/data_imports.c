/* Data imports: fill the image's non-lazy pointer slots. See data_imports.h. */
#include <ctype.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <runetype.h>
#include <servers/bootstrap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "guest_heap.h"
#include "handles.h"
#include "host_funcs.h"
#include "data_imports.h"

/* Defined by the generated import_stubs.c. */
int import_func_is_placeholder(recomp_func_t fn);

/* i386 Darwin _RuneLocale, probed with clang -target i386-apple-macosx10.9
 * and offsetof: __runetype[256] at 52, __maplower[256] at 1076, __mapupper[256]
 * at 2100, sizeof 3164. The pointers in it stay zero. */
#define RUNE_LOCALE_SIZE 3164u
#define RUNE_RUNETYPE_OFF 52u
#define RUNE_MAPLOWER_OFF 1076u
#define RUNE_MAPUPPER_OFF 2100u

#define STORAGE_SIZE 0x10000u

struct reg_entry {
    char *name;
    uint32_t value;
};

static struct reg_entry *g_reg;
static size_t g_reg_count, g_reg_cap;
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t g_storage, g_storage_used;
static const char *g_image_path;

void data_import_register(const char *macho_name, uint32_t slot_value)
{
    pthread_mutex_lock(&g_reg_lock);
    for (size_t i = 0; i < g_reg_count; i++) {
        if (strcmp(g_reg[i].name, macho_name) == 0) {
            g_reg[i].value = slot_value;
            pthread_mutex_unlock(&g_reg_lock);
            return;
        }
    }
    if (g_reg_count == g_reg_cap) {
        g_reg_cap = g_reg_cap ? g_reg_cap * 2 : 64;
        g_reg = realloc(g_reg, g_reg_cap * sizeof *g_reg);
        if (g_reg == NULL)
            abort();
    }
    g_reg[g_reg_count].name = strdup(macho_name);
    g_reg[g_reg_count].value = slot_value;
    g_reg_count++;
    pthread_mutex_unlock(&g_reg_lock);
}

static int lookup_registered(const char *name, uint32_t *value)
{
    int found = 0;
    pthread_mutex_lock(&g_reg_lock);
    for (size_t i = 0; i < g_reg_count && !found; i++) {
        if (strcmp(g_reg[i].name, name) == 0) {
            *value = g_reg[i].value;
            found = 1;
        }
    }
    pthread_mutex_unlock(&g_reg_lock);
    return found;
}

uint32_t data_import_alloc(uint32_t n)
{
    uint32_t need = (n + 15u) & ~15u;
    pthread_mutex_lock(&g_reg_lock);
    if (g_storage == 0) {
        g_storage = guest_calloc(1, STORAGE_SIZE);
        if (g_storage == 0) {
            fprintf(stderr, "data_import_alloc: guest heap exhausted\n");
            abort();
        }
    }
    if (need > STORAGE_SIZE - g_storage_used) {
        fprintf(stderr, "data_import_alloc: %u bytes do not fit in the %u byte block\n",
                n, STORAGE_SIZE);
        abort();
    }
    uint32_t a = g_storage + g_storage_used;
    g_storage_used += need;
    pthread_mutex_unlock(&g_reg_lock);
    return a;
}

static void gwr32(uint32_t a, uint32_t v)
{
    memcpy(guest_ptr(a), &v, 4);
}

void data_imports_core_init(void)
{
    uint32_t guard = data_import_alloc(4), v;
    do {
        v = arc4random();
    } while (v == 0);
    gwr32(guard, v);
    data_import_register("___stack_chk_guard", guard);

    uint32_t task = data_import_alloc(4), boot = data_import_alloc(4);
    gwr32(task, (uint32_t)mach_task_self_);
    gwr32(boot, (uint32_t)bootstrap_port);
    data_import_register("_mach_task_self_", task);
    data_import_register("_bootstrap_port", boot);

    uint32_t rune = data_import_alloc(RUNE_LOCALE_SIZE);
    memcpy(guest_ptr(rune + RUNE_RUNETYPE_OFF), _DefaultRuneLocale.__runetype,
           sizeof _DefaultRuneLocale.__runetype);
    memcpy(guest_ptr(rune + RUNE_MAPLOWER_OFF), _DefaultRuneLocale.__maplower,
           sizeof _DefaultRuneLocale.__maplower);
    memcpy(guest_ptr(rune + RUNE_MAPUPPER_OFF), _DefaultRuneLocale.__mapupper,
           sizeof _DefaultRuneLocale.__mapupper);
    data_import_register("__DefaultRuneLocale", rune);

    data_import_register("___stdinp", GUEST_STDINP_ADDR);
    data_import_register("___stdoutp", GUEST_STDOUTP_ADDR);
    data_import_register("___stderrp", GUEST_STDERRP_ADDR);
}

__attribute__((weak)) void thunk_cf_data_init(void) {}
__attribute__((weak)) void frontend_data_init(void) {}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Section lookup over the load commands in hdr. */
static int find_section(const uint8_t *hdr, const char *name, uint32_t *addr, uint32_t *size,
                        uint32_t *reserved1)
{
    uint32_t ncmds = rd32(hdr + 16), off = 28;
    for (uint32_t i = 0; i < ncmds && off + 8 <= GUEST_HEADER_SIZE; i++) {
        uint32_t cmd = rd32(hdr + off), cmdsize = rd32(hdr + off + 4);
        if (cmd == LC_SEGMENT) {
            uint32_t nsects = rd32(hdr + off + 48);
            const uint8_t *s = hdr + off + 56;
            for (uint32_t k = 0; k < nsects; k++, s += 68) {
                if (strncmp((const char *)s, name, 16) == 0) {
                    *addr = rd32(s + 32);
                    *size = rd32(s + 36);
                    *reserved1 = rd32(s + 60);
                    return 0;
                }
            }
        }
        off += cmdsize;
    }
    return -1;
}

static const uint8_t *find_cmd(const uint8_t *hdr, uint32_t want)
{
    uint32_t ncmds = rd32(hdr + 16), off = 28;
    for (uint32_t i = 0; i < ncmds && off + 8 <= GUEST_HEADER_SIZE; i++) {
        if (rd32(hdr + off) == want)
            return hdr + off;
        off += rd32(hdr + off + 4);
    }
    return NULL;
}

static void *read_file(int fd, uint32_t off, uint32_t n)
{
    void *p = malloc(n ? n : 1);
    if (p == NULL || pread(fd, p, n, off) != (ssize_t)n) {
        fprintf(stderr, "data_imports: read of %u bytes at 0x%X failed\n", n, off);
        exit(1);
    }
    return p;
}

void data_imports_set_image(const char *path)
{
    g_image_path = path;
}

/* The value a slot for the symbol takes: the registered value, else host_func_va
 * of the real import thunk of that name. Returns 0 when there is none. */
static uint32_t resolve_symbol(const char *name)
{
    uint32_t value = 0;
    if (!lookup_registered(name, &value)) {
        recomp_func_t fn = import_func_by_name(name);
        if (fn != NULL && !import_func_is_placeholder(fn))
            value = host_func_va(fn);
    }
    return value;
}

/* True when the word at addr lies inside the __DATA segment of the image. */
static int in_data_segment(const uint8_t *hdr, uint32_t addr)
{
    uint32_t ncmds = rd32(hdr + 16), off = 28;
    for (uint32_t i = 0; i < ncmds && off + 8 <= GUEST_HEADER_SIZE; i++) {
        if (rd32(hdr + off) == LC_SEGMENT &&
            strncmp((const char *)hdr + off + 8, "__DATA", 16) == 0) {
            uint32_t vmaddr = rd32(hdr + off + 24), vmsize = rd32(hdr + off + 28);
            return addr >= vmaddr && addr - vmaddr <= vmsize - 4;
        }
        off += rd32(hdr + off + 4);
    }
    return 0;
}

/* Apply the external relocations of the image (LC_DYSYMTAB extreloff/nextrel).
 * Each one is a 4-byte slot that holds an addend and takes the symbol's value
 * plus that addend. Only GENERIC_RELOC_VANILLA entries (extern, not
 * pc-relative, 4 bytes) occur in the game binary; any other entry stops the
 * load. A slot whose symbol has no value stays as it is. */
static void bind_external_relocs(const uint8_t *hdr, const uint8_t *dy, int fd,
                                 const uint8_t *syms, const char *strs, uint32_t nsyms)
{
    uint32_t nrel = rd32(dy + 68);
    uint8_t *rel = read_file(fd, rd32(dy + 64), nrel * 8);
    uint32_t base = rd32(hdr + 28 + 24); /* vmaddr of the first segment */
    const char *verbose = getenv("RECOMP_STARTUP_VERBOSE");
    unsigned bound = 0, unbound = 0;
    unsigned *per_sym = calloc(nsyms ? nsyms : 1, sizeof *per_sym);
    if (per_sym == NULL)
        abort();
    for (uint32_t i = 0; i < nrel; i++) {
        uint32_t addr = rd32(rel + 8 * i), info = rd32(rel + 8 * i + 4);
        uint32_t symnum = info & 0xFFFFFFu, pcrel = (info >> 24) & 1u;
        uint32_t length = (info >> 25) & 3u, ext = (info >> 27) & 1u, type = info >> 28;
        uint32_t slot = base + addr;
        if (!ext || pcrel || length != 2 || type != 0 || symnum >= nsyms ||
            !in_data_segment(hdr, slot)) {
            fprintf(stderr, "data_imports: unsupported relocation %u: address 0x%08X info 0x%08X\n",
                    i, addr, info);
            exit(1);
        }
        uint32_t value = resolve_symbol(strs + rd32(syms + symnum * 12));
        if (value == 0) {
            unbound++;
            per_sym[symnum]++;
            continue;
        }
        uint32_t addend;
        memcpy(&addend, guest_ptr(slot), 4);
        gwr32(slot, value + addend);
        bound++;
    }
    fprintf(stderr, "data_imports: %u extrel bound, %u unbound\n", bound, unbound);
    if (verbose != NULL && verbose[0] == '1') {
        for (uint32_t k = 0; k < nsyms; k++)
            if (per_sym[k] != 0)
                fprintf(stderr, "data_imports: %s unbound for %u extrel\n",
                        strs + rd32(syms + k * 12), per_sym[k]);
    }
    free(per_sym);
    free(rel);
}

/* Walk the indirect symbol table of the __nl_symbol_ptr and __got sections of
 * the image whose load commands are in hdr and whose file is at path. A slot
 * gets the registered value for its symbol name; failing that, host_func_va of
 * the real import thunk of that name. Then apply the external relocations.
 * Returns the number of unbound indirect slots. */
int data_imports_bind_image(const uint8_t *hdr, const char *path)
{
    const uint8_t *st = find_cmd(hdr, LC_SYMTAB), *dy = find_cmd(hdr, LC_DYSYMTAB);
    if (st == NULL || dy == NULL) {
        fprintf(stderr, "data_imports: no LC_SYMTAB or LC_DYSYMTAB\n");
        exit(1);
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        exit(1);
    }
    uint32_t nsyms = rd32(st + 12), strsize = rd32(st + 20);
    uint32_t nind = rd32(dy + 60);
    uint8_t *syms = read_file(fd, rd32(st + 8), nsyms * 12);
    char *strs = read_file(fd, rd32(st + 16), strsize);
    uint32_t *ind = read_file(fd, rd32(dy + 56), nind * 4);

    const char *verbose = getenv("RECOMP_STARTUP_VERBOSE");
    unsigned bound = 0, unbound = 0;
    static const char *const sects[] = {"__nl_symbol_ptr", "__got"};
    for (unsigned s = 0; s < 2; s++) {
        uint32_t addr, size, first;
        if (find_section(hdr, sects[s], &addr, &size, &first) != 0)
            continue;
        for (uint32_t i = 0; i < size / 4 && first + i < nind; i++) {
            uint32_t idx = ind[first + i];
            if (idx & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS) || idx >= nsyms)
                continue;
            const char *name = strs + rd32(syms + idx * 12);
            uint32_t slot = addr + 4 * i, value = resolve_symbol(name);
            if (value != 0) {
                gwr32(slot, value);
                bound++;
            } else {
                unbound++;
                if (verbose != NULL && verbose[0] == '1')
                    fprintf(stderr, "data_imports: %s at 0x%08X unbound\n", name, slot);
            }
        }
    }
    bind_external_relocs(hdr, dy, fd, syms, strs, nsyms);
    close(fd);
    free(syms);
    free(strs);
    free(ind);
    fprintf(stderr, "data_imports: %u slots bound, %u unbound\n", bound, unbound);
    return (int)unbound;
}

int data_imports_bind_all(void)
{
    data_imports_core_init();
    thunk_cf_data_init();
    frontend_data_init();
    if (g_image_path == NULL) {
        fprintf(stderr, "data_imports: image path not set\n");
        exit(1);
    }
    return data_imports_bind_image(guest_image_header(), g_image_path);
}
