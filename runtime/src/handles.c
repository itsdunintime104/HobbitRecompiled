/* Handle table: maps 32-bit guest handles to host objects. */
#include <pthread.h>
#include <stddef.h>

#include "handles.h"

static struct {
    void *host;
    enum handle_kind kind;
} g_table[HANDLE_MAX];

static uint32_t g_next;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Index of h in the table, or HANDLE_MAX if h is not a handle value. */
static uint32_t index_of(uint32_t h)
{
    uint32_t idx = h - HANDLE_BASE;
    return idx < HANDLE_MAX ? idx : HANDLE_MAX;
}

uint32_t handle_put(void *host, enum handle_kind kind)
{
    uint32_t result = 0;

    if (host == NULL || kind == HANDLE_NONE)
        return 0;
    pthread_mutex_lock(&g_lock);
    for (uint32_t n = 0; n < HANDLE_MAX; n++) {
        uint32_t idx = (g_next + n) % HANDLE_MAX;
        if (g_table[idx].kind == HANDLE_NONE) {
            g_table[idx].host = host;
            g_table[idx].kind = kind;
            g_next = idx + 1;
            result = HANDLE_BASE + idx;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

void *handle_get(uint32_t h, enum handle_kind kind)
{
    uint32_t idx = index_of(h);
    void *host = NULL;

    if (idx == HANDLE_MAX)
        return NULL;
    pthread_mutex_lock(&g_lock);
    if (g_table[idx].kind == kind)
        host = g_table[idx].host;
    pthread_mutex_unlock(&g_lock);
    return host;
}

void *handle_release(uint32_t h, enum handle_kind kind)
{
    uint32_t idx = index_of(h);
    void *host = NULL;

    if (idx == HANDLE_MAX)
        return NULL;
    pthread_mutex_lock(&g_lock);
    if (g_table[idx].kind == kind) {
        host = g_table[idx].host;
        g_table[idx].host = NULL;
        g_table[idx].kind = HANDLE_NONE;
    }
    pthread_mutex_unlock(&g_lock);
    return host;
}
