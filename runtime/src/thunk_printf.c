/* Variadic import thunks: printf family, sscanf and fcntl.
 *
 * The guest is i386 cdecl, so every variadic argument sits on the guest stack
 * after the fixed ones. A small walker reads them with their i386 sizes and
 * formats or parses one conversion at a time with the host libc. */
#define RECOMP_GENERATED_CODE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "diag.h"
#include "guest_mem.h"
#include "handles.h"
#include "recomp_types.h"

/* Fixed cdecl argument n of the thunk being called. */
#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

typedef struct {
    char *p;
    size_t len;
    size_t cap;
} Buf;

static int buf_reserve(Buf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return 0;
    size_t cap = b->cap ? b->cap : 128;
    while (cap < b->len + extra + 1)
        cap *= 2;
    char *np = realloc(b->p, cap);
    if (!np)
        return -1;
    b->p = np;
    b->cap = cap;
    return 0;
}

static int buf_append(Buf *b, const char *s, size_t n)
{
    if (buf_reserve(b, n) != 0)
        return -1;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

/* Format one conversion with the host snprintf and append it. */
static int buf_emit(Buf *b, const char *spec, ...)
{
    va_list ap, ap2;
    va_start(ap, spec);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, spec, ap);
    va_end(ap);
    int rc = -1;
    if (n >= 0 && buf_reserve(b, (size_t)n) == 0) {
        vsnprintf(b->p + b->len, (size_t)n + 1, spec, ap2);
        b->len += (size_t)n;
        rc = 0;
    }
    va_end(ap2);
    return rc;
}

typedef struct {
    uint32_t next; /* guest address of the next variadic argument */
} ArgCursor;

static uint32_t arg32(ArgCursor *c)
{
    uint32_t v = MEM32(c->next);
    c->next += 4;
    return v;
}

static uint64_t arg64(ArgCursor *c)
{
    uint64_t lo = MEM32(c->next);
    uint64_t hi = MEM32(c->next + 4);
    c->next += 8;
    return lo | (hi << 32);
}

enum len_mod { L_NONE, L_HH, L_H, L_L, L_LL, L_J, L_Z, L_T, L_BIGL };

/* Parse an optional length modifier at *pp. */
static enum len_mod parse_len(const char **pp)
{
    const char *p = *pp;
    enum len_mod lm = L_NONE;
    switch (*p) {
    case 'h':
        p++;
        lm = L_H;
        if (*p == 'h') {
            p++;
            lm = L_HH;
        }
        break;
    case 'l':
        p++;
        lm = L_L;
        if (*p == 'l') {
            p++;
            lm = L_LL;
        }
        break;
    case 'q':
        p++;
        lm = L_LL;
        break;
    case 'j':
        p++;
        lm = L_J;
        break;
    case 'z':
        p++;
        lm = L_Z;
        break;
    case 't':
        p++;
        lm = L_T;
        break;
    case 'L':
        p++;
        lm = L_BIGL;
        break;
    default:
        break;
    }
    *pp = p;
    return lm;
}

/* Parse "[flags][width][.prec][len]conv" starting after the '%' at *pp.
 * "%" plus flags, width and precision go to spec, with '*' replaced by the
 * argument it reads. Returns the conversion character, or 0 at the end. */
static int parse_spec(const char **pp, ArgCursor *c, char *spec, size_t cap, enum len_mod *lm)
{
    const char *p = *pp;
    size_t n = 0;
    spec[n++] = '%';
    while (*p && strchr("-+ #0'", *p) && n < cap - 16)
        spec[n++] = *p++;
    if (*p == '*') {
        int w = (int)arg32(c);
        p++;
        if (w < 0) {
            spec[n++] = '-';
            w = -w;
        }
        n += (size_t)snprintf(spec + n, cap - n, "%d", w);
    } else {
        while (*p >= '0' && *p <= '9' && n < cap - 16)
            spec[n++] = *p++;
    }
    if (*p == '.') {
        spec[n++] = *p++;
        if (*p == '*') {
            int pr = (int)arg32(c);
            p++;
            if (pr < 0)
                n--; /* negative precision means none */
            else
                n += (size_t)snprintf(spec + n, cap - n, "%d", pr);
        } else {
            while (*p >= '0' && *p <= '9' && n < cap - 16)
                spec[n++] = *p++;
        }
    }
    spec[n] = '\0';
    *lm = parse_len(&p);
    int conv = (unsigned char)*p;
    if (conv)
        p++;
    *pp = p;
    return conv;
}

