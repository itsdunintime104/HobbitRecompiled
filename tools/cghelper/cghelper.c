/* cghelper: serves Cg runtime calls for the arm64 game over a socket.
 *
 * Built x86_64 and run under Rosetta, because the game bundle's Cg.framework
 * has only i386 and x86_64 slices. runtime/src/thunk_cg.c starts it with the
 * socket on fd CGW_FD. The framework is loaded with dlopen from
 * $HOBBIT_APP/Contents/Frameworks/Cg.framework/Versions/A/Cg; its install
 * name is @executable_path-relative, so it cannot be linked by path.
 *
 * Build: clang -std=gnu11 -O1 -arch x86_64 -o <out>/cghelper tools/cghelper/cghelper.c */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cg_wire.h"

/* Default bundle, relative to $HOME, when HOBBIT_APP is unset. */
#define DEFAULT_APP_REL \
    "/Library/Application Support/Steam/steamapps/common/LEGO - The Hobbit/LEGO The Hobbit.app"
#define CG_SUBPATH "/Contents/Frameworks/Cg.framework/Versions/A/Cg"

typedef void *H; /* CGcontext, CGprogram, CGparameter, CGhandle */

static H (*p_cgCreateContext)(void);
static H (*p_cgCreateProgram)(H, int, const char *, int, const char *, const char **);
static void (*p_cgDestroyParameter)(H);
static void (*p_cgDestroyProgram)(H);
static H (*p_cgGetArrayParameter)(H, int);
static const char *(*p_cgGetErrorString)(int);
static H (*p_cgGetFirstStructParameter)(H);
static const char *(*p_cgGetLastListing)(H);
static H (*p_cgGetNamedParameter)(H, const char *);
static int (*p_cgGetParameterClass)(H);
static int (*p_cgGetParameterDirection)(H);
static const char *(*p_cgGetParameterName)(H);
static int (*p_cgGetParameterResource)(H);
static unsigned long (*p_cgGetParameterResourceIndex)(H);
static const char *(*p_cgGetParameterSemantic)(H);
static const char *(*p_cgGetProgramString)(H, int);
static const char *(*p_cgGetString)(int);
static int (*p_cgIsParameter)(H);
static int (*p_cgIsParameterReferenced)(H);
static int (*p_cgIsParameterUsed)(H, H);

static char g_load_error[1024];

static int load_cg(void)
{
    const char *app = getenv("HOBBIT_APP");
    char path[2048];
    const char *home = getenv("HOME");
    if (app && *app)
        snprintf(path, sizeof path, "%s" CG_SUBPATH, app);
    else
        snprintf(path, sizeof path, "%s" DEFAULT_APP_REL CG_SUBPATH, home ? home : "");
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        snprintf(g_load_error, sizeof g_load_error, "dlopen %s: %s", path, dlerror());
        return -1;
    }
