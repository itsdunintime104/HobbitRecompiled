#ifndef GL_BIND_H
#define GL_BIND_H

#include <stdint.h>

/* Buffer binding slots cached by glBindBuffer in thunk_gl.c. */
enum { GL_BIND_ARRAY, GL_BIND_ELEMENT, GL_BIND_PACK, GL_BIND_UNPACK, GL_BIND_COUNT };

/* Pointer argument of a GL call: a byte offset while a buffer is bound to the
 * slot's target, otherwise a guest address (0 is NULL). */
const void *gl_buf_ptr(int slot, uint32_t value);

#endif