/* Store the running output length through a %n guest pointer. */
static void store_count(uint32_t g, enum len_mod lm, size_t count)
{
    void *dst = guest_ptr(g);
    if (lm == L_LL || lm == L_J) {
        long long v = (long long)count;
        memcpy(dst, &v, sizeof v);
    } else if (lm == L_HH) {
        int8_t v = (int8_t)count;
        memcpy(dst, &v, sizeof v);
    } else if (lm == L_H) {
        int16_t v = (int16_t)count;
        memcpy(dst, &v, sizeof v);
    } else {
        int32_t v = (int32_t)count;
        memcpy(dst, &v, sizeof v);
    }
}

/* Run the printf format fmt against the guest arguments at guest address
 * argp. The text is appended to out. Returns 0, or -1 on allocation failure. */
static int guest_format(const char *fmt, uint32_t argp, Buf *out)
{
    ArgCursor cur = {argp};
    const char *p = fmt;
    while (*p) {
        const char *lit = p;
        while (*p && *p != '%')
            p++;
        if (p > lit && buf_append(out, lit, (size_t)(p - lit)) != 0)
            return -1;
        if (!*p)
            break;
        const char *start = p++;
        if (*p == '%') {
            p++;
            if (buf_append(out, "%", 1) != 0)
                return -1;
            continue;
        }
        char spec[64];
        enum len_mod lm;
        int conv = parse_spec(&p, &cur, spec, sizeof spec, &lm);
        size_t sl = strlen(spec);
        int rc = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            if (lm == L_LL || lm == L_J) {
                snprintf(spec + sl, sizeof spec - sl, "ll%c", conv);
                rc = buf_emit(out, spec, (long long)arg64(&cur));
            } else {
                const char *m = lm == L_HH ? "hh" : lm == L_H ? "h" : "";
                snprintf(spec + sl, sizeof spec - sl, "%s%c", m, conv);
                rc = buf_emit(out, spec, (int)arg32(&cur));
            }
            break;
        case 'c':
            if (lm == L_L) {
                snprintf(spec + sl, sizeof spec - sl, "lc");
                rc = buf_emit(out, spec, (wint_t)arg32(&cur));
            } else {
                snprintf(spec + sl, sizeof spec - sl, "c");
                rc = buf_emit(out, spec, (int)arg32(&cur));
            }
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            if (lm == L_BIGL) { /* 12-byte x87 long double: not supported */
                THUNK_UNSUPPORTED("printf conversion '%c'", conv);
                rc = buf_append(out, start, (size_t)(p - start));
                break;
            }
            snprintf(spec + sl, sizeof spec - sl, "%c", conv);
            uint64_t bits = arg64(&cur);
            double d;
            memcpy(&d, &bits, sizeof d);
            rc = buf_emit(out, spec, d);
            break;
        }
        case 's':
            snprintf(spec + sl, sizeof spec - sl, "%ss", lm == L_L ? "l" : "");
            rc = buf_emit(out, spec, (const char *)guest_ptr(arg32(&cur)));
            break;
        case 'p':
            snprintf(spec + sl, sizeof spec - sl, "p");
            rc = buf_emit(out, spec, (void *)(uintptr_t)arg32(&cur));
            break;
        case 'n':
            store_count(arg32(&cur), lm, out->len);
            break;
        default: /* unknown or truncated conversion: emit it as written */
            if (conv != 0)
                THUNK_UNSUPPORTED("printf conversion '%c'", conv);
            rc = buf_append(out, start, (size_t)(p - start));
            break;
        }
        if (rc != 0)
            return -1;
    }
    return 0;
}

