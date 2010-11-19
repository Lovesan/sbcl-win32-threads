#ifndef WIN32_PTHREAD_INCLUDED
#define WIN32_PTHREAD_INCLUDED

#include <time.h>
#include <errno.h>
#include <sys/types.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* 0 - Misc */


#define SIG_IGN ((void (*)(int, siginfo_t, void*))-1)
#define SIG_DFL ((void (*)(int, siginfo_t, void*))-2)

#define SIGHUP    1
#define SIGINT    2 /* Interactive attention */
#define SIGQUIT   3
#define SIGILL    4 /* Illegal instruction */
#define SIGPIPE   5
#define SIGALRM   6
#define SIGURG    7
#define SIGFPE    8 /* Floating point error */
#define SIGTSTP   9
#define SIGCHLD   10
#define SIGSEGV   11 /* Segmentation violation */
#define SIGIO     12
#define SIGXCPU   13
#define SIGXFSZ   14
#define SIGTERM   15 /* Termination request */
#define SIGVTALRM 16
#define SIGPROF   17
#define SIGWINCH  18
#define SIGBREAK  21 /* Control-break */
#define SIGABRT   22 /* Abnormal termination (abort) */

#define SIGRTMIN  23

#define SIG_DEFER SIGHUP

#define NSIG 31     /* maximum signal number + 1 */

void pthreads_win32_init();

/* 1 - Thread */

typedef struct pthread_thread* pthread_t;

typedef struct pthread_attr_t {
  unsigned int stack_size;
} pthread_attr_t;

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setstack(pthread_attr_t *attr, void *stackaddr, size_t stacksize);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize);

typedef void (*pthread_cleanup_fn)(void* arg);

#define pthread_cleanup_push(fn, arg) { pthread_cleanup_fn __pthread_fn = fn; void *__pthread_arg = arg;
#define pthread_cleanup_pop(execute) if (execute) __pthread_fn(__pthread_arg); }

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine) (void *), void *arg);
int pthread_equal(pthread_t thread1, pthread_t thread2);
int pthread_detach(pthread_t thread);
int pthread_join(pthread_t thread, void **retval);
pthread_t pthread_self(void);

typedef DWORD pthread_key_t;
int pthread_key_create(pthread_key_t *key, void (*destructor)(void*));
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

#define SIG_BLOCK 1
#define SIG_UNBLOCK 2
#define SIG_SETMASK 3
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);

/* 1a - Thread non-portable */

void pthread_np_suspend(pthread_t thread);
void pthread_np_suspend_with_signal(pthread_t thread, int signum);
void pthread_np_resume(pthread_t thread);
void pthread_np_request_interruption(pthread_t thread);

/* 2 - Mutex */

typedef CRITICAL_SECTION* pthread_mutex_t;
typedef int pthread_mutexattr_t;
#define PTHREAD_MUTEX_INITIALIZER ((pthread_mutex_t)-1)
int pthread_mutex_init(pthread_mutex_t * mutex, const pthread_mutexattr_t * attr);
int pthread_mutexattr_init(pthread_mutexattr_t*);
int pthread_mutexattr_destroy(pthread_mutexattr_t*);
int pthread_mutexattr_settype(pthread_mutexattr_t*, int);
#define PTHREAD_MUTEX_ERRORCHECK 0
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

/* 3 - Condition variable */

typedef struct thread_wakeup {
  HANDLE event;
  struct thread_wakeup *next;
} thread_wakeup;

typedef HANDLE (*cv_event_get_fn)();
typedef void (*cv_event_return_fn)(HANDLE event);

typedef struct pthread_cond_t {
  pthread_mutex_t wakeup_lock;
  struct thread_wakeup *first_wakeup;
  struct thread_wakeup *last_wakeup;
  unsigned char alertable;
  cv_event_get_fn get_fn;
  cv_event_return_fn return_fn;
} pthread_cond_t;

typedef struct pthread_condattr_t {
  unsigned char alertable;
  cv_event_get_fn get_fn;
  cv_event_return_fn return_fn;
} pthread_condattr_t;

