#ifndef THUNK_GL_H
#define THUNK_GL_H

#include "recomp_types.h"

/* The game loads GL extension entry points with dlopen("OpenGL") + dlsym.
 * Returns the thunk for a GL entry point that thunk_gl.c implements by hand
 * (name without the Mach-O underscore, for example "glBindBufferARB"), or
 * NULL when the name is not in the table. */
recomp_func_t gl_ext_lookup(const char *name);

/* Thunk from the generated thunk_gl_ext.c (tools/hobbit/emit_gl_ext.py) for a
 * GL name that the host exports and the game looks up. NULL when there is
 * none. gl_ext_lookup() falls back to it. */
recomp_func_t gl_ext_auto_lookup(const char *name);

#endif
