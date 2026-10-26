/*
 * RadioBridge — rb_thread.h
 *
 * Portable threads, mutexes, call-once, sleep, monotonic clock, and
 * path probing. This is the ONLY place in the codebase that knows
 * about pthreads or the Win32 thread API:
 *
 *  - POSIX (Linux, macOS): pthreads + nanosleep + clock_gettime.
 *  - Windows (MSVC): CRITICAL_SECTION, _beginthreadex threads,
 *    InitOnceExecuteOnce, Sleep, GetTickCount64, _access.
 *
 * Semantics match the pthread calls they replace:
 *  - rb_thread_create returns 0 on success, nonzero on failure.
 *  - rb_thread_join / rb_thread_detach take the handle by value.
 *  - rb_thread_equal compares thread identity (thread id on Windows,
 *    because a Win32 pseudo-handle must never be compared with a real
 *    thread handle).
 *  - A zeroed rb_thread_t is invalid (RB_THREAD_INVALID); code must
 *    never join or detach an invalid handle.
 */
#ifndef RADIOBRIDGE_RB_THREAD_H
#define RADIOBRIDGE_RB_THREAD_H

#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
typedef struct {
    CRITICAL_SECTION cs;
} rb_mutex_t;
typedef struct {
    HANDLE handle;
    DWORD id;
} rb_thread_t;
typedef struct {
    INIT_ONCE once;
} rb_once_t;
#define RB_ONCE_INIT \
    { \
        INIT_ONCE_STATIC_INIT \
    }
#define RB_THREAD_INVALID \
    { \
        NULL, 0 \
    }
#else
#include <pthread.h>
typedef pthread_mutex_t rb_mutex_t;
typedef pthread_t rb_thread_t;
typedef pthread_once_t rb_once_t;
#define RB_ONCE_INIT PTHREAD_ONCE_INIT
#define RB_THREAD_INVALID 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

int rb_mutex_init(rb_mutex_t *m);
void rb_mutex_destroy(rb_mutex_t *m);
void rb_mutex_lock(rb_mutex_t *m);
void rb_mutex_unlock(rb_mutex_t *m);

int rb_thread_create(rb_thread_t *th, void *(*fn)(void *), void *arg);
int rb_thread_join(rb_thread_t th);
int rb_thread_detach(rb_thread_t th);
rb_thread_t rb_thread_self(void);
int rb_thread_equal(rb_thread_t a, rb_thread_t b);

void rb_once(rb_once_t *once, void (*fn)(void));

void rb_sleep_ms(unsigned int ms);
uint64_t rb_now_ms(void);
int rb_path_exists(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* RADIOBRIDGE_RB_THREAD_H */
