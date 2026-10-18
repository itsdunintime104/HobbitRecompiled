#ifndef CG_WIRE_H
#define CG_WIRE_H
#include <stdint.h>

/* Wire protocol between runtime/src/thunk_cg.c (arm64, in the game process)
 * and tools/cghelper/cghelper.c (x86_64, under Rosetta). Both ends are little
 * endian. The helper serves requests on fd CGW_FD, one at a time.
 *
 * Request: struct cgw_req, then nstr strings. Each string is a u32 length
 * and that many bytes, with no NUL. Length CGW_NULL means a NULL pointer.
 * Reply: struct cgw_rep, then slen bytes when slen != CGW_NULL.
 *
 * Cg handles cross the wire as u32. The Cg 3.0 runtime returns small integer
 * handles (seen: 0xc, 0x16), and the helper aborts if one does not fit. */

#define CGW_FD 3
#define CGW_NULL 0xFFFFFFFFu
#define CGW_MAX_STR 4096u /* strings per request; cgCreateProgram args */

enum cgw_op {
    CGW_HELLO = 0,                   /* -> value 1 when Cg loaded; string = dlopen error */
    CGW_CREATE_CONTEXT,              /* () -> handle */
    CGW_CREATE_PROGRAM,              /* w0 ctx, w1 type, w2 profile; s: program, entry, args... */
    CGW_DESTROY_PARAMETER,           /* w0 param */
    CGW_DESTROY_PROGRAM,             /* w0 program */
    CGW_GET_ARRAY_PARAMETER,         /* w0 param, w1 index -> handle */
    CGW_GET_ERROR_STRING,            /* w0 error -> string */
    CGW_GET_FIRST_STRUCT_PARAMETER,  /* w0 param -> handle */
    CGW_GET_LAST_LISTING,            /* w0 ctx -> string */
    CGW_GET_NAMED_PARAMETER,         /* w0 program; s: name -> handle */
    CGW_GET_PARAMETER_CLASS,         /* w0 param -> int */
    CGW_GET_PARAMETER_DIRECTION,     /* w0 param -> int */
    CGW_GET_PARAMETER_NAME,          /* w0 param -> string */
    CGW_GET_PARAMETER_RESOURCE,      /* w0 param -> int */
    CGW_GET_PARAMETER_RESOURCE_INDEX,/* w0 param -> unsigned long, truncated to u32 */
    CGW_GET_PARAMETER_SEMANTIC,      /* w0 param -> string */
    CGW_GET_PROGRAM_STRING,          /* w0 program, w1 enum -> string */
    CGW_GET_STRING,                  /* w0 enum -> string */
    CGW_IS_PARAMETER,                /* w0 param -> bool */
    CGW_IS_PARAMETER_REFERENCED,     /* w0 param -> bool */
    CGW_IS_PARAMETER_USED,           /* w0 param, w1 handle -> bool */
    CGW_OP_COUNT
};

struct cgw_req {
    uint32_t op;
    uint32_t w[3];
    uint32_t nstr;
};

struct cgw_rep {
    uint32_t value; /* handle, int or bool; for string ops 1 when non-NULL */
    uint32_t slen;  /* CGW_NULL when no string */
};

#endif