#define SYM(n) \
    if (!(*(void **)&p_##n = dlsym(lib, #n))) { \
        snprintf(g_load_error, sizeof g_load_error, "dlsym %s missing", #n); \
        return -1; \
    }
    SYM(cgCreateContext) SYM(cgCreateProgram) SYM(cgDestroyParameter) SYM(cgDestroyProgram)
    SYM(cgGetArrayParameter) SYM(cgGetErrorString) SYM(cgGetFirstStructParameter)
    SYM(cgGetLastListing) SYM(cgGetNamedParameter) SYM(cgGetParameterClass)
    SYM(cgGetParameterDirection) SYM(cgGetParameterName) SYM(cgGetParameterResource)
    SYM(cgGetParameterResourceIndex) SYM(cgGetParameterSemantic) SYM(cgGetProgramString)
    SYM(cgGetString) SYM(cgIsParameter) SYM(cgIsParameterReferenced) SYM(cgIsParameterUsed)
#undef SYM
    return 0;
}

static int read_full(void *buf, size_t n)
{
    for (uint8_t *p = buf; n;) {
        ssize_t r = read(CGW_FD, p, n);
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_full(const void *buf, size_t n)
{
    for (const uint8_t *p = buf; n;) {
        ssize_t r = write(CGW_FD, p, n);
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static H in_h(uint32_t w) { return (H)(uintptr_t)w; }

/* Cg handles must fit the guest's 32-bit slot; see cg_wire.h. */
static uint32_t out_h(H h)
{
    uintptr_t v = (uintptr_t)h;
    if (v >> 32) {
        fprintf(stderr, "cghelper: Cg handle %p does not fit 32 bits\n", h);
        abort();
    }
    return (uint32_t)v;
}

int main(void)
{
    int loaded = load_cg() == 0;
    for (;;) {
        struct cgw_req q;
        if (read_full(&q, sizeof q) != 0)
            return 0; /* the game closed the socket */
        if (q.nstr > CGW_MAX_STR)
            return 1;
        char *s[CGW_MAX_STR + 1];
        for (uint32_t i = 0; i < q.nstr; i++) {
            uint32_t len;
            if (read_full(&len, 4) != 0)
                return 1;
            s[i] = NULL;
            if (len == CGW_NULL)
                continue;
            s[i] = malloc((size_t)len + 1);
            if (!s[i] || read_full(s[i], len) != 0)
                return 1;
            s[i][len] = 0;
        }
        s[q.nstr] = NULL;

        struct cgw_rep r = {0, CGW_NULL};
        const char *str = NULL;
        int is_str = 0;
        if (!loaded && q.op != CGW_HELLO) {
            r.value = 0;
        } else {
            switch (q.op) {
            case CGW_HELLO: r.value = (uint32_t)loaded; str = loaded ? NULL : g_load_error; is_str = 1; break;
            case CGW_CREATE_CONTEXT: r.value = out_h(p_cgCreateContext()); break;
            case CGW_CREATE_PROGRAM:
                r.value = out_h(p_cgCreateProgram(in_h(q.w[0]), (int)q.w[1], q.nstr > 0 ? s[0] : NULL,
                                                  (int)q.w[2], q.nstr > 1 ? s[1] : NULL,
                                                  q.nstr > 2 ? (const char **)&s[2] : NULL));
                break;
            case CGW_DESTROY_PARAMETER: p_cgDestroyParameter(in_h(q.w[0])); break;
            case CGW_DESTROY_PROGRAM: p_cgDestroyProgram(in_h(q.w[0])); break;
            case CGW_GET_ARRAY_PARAMETER: r.value = out_h(p_cgGetArrayParameter(in_h(q.w[0]), (int)q.w[1])); break;
            case CGW_GET_ERROR_STRING: str = p_cgGetErrorString((int)q.w[0]); is_str = 1; break;
            case CGW_GET_FIRST_STRUCT_PARAMETER: r.value = out_h(p_cgGetFirstStructParameter(in_h(q.w[0]))); break;
            case CGW_GET_LAST_LISTING: str = p_cgGetLastListing(in_h(q.w[0])); is_str = 1; break;
            case CGW_GET_NAMED_PARAMETER:
                r.value = out_h(p_cgGetNamedParameter(in_h(q.w[0]), q.nstr > 0 ? s[0] : NULL));
                break;
            case CGW_GET_PARAMETER_CLASS: r.value = (uint32_t)p_cgGetParameterClass(in_h(q.w[0])); break;
            case CGW_GET_PARAMETER_DIRECTION: r.value = (uint32_t)p_cgGetParameterDirection(in_h(q.w[0])); break;
            case CGW_GET_PARAMETER_NAME: str = p_cgGetParameterName(in_h(q.w[0])); is_str = 1; break;
            case CGW_GET_PARAMETER_RESOURCE: r.value = (uint32_t)p_cgGetParameterResource(in_h(q.w[0])); break;
            case CGW_GET_PARAMETER_RESOURCE_INDEX:
                r.value = (uint32_t)p_cgGetParameterResourceIndex(in_h(q.w[0]));
                break;
            case CGW_GET_PARAMETER_SEMANTIC: str = p_cgGetParameterSemantic(in_h(q.w[0])); is_str = 1; break;
            case CGW_GET_PROGRAM_STRING: str = p_cgGetProgramString(in_h(q.w[0]), (int)q.w[1]); is_str = 1; break;
            case CGW_GET_STRING: str = p_cgGetString((int)q.w[0]); is_str = 1; break;
            case CGW_IS_PARAMETER: r.value = (uint32_t)p_cgIsParameter(in_h(q.w[0])); break;
            case CGW_IS_PARAMETER_REFERENCED: r.value = (uint32_t)p_cgIsParameterReferenced(in_h(q.w[0])); break;
            case CGW_IS_PARAMETER_USED:
                r.value = (uint32_t)p_cgIsParameterUsed(in_h(q.w[0]), in_h(q.w[1]));
                break;
            default: return 1;
            }
        }
        if (is_str && str) {
            r.slen = (uint32_t)strlen(str);
            if (q.op != CGW_HELLO)
                r.value = 1;
        }
        for (uint32_t i = 0; i < q.nstr; i++)
            free(s[i]);
        if (write_full(&r, sizeof r) != 0 || (r.slen != CGW_NULL && write_full(str, r.slen) != 0))
            return 1;
    }
}
