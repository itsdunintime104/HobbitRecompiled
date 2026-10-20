# GL capability flags and the Shader Model 3 alert

Symptom: after the GL context is created ("GL 2.1 Metal - 90.5, Apple M1 Max") the game shows "requires a graphics card which supports Shader Model 3" and exits.

## Evidence (debug round, 2026-10-16)

The alert comes from sub_002A5270. It makes a temporary context and calls the GL probe at 0x31A7F0 (sub_0031A800, PIC base 0x31a80e).

| Address | Meaning |
|---|---|
| 0x2A52FA | checks byte [nl ptr 0x11DA290] = flag 0x1805CEA; 0 jumps to the alert at 0x2A534F |
| 0x2A5305 | checks flag 0x1805D9F; 0 jumps to the alert |
| 0x31a916 | sets flag 0x1805CEA (the GL 2.0 flag) from the major number of glGetString(GL_VERSION), 2 for "2.1" |
| 0x31d28e to 0x31f2e5 | GL 2.0 block: dlsym of every 2.0 entry point, from glAttachShader to glVertexAttribPointer |
| 0x31f2f9 to 0x31f30c | if any dlsym in the block returned NULL (bl = 1), the flag is cleared |
| 0x338D60 | store of flag 0x1805D9F, the FBO-EXT check; it passes since all FBO EXT functions resolve |

At the alert breakpoint the runtime bytes were 0x1805CEA = 0x00, 0x1805D9F = 0x01, 0x1805D2D = 0x01.

Cause: the only GL 2.0 name that resolved to 0 was core `glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)`. It was missing from the thunk tables, so the flag was cleared and the alert fired. The flag depends on core GL 2.0 names, not on the ARB names.

## Thunks (engine/runtime/src/thunk_gl.c)

- `glShaderSource`: reads `count` guest u32 string pointers, builds a host array with guest_ptr per entry, and passes the length array through guest_ptr (32-bit ints in both address spaces) or NULL.
- ARB shader objects: `GLhandleARB` is `void *` on macOS (`<OpenGL/gltypes.h>`), 8 bytes on the host and 4 in the guest. A two-way table hands out a dense counter from 1; 0 is NULL. A host value keeps its guest value for the process lifetime. Covered: glDeleteObjectARB, glGetHandleARB, glDetachObjectARB, glCreateShaderObjectARB, glShaderSourceARB, glCompileShaderARB, glCreateProgramObjectARB, glAttachObjectARB, glLinkProgramARB, glUseProgramObjectARB, glValidateProgramARB, glGetObjectParameterfvARB, glGetObjectParameterivARB, glGetInfoLogARB, glGetAttachedObjectsARB (converts its output array), glGetUniformLocationARB, glGetActiveUniformARB, glGetUniformfvARB, glGetUniformivARB, glGetShaderSourceARB, glBindAttribLocationARB, glGetActiveAttribARB, glGetAttribLocationARB.
- Sync objects: `GLsync` is a host pointer, mapped through a second table with its own counter. Covered: glFenceSync, glIsSync, glDeleteSync, glClientWaitSync and glWaitSync (the 64-bit timeout is two guest u32 slots), glGetSynciv.

The ARB handle namespace and the core GL 2.0 names are separate for the guest: a core `glCreateShader` name is the host name, an ARB handle is a mapped counter value. A guest that mixes the two on one object would break. The game's GL 2.0 path uses the core names.

## Left unresolved

`glGetTexParameterPointervAPPLE`, `glMultiDrawElementsBaseVertex` and `glTransformFeedbackVaryingsEXT` are not in the GL 2.0 block, so they do not affect the flag. They still resolve to 0.
