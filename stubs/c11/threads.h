// stubs/threads.h — complete C11 <threads.h> shim backed by pthreads,
// for platforms (Bionic/Termux) where Mesa's own c11/threads.h refuses
// to build. NOT a full/correct C11 implementation — just enough surface
// area for Mesa's util code (simple_mtx.h, u_printf.h, ralloc.c, etc.)
// to compile and behave correctly in a single-process test binary.
#ifndef STUB_THREADS_H
#define STUB_THREADS_H

#include <pthread.h>
#include <time.h>
#include <errno.h>

// ---- return codes ----
enum {
    thrd_success  = 0,
    thrd_timedout = 1,
    thrd_busy     = 2,
    thrd_error    = 3,
    thrd_nomem    = 4,
};

// ---- once_flag / call_once ----
typedef pthread_once_t once_flag;
#define ONCE_FLAG_INIT PTHREAD_ONCE_INIT
static inline void call_once(once_flag *flag, void (*func)(void)) {
    pthread_once(flag, func);
}

// ---- mtx_t ----
typedef pthread_mutex_t mtx_t;
enum {
    mtx_plain     = 0,
    mtx_recursive = 1,
    mtx_timed     = 2,
};
static inline int mtx_init(mtx_t *m, int type) {
    if (type & mtx_recursive) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        int r = pthread_mutex_init(m, &attr);
        pthread_mutexattr_destroy(&attr);
        return r == 0 ? thrd_success : thrd_error;
    }
    return pthread_mutex_init(m, NULL) == 0 ? thrd_success : thrd_error;
}
static inline int mtx_lock(mtx_t *m) {
    return pthread_mutex_lock(m) == 0 ? thrd_success : thrd_error;
}
static inline int mtx_trylock(mtx_t *m) {
    int r = pthread_mutex_trylock(m);
    if (r == 0) return thrd_success;
    if (r == EBUSY) return thrd_busy;
    return thrd_error;
}
static inline int mtx_timedlock(mtx_t *m, const struct timespec *ts) {
    int r = pthread_mutex_timedlock(m, ts);
    if (r == 0) return thrd_success;
    if (r == ETIMEDOUT) return thrd_timedout;
    return thrd_error;
}
static inline int mtx_unlock(mtx_t *m) {
    return pthread_mutex_unlock(m) == 0 ? thrd_success : thrd_error;
}
static inline void mtx_destroy(mtx_t *m) {
    pthread_mutex_destroy(m);
}

// ---- cnd_t ----
typedef pthread_cond_t cnd_t;
static inline int cnd_init(cnd_t *c) {
    return pthread_cond_init(c, NULL) == 0 ? thrd_success : thrd_error;
}
static inline int cnd_signal(cnd_t *c) {
    return pthread_cond_signal(c) == 0 ? thrd_success : thrd_error;
}
static inline int cnd_broadcast(cnd_t *c) {
    return pthread_cond_broadcast(c) == 0 ? thrd_success : thrd_error;
}
static inline int cnd_wait(cnd_t *c, mtx_t *m) {
    return pthread_cond_wait(c, m) == 0 ? thrd_success : thrd_error;
}
static inline int cnd_timedwait(cnd_t *c, mtx_t *m, const struct timespec *ts) {
    int r = pthread_cond_timedwait(c, m, ts);
    if (r == 0) return thrd_success;
    if (r == ETIMEDOUT) return thrd_timedout;
    return thrd_error;
}
static inline void cnd_destroy(cnd_t *c) {
    pthread_cond_destroy(c);
}

// ---- thrd_t ----
typedef pthread_t thrd_t;
typedef int (*thrd_start_t)(void *);

struct __stub_thrd_trampoline_args {
    thrd_start_t func;
    void *arg;
};
static inline void *__stub_thrd_trampoline(void *p) {
    struct __stub_thrd_trampoline_args *a = p;
    thrd_start_t func = a->func;
    void *arg = a->arg;
    int rc = func(arg);
    (void)rc;
    return NULL;
}
static inline int thrd_create(thrd_t *t, thrd_start_t func, void *arg) {
    struct __stub_thrd_trampoline_args *a = malloc(sizeof(*a));
    if (!a) return thrd_nomem;
    a->func = func;
    a->arg = arg;
    int r = pthread_create(t, NULL, __stub_thrd_trampoline, a);
    return r == 0 ? thrd_success : thrd_error;
}
static inline int thrd_join(thrd_t t, int *res) {
    void *ret;
    int r = pthread_join(t, &ret);
    if (res) *res = 0;
    return r == 0 ? thrd_success : thrd_error;
}
static inline void thrd_yield(void) {
    sched_yield();
}
static inline thrd_t thrd_current(void) {
    return pthread_self();
}
static inline int thrd_equal(thrd_t a, thrd_t b) {
    return pthread_equal(a, b) != 0;
}

#endif // STUB_THREADS_H