#ifndef SLOWFRAME_H
#define SLOWFRAME_H

/* Call once per frame on the main thread, at the frame boundary. Does nothing
 * unless RECOMP_SLOWFRAME_MS is set. */
void slowframe_mark(void);

#endif