/* Format the guest printf string fmt_guest with the i386 argument area at
 * args_guest (a guest va_list). Writes at most cap - 1 bytes plus a NUL to
 * out, as snprintf does; cap 0 writes nothing. Returns the full length, or
 * (uint32_t)-1 on allocation failure. Used by vsnprintf and vsprintf. */
uint32_t printf_format_from(uint32_t fmt_guest, uint32_t args_guest, char *out, size_t cap)
{
    Buf b = {0};
    if (guest_format(guest_ptr(fmt_guest), args_guest, &b) != 0) {
        free(b.p);
        return (uint32_t)-1;
    }
    if (cap) {
        size_t n = b.len < cap - 1 ? b.len : cap - 1;
        if (n)
            memcpy(out, b.p, n);
        out[n] = '\0';
    }
    uint32_t len = (uint32_t)b.len;
    free(b.p);
    return len;
}

static void finish(uint32_t result)
{
    g_eax = result;
    g_esp += 4;
}

void imp__printf(void)
{
    Buf b = {0};
    if (guest_format(guest_ptr(ARG(0)), g_esp + 8, &b) != 0) {
        free(b.p);
        finish((uint32_t)-1);
        return;
    }
    if (b.len)
        fwrite(b.p, 1, b.len, stdout);
    free(b.p);
    finish((uint32_t)(int)b.len);
}

void imp__fprintf(void)
{
    FILE *f = handle_get(ARG(0), HANDLE_FILE);
    Buf b = {0};
    if (!f || guest_format(guest_ptr(ARG(1)), g_esp + 12, &b) != 0) {
        free(b.p);
        finish((uint32_t)-1);
        return;
    }
    if (b.len)
        fwrite(b.p, 1, b.len, f);
    free(b.p);
    finish((uint32_t)(int)b.len);
}

void imp__sprintf(void)
{
    Buf b = {0};
    if (guest_format(guest_ptr(ARG(1)), g_esp + 12, &b) != 0) {
        free(b.p);
        finish((uint32_t)-1);
        return;
    }
    char *dst = guest_ptr(ARG(0));
    if (b.len)
        memcpy(dst, b.p, b.len);
    dst[b.len] = '\0';
    free(b.p);
    finish((uint32_t)(int)b.len);
}

void imp__snprintf(void)
{
    uint32_t size = ARG(1);
    Buf b = {0};
    if (guest_format(guest_ptr(ARG(2)), g_esp + 16, &b) != 0) {
        free(b.p);
        finish((uint32_t)-1);
        return;
    }
    if (size) {
        char *dst = guest_ptr(ARG(0));
        size_t n = b.len < size - 1 ? b.len : size - 1;
        if (n)
            memcpy(dst, b.p, n);
        dst[n] = '\0';
    }
    free(b.p);
    finish((uint32_t)(int)b.len);
}

/* ---- sscanf ---- */

static int is_space(int ch)
{
    return ch == ' ' || (ch >= '\t' && ch <= '\r');
}

/* Storage for one scanned value. The host sscanf writes it; it is then copied
 * to the guest at the guest width. */
typedef union {
    long long i64;
    int i32;
    short i16;
    signed char i8;
    float f;
    double d;
    void *ptr;
} ScanVal;

enum scan_kind { T_INT, T_FLOAT, T_PTR32, T_STR };