#ifndef _TIMESPEC_DEFINED
typedef struct timespec {
  time_t tv_sec;
  long tv_nsec;
} timespec;
#endif

// not implemented: PTHREAD_COND_INITIALIZER
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_init(pthread_cond_t * cond, const pthread_condattr_t * attr);
int pthread_cond_broadcast(pthread_cond_t *cond);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_timedwait(pthread_cond_t * cond, pthread_mutex_t * mutex, const struct timespec * abstime);
int pthread_cond_wait(pthread_cond_t * cond, pthread_mutex_t * mutex);

#define ETIMEDOUT 123 //Something

int sched_yield();

void pthread_lock_structures();
void pthread_unlock_structures();

typedef void *(*pthread_fn)(void*);

typedef enum {
  pthread_state_running,
  pthread_state_finished,
  pthread_state_joined
} pthread_thread_state;

typedef struct pthread_thread {
  pthread_fn start_routine;
  void* arg;
  HANDLE handle;
  pthread_cond_t *waiting_cond;
  sigset_t blocked_signal_set;
  unsigned int signal_is_pending[NSIG];
  void * retval;

  pthread_mutex_t lock;
  pthread_cond_t cond;
  int detached;
  pthread_thread_state state;

  /* Boolean flag: thread will produce fibers instead of threads with
     pthread_create */
  int fiber_factory;

  /* NULL if current thread has no fibers and is not a fiber; LPVOID
     returned by CreateFiber or ConvertThreadToFiber otherwise */
  void* fiber;

  /* True if pthreads_win32 created fiber, false if it was already
     present and just captured. We should delete our fiber when not
     needed, but external fibers should be left intact. */
  int own_fiber;

  /* For noticed foreign threads, wait_handle contains a result of
     RegisterWaitForSingleObject. */
  HANDLE wait_handle;

  /* When a fiber function finishes, it switches to fiber_parent
     instead of deleting fiber. */
  void* fiber_parent;

  /* But someone has to clean up after switching to fiber_parent.  Let
     it be the parent itself: before switching, parent sets
     fiber_cleanup_pointer to a place where dead child fiber pointer
     is to be stored. */

  void** fiber_cleanup_pointer;

  /* for non-current fiber, this field provides context of its
     last-known running state: not for jumps et al., but for
     conservative stack GCing */
  CONTEXT* fiber_context;

  /* Thread TEB base */
  void* teb;

  /* Suspend and resume semantics:

     - Current thread with current fiber, i.e. pthread_self() is
     really suspended by itself (caller knows better).

     - Current thread with non-current fiber: suspend_count is
     incremented on suspend, decremented (but not below 0) on
     resume, and switching to a suspended fiber is prevented.

     - Other thread and other thread's fiber: thread itself is
     suspended. */

  int suspend_count;

  void (*cleanup_callback)(void* context);
  void *cleanup_context;
  pthread_t (*fiber_callback)(void* context);
  void *fiber_callback_context;
} pthread_thread;

void pthread_np_pending_signal_handler(int signum);

void pthread_np_add_pending_signal(pthread_t thread, int signum);
void pthread_np_remove_pending_signal(pthread_t thread, int signum);

int pthread_np_notice_thread();
int pthread_np_get_thread_context(pthread_t thread, CONTEXT* context);
int pthread_np_convert_self_to_fiber();
int pthread_np_switch_to_fiber(pthread_t fiber);
int pthread_np_run_in_fiber(pthread_t pth, pthread_t (*callback)(void*),
                            void* context);
int pthread_np_set_fiber_factory_mode(int on);
int pthread_np_fiber_save_tls(int slot, int enable);
void pthread_np_set_cleanup(pthread_t thread, void (*cleaner)(void*),void* context);
HANDLE pthread_np_get_handle(pthread_t pth);
void* pthread_np_get_lowlevel_fiber(pthread_t pth);
int pthread_np_delete_lowlevel_fiber(void* ll_fiber);

int pthread_np_donate_fiber(pthread_t fiber, pthread_t recipient);

int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigdelset(sigset_t *set, int signum);
int sigismember(const sigset_t *set, int signum);

typedef int sig_atomic_t;

#endif
