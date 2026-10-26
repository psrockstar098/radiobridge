/*
 * RadioBridge — rb_thread.c
 *
 * Portable thread/mutex/once/sleep/clock implementations behind
 * rb_thread.h. Windows uses the Win32 API directly (no pthreads
 * dependency); POSIX uses pthreads.
 */
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <stdlib.h>
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

#include "core/rb_thread.h"

#ifdef _WIN32

int rb_mutex_init(rb_mutex_t *m) {
    InitializeCriticalSection(&m->cs);
    return 0;
}

void rb_mutex_destroy(rb_mutex_t *m) { DeleteCriticalSection(&m->cs); }

void rb_mutex_lock(rb_mutex_t *m) { EnterCriticalSection(&m->cs); }

void rb_mutex_unlock(rb_mutex_t *m) { LeaveCriticalSection(&m->cs); }

typedef struct {
    void *(*fn)(void *);
    void *arg;
} rb_thread_param_t;

/* _beginthreadex (not raw CreateThread) so the C runtime sets up
 * per-thread state for threads that call malloc/stdio. */
static unsigned __stdcall rb_thread_thunk(void *param) {
    rb_thread_param_t *p = (rb_thread_param_t *)param;
    void *(*fn)(void *) = p->fn;
    void *arg = p->arg;
    free(p);
    fn(arg);
    return 0;
}

int rb_thread_create(rb_thread_t *th, void *(*fn)(void *), void *arg) {
    rb_thread_param_t *p =
        (rb_thread_param_t *)malloc(sizeof(*p));
    if (p == NULL) return -1;
    p->fn = fn;
    p->arg = arg;
    unsigned id = 0;
    uintptr_t h = _beginthreadex(NULL, 0, rb_thread_thunk, p, 0, &id);
    if (h == 0) {
        free(p);
        return -1;
    }
    th->handle = (HANDLE)h;
    th->id = id;
    return 0;
}

int rb_thread_join(rb_thread_t th) {
    if (th.handle == NULL) return -1;
    WaitForSingleObject(th.handle, INFINITE);
    CloseHandle(th.handle);
    return 0;
}

int rb_thread_detach(rb_thread_t th) {
    if (th.handle == NULL) return -1;
    CloseHandle(th.handle);
    return 0;
}

rb_thread_t rb_thread_self(void) {
    rb_thread_t t;
    t.handle = GetCurrentThread();
    t.id = GetCurrentThreadId();
    return t;
}

int rb_thread_equal(rb_thread_t a, rb_thread_t b) {
    return a.id == b.id;
}

typedef struct {
    void (*fn)(void);
} rb_once_param_t;

static BOOL CALLBACK rb_once_cb(PINIT_ONCE once, PVOID param,
                                PVOID *ctx) {
    (void)once;
    (void)ctx;
    ((rb_once_param_t *)param)->fn();
    return TRUE;
}

void rb_once(rb_once_t *once, void (*fn)(void)) {
    rb_once_param_t p;
    p.fn = fn;
    InitOnceExecuteOnce(&once->once, rb_once_cb, &p, NULL);
}

void rb_sleep_ms(unsigned int ms) { Sleep(ms); }

uint64_t rb_now_ms(void) { return (uint64_t)GetTickCount64(); }

int rb_path_exists(const char *path) { return _access(path, 0) == 0; }

#else /* POSIX */

int rb_mutex_init(rb_mutex_t *m) { return pthread_mutex_init(m, NULL); }

void rb_mutex_destroy(rb_mutex_t *m) { pthread_mutex_destroy(m); }

void rb_mutex_lock(rb_mutex_t *m) { pthread_mutex_lock(m); }

void rb_mutex_unlock(rb_mutex_t *m) { pthread_mutex_unlock(m); }

int rb_thread_create(rb_thread_t *th, void *(*fn)(void *), void *arg) {
    return pthread_create(th, NULL, fn, arg);
}

int rb_thread_join(rb_thread_t th) { return pthread_join(th, NULL); }

int rb_thread_detach(rb_thread_t th) { return pthread_detach(th); }

rb_thread_t rb_thread_self(void) { return pthread_self(); }

int rb_thread_equal(rb_thread_t a, rb_thread_t b) {
    return pthread_equal(a, b);
}

void rb_once(rb_once_t *once, void (*fn)(void)) {
    pthread_once(once, fn);
}

void rb_sleep_ms(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

uint64_t rb_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u +
           (uint64_t)(ts.tv_nsec / 1000000L);
}

int rb_path_exists(const char *path) { return access(path, F_OK) == 0; }

#endif /* _WIN32 */