void imp__sscanf(void)
{
    const char *in = guest_ptr(ARG(0));
    const char *p = guest_ptr(ARG(1));
    ArgCursor cur = {g_esp + 12};
    const char *ip = in;
    int assigned = 0;
    int input_failure = 0; /* input ended before a conversion could start */

    while (*p) {
        if (is_space(*p)) {
            while (is_space(*p))
                p++;
            while (is_space(*ip))
                ip++;
            continue;
        }
        if (*p != '%') {
            if (*ip != *p) {
                input_failure = *ip == '\0';
                break;
            }
            p++;
            ip++;
            continue;
        }
        p++;
        if (*p == '%') {
            while (is_space(*ip))
                ip++;
            if (*ip != '%') {
                input_failure = *ip == '\0';
                break;
            }
            p++;
            ip++;
            continue;
        }

        char spec[64];
        size_t n = 0;
        spec[n++] = '%';
        int suppress = 0;
        if (*p == '*') {
            suppress = 1;
            spec[n++] = *p++;
        }
        while (*p >= '0' && *p <= '9' && n < sizeof spec - 24)
            spec[n++] = *p++;
        spec[n] = '\0';
        enum len_mod lm = parse_len(&p);
        int conv = (unsigned char)*p;
        if (!conv)
            break;
        p++;

        if (conv == 'n') {
            if (!suppress)
                store_count(arg32(&cur), lm, (size_t)(ip - in));
            continue;
        }

        /* A scanset is copied through to the closing ']'. A leading ']' or
         * '^]' belongs to the set. */
        if (conv == '[') {
            if (lm == L_L) { /* wide scanset: not supported */
                THUNK_UNSUPPORTED("sscanf scanset");
                break;
            }
            spec[n++] = '[';
            if (*p == '^')
                spec[n++] = *p++;
            if (*p == ']')
                spec[n++] = *p++;
            while (*p && *p != ']' && n < sizeof spec - 24)
                spec[n++] = *p++;
            if (*p != ']') {
                THUNK_UNSUPPORTED("sscanf scanset");
                break;
            }
            p++;
            spec[n++] = ']';
            spec[n] = '\0';
        }

        enum scan_kind kind = T_STR;
        size_t gsize = 4;
        const char *host_len = "";
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            kind = T_INT;
            if (lm == L_LL || lm == L_J) {
                host_len = "ll";
                gsize = 8;
            } else if (lm == L_H) {
                host_len = "h";
                gsize = 2;
            } else if (lm == L_HH) {
                host_len = "hh";
                gsize = 1;
            }
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            kind = T_FLOAT;
            if (lm == L_L) {
                host_len = "l";
                gsize = 8;
            }
            break;
        case 'p':
            kind = T_PTR32;
            break;
        case 's': case 'c': case '[':
            kind = T_STR;
            break;
        default: /* unsupported conversion: stop scanning */
            THUNK_UNSUPPORTED("sscanf conversion '%c'", conv);
            goto done;
        }

        /* The trailing %n reports how many bytes this conversion consumed. */
        char full[96];
        if (conv == '[')
            snprintf(full, sizeof full, "%s%%n", spec);
        else
            snprintf(full, sizeof full, "%s%s%c%%n", spec, host_len, conv);

        int consumed = -1;
        int rc;
        ScanVal v;
        memset(&v, 0, sizeof v);
        uint32_t gaddr = 0;
        if (suppress) {
            rc = sscanf(ip, full, &consumed);
        } else {
            gaddr = arg32(&cur);
            if (kind == T_STR)
                rc = sscanf(ip, full, (char *)guest_ptr(gaddr), &consumed);
            else
                rc = sscanf(ip, full, &v, &consumed);
        }
        if (consumed < 0) {
            input_failure = rc == EOF;
            break;
        }
        ip += consumed;
        if (!suppress) {
            if (kind == T_PTR32) {
                uint32_t w = (uint32_t)(uintptr_t)v.ptr;
                memcpy(guest_ptr(gaddr), &w, sizeof w);
            } else if (kind != T_STR) {
                memcpy(guest_ptr(gaddr), &v, gsize);
            }
            assigned++;
        }
    }
done:
    finish((uint32_t)((assigned == 0 && input_failure) ? EOF : assigned));
}

/* ---- fcntl ---- */

void imp__fcntl_UNIX2003(void)
{
    int fd = (int)ARG(0);
    int cmd = (int)ARG(1);
    int rc;
    switch (cmd) {
    case F_GETFL:
    case F_GETFD:
        rc = fcntl(fd, cmd);
        break;
    case F_SETFL:
    case F_SETFD:
        rc = fcntl(fd, cmd, (int)ARG(2));
        break;
    default:
        guest_set_errno(EINVAL);
        THUNK_UNSUPPORTED("fcntl command %d", cmd);
        rc = -1;
        break;
    }
    finish((uint32_t)rc);
}
