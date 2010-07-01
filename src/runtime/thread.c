/*
 * This software is part of the SBCL system. See the README file for
 * more information.
 *
 * This software is derived from the CMU CL system, which was
 * written at Carnegie Mellon University and released into the
 * public domain. The software is in the public domain and is
 * provided with absolutely no warranty. See the COPYING and CREDITS
 * files for more information.
 */

#include "sbcl.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifndef LISP_FEATURE_WIN32
#include <sched.h>
#include "win32-os.h"
#endif
#include "pthreads_win32.h"
#include <stddef.h>
#include <errno.h>
#include <sys/types.h>
#ifndef LISP_FEATURE_WIN32
#include <sys/wait.h>
#endif

#ifdef LISP_FEATURE_MACH_EXCEPTION_HANDLER
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_types.h>
#endif

#include "runtime.h"
#include "validate.h"           /* for BINDING_STACK_SIZE etc */
#include "thread.h"
#include "arch.h"
#include "target-arch-os.h"
#include "os.h"
#include "globals.h"
#include "dynbind.h"
#include "genesis/cons.h"
#include "genesis/fdefn.h"
#include "interr.h"             /* for lose() */
#include "alloc.h"
#include "gc-internal.h"
#include "pseudo-atomic.h"

#ifdef LISP_FEATURE_WIN32
/*
 * Win32 doesn't have SIGSTKSZ, and we're not switching stacks anyway,
 * so define it arbitrarily
 */
#define SIGSTKSZ 1024
#endif

#if defined(LISP_FEATURE_DARWIN) && defined(LISP_FEATURE_SB_THREAD)
#define DELAY_THREAD_POST_MORTEM 5
#define LOCK_CREATE_THREAD
#endif

#ifdef LISP_FEATURE_FREEBSD
#define CREATE_CLEANUP_THREAD
#define LOCK_CREATE_THREAD
#endif

#ifdef LISP_FEATURE_SB_THREAD
struct thread_post_mortem {
#ifdef DELAY_THREAD_POST_MORTEM
    struct thread_post_mortem *next;
#endif
    os_thread_t os_thread;
    pthread_attr_t *os_attr;
    os_vm_address_t os_address;
};

#ifdef DELAY_THREAD_POST_MORTEM
static int pending_thread_post_mortem_count = 0;
pthread_mutex_t thread_post_mortem_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
static struct thread_post_mortem * volatile pending_thread_post_mortem = 0;
#endif

int dynamic_values_bytes=TLS_SIZE*sizeof(lispobj);  /* same for all threads */
struct thread *all_threads;
extern struct interrupt_data * global_interrupt_data;

#ifdef LISP_FEATURE_SB_THREAD
pthread_mutex_t all_threads_lock = PTHREAD_MUTEX_INITIALIZER;
#ifdef LOCK_CREATE_THREAD
static pthread_mutex_t create_thread_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
#ifdef LISP_FEATURE_GCC_TLS
__thread struct thread *current_thread;
#endif
pthread_key_t lisp_thread = 0;
#endif

#if defined(LISP_FEATURE_X86) || defined(LISP_FEATURE_X86_64)
extern lispobj call_into_lisp_first_time(lispobj fun, lispobj *args, int nargs);
#endif

static void
link_thread(struct thread *th)
{
    if (all_threads) all_threads->prev=th;
    th->next=all_threads;
    th->prev=0;
    all_threads=th;
}

#ifdef LISP_FEATURE_SB_THREAD
static void
unlink_thread(struct thread *th)
{
    if (th->prev)
        th->prev->next = th->next;
    else
        all_threads = th->next;
    if (th->next)
        th->next->prev = th->prev;
}
#endif

static int
initial_thread_trampoline(struct thread *th)
{
    lispobj function;
#if defined(LISP_FEATURE_X86) || defined(LISP_FEATURE_X86_64)
    lispobj *args = NULL;
#endif
#ifdef LISP_FEATURE_SB_THREAD
    pthread_setspecific(lisp_thread, (void *)1);
#endif
    function = th->no_tls_value_marker;
    th->no_tls_value_marker = NO_TLS_VALUE_MARKER_WIDETAG;
    if(arch_os_thread_init(th)==0) return 1;
    link_thread(th);
    th->os_thread = thread_self();
#ifndef LISP_FEATURE_WIN32
    protect_control_stack_hard_guard_page(1, NULL);
    protect_binding_stack_hard_guard_page(1, NULL);
    protect_alien_stack_hard_guard_page(1, NULL);
    protect_control_stack_guard_page(1, NULL);
    protect_binding_stack_guard_page(1, NULL);
    protect_alien_stack_guard_page(1, NULL);
#endif

#if defined(LISP_FEATURE_X86) || defined(LISP_FEATURE_X86_64)
    odprintf("call_into_lisp_first_time");
    return call_into_lisp_first_time(function,args,0);
#else
    return funcall0(function);
#endif
}

#ifdef LISP_FEATURE_SB_THREAD
#define THREAD_STATE_LOCK_SIZE \
    (sizeof(pthread_mutex_t))+(sizeof(pthread_cond_t))
#else
#define THREAD_STATE_LOCK_SIZE 0
#endif

#define THREAD_STRUCT_SIZE (thread_control_stack_size + BINDING_STACK_SIZE + \
                            ALIEN_STACK_SIZE +                               \
                            THREAD_STATE_LOCK_SIZE +                         \
                            dynamic_values_bytes +                           \
                            32 * SIGSTKSZ +                                  \
                            THREAD_ALIGNMENT_BYTES)

#ifdef LISP_FEATURE_SB_THREAD
/* THREAD POST MORTEM CLEANUP
 *
 * Memory allocated for the thread stacks cannot be reclaimed while
 * the thread is still alive, so we need a mechanism for post mortem
 * cleanups. FIXME: We actually have three, for historical reasons as
 * the saying goes. Do we really need three? Nikodemus guesses that
 * not anymore, now that we properly call pthread_attr_destroy before
 * freeing the stack. */

static struct thread_post_mortem *
plan_thread_post_mortem(struct thread *corpse)
{
    if (corpse) {
        struct thread_post_mortem *post_mortem = malloc(sizeof(struct thread_post_mortem));
        gc_assert(post_mortem);
        post_mortem->os_thread = corpse->os_thread;
        post_mortem->os_attr = corpse->os_attr;
        post_mortem->os_address = corpse->os_address;
#ifdef DELAY_THREAD_POST_MORTEM
        post_mortem->next = NULL;
#endif
        return post_mortem;
    } else {
        /* FIXME: When does this happen? */
        return NULL;
    }
}

static void
perform_thread_post_mortem(struct thread_post_mortem *post_mortem)
{
#if defined(CREATE_POST_MORTEM_THREAD)
    pthread_detach(pthread_self());
#endif
    odprintf("perform_thread_post_mortem(0x%p)", post_mortem);
    if (post_mortem) {
        gc_assert(!pthread_join(post_mortem->os_thread, NULL));
        gc_assert(!pthread_attr_destroy(post_mortem->os_attr));
        free(post_mortem->os_attr);
#if defined(LISP_FEATURE_WIN32)
        os_invalidate_free(post_mortem->os_address, THREAD_STRUCT_SIZE);
#else
        os_invalidate(post_mortem->os_address, THREAD_STRUCT_SIZE);
#endif
        free(post_mortem);
    }
}

static void
schedule_thread_post_mortem(struct thread *corpse)
{
    struct thread_post_mortem *post_mortem = NULL;
    odprintf("schedule_thread_post_mortem(0x%p)", corpse);
    if (corpse) {
        post_mortem = plan_thread_post_mortem(corpse);

#ifdef DELAY_THREAD_POST_MORTEM
        pthread_mutex_lock(&thread_post_mortem_lock);
        /* First stick the new post mortem to the end of the queue. */
        if (pending_thread_post_mortem) {
            struct thread_post_mortem *next = pending_thread_post_mortem;
            while (next->next) {
                next = next->next;
            }
            next->next = post_mortem;
        } else {
            pending_thread_post_mortem = post_mortem;
        }
        /* Then, if there are enough things in the queue, clean up one
         * from the head -- or increment the count, and null out the
         * post_mortem we have. */
        if (pending_thread_post_mortem_count > DELAY_THREAD_POST_MORTEM) {
            post_mortem = pending_thread_post_mortem;
            pending_thread_post_mortem = post_mortem->next;
        } else {
            pending_thread_post_mortem_count++;
            post_mortem = NULL;
        }
        pthread_mutex_unlock(&thread_post_mortem_lock);
        /* Finally run, the cleanup, if any. */
        perform_thread_post_mortem(post_mortem);
#elif defined(CREATE_POST_MORTEM_THREAD)
        gc_assert(!pthread_create(&thread, NULL, perform_thread_post_mortem, post_mortem));
#else
        post_mortem = (struct thread_post_mortem *)
            swap_lispobjs((lispobj *)(void *)&pending_thread_post_mortem,
                          (lispobj)post_mortem);
        perform_thread_post_mortem(post_mortem);
#endif
    }
}

/* this is the first thing that runs in the child (which is why the
 * silly calling convention).  Basically it calls the user's requested
 * lisp function after doing arch_os_thread_init and whatever other
 * bookkeeping needs to be done
 */
int
new_thread_trampoline(struct thread *th)
{
    lispobj function;
    int result, lock_ret;
    odprintf("new_thread_trampoline");
    FSHOW((stderr,"/creating thread %lu\n", thread_self()));
    check_deferrables_blocked_or_lose(0);
    check_gc_signals_unblocked_or_lose(0);
    pthread_setspecific(lisp_thread, (void *)1);
    function = th->no_tls_value_marker;
    th->no_tls_value_marker = NO_TLS_VALUE_MARKER_WIDETAG;
    if(arch_os_thread_init(th)==0) {
        /* FIXME: handle error */
        lose("arch_os_thread_init failed\n");
    }

    th->os_thread=thread_self();
    protect_control_stack_guard_page(1, NULL);
    protect_binding_stack_guard_page(1, NULL);
    protect_alien_stack_guard_page(1, NULL);
    /* Since GC can only know about this thread from the all_threads
     * list and we're just adding this thread to it, there is no
     * danger of deadlocking even with SIG_STOP_FOR_GC blocked (which
     * it is not). */
    odprintf("new_thread_trampoline, acquiring all_threads_lock");
    lock_ret = pthread_mutex_lock(&all_threads_lock);
    odprintf("new_thread_trampoline, acquired all_threads_lock");
    gc_assert(lock_ret == 0);
    link_thread(th);
    lock_ret = pthread_mutex_unlock(&all_threads_lock);
    odprintf("new_thread_trampoline, released all_threads_lock");
    gc_assert(lock_ret == 0);

    odprintf("new_thread_trampoline, calling thread function 0x%p", function);
    result = funcall0(function);

    /* Block GC */
    block_blockable_signals(0, 0);
    set_thread_state(th, STATE_DEAD);

    /* SIG_STOP_FOR_GC is blocked and GC might be waiting for this
     * thread, but since we are already dead it won't wait long. */
    lock_ret = pthread_mutex_lock(&all_threads_lock);
    gc_assert(lock_ret == 0);

    gc_alloc_update_page_tables(BOXED_PAGE_FLAG, &th->alloc_region);
    unlink_thread(th);
    pthread_mutex_unlock(&all_threads_lock);
    gc_assert(lock_ret == 0);

    if(th->tls_cookie>=0) arch_os_thread_cleanup(th);
    pthread_mutex_destroy(th->state_lock);
    pthread_cond_destroy(th->state_cond);

#if defined(LISP_FEATURE_WIN32)
  #if defined(LISP_FEATURE_SB_THREAD)
    pthread_mutex_destroy(&th->interrupt_data->win32_data.lock);
  #endif
    os_invalidate_free((os_vm_address_t)th->interrupt_data,
                  (sizeof (struct interrupt_data)));
#else
    os_invalidate((os_vm_address_t)th->interrupt_data,
                  (sizeof (struct interrupt_data)));
#endif

#ifdef LISP_FEATURE_MACH_EXCEPTION_HANDLER
    FSHOW((stderr, "Deallocating mach port %x\n", THREAD_STRUCT_TO_EXCEPTION_PORT(th)));
    mach_port_move_member(mach_task_self(),
                          THREAD_STRUCT_TO_EXCEPTION_PORT(th),
                          MACH_PORT_NULL);
    mach_port_deallocate(mach_task_self(),
                         THREAD_STRUCT_TO_EXCEPTION_PORT(th));
    mach_port_destroy(mach_task_self(),
                      THREAD_STRUCT_TO_EXCEPTION_PORT(th));
#endif

    schedule_thread_post_mortem(th);
    FSHOW((stderr,"/exiting thread %lu\n", thread_self()));
    return result;
}

#endif /* LISP_FEATURE_SB_THREAD */

static void
free_thread_struct(struct thread *th)
{
    odprintf("free_thread_struct 0x%p (0x%p)", th, th->os_thread);
#if defined(LIS_FEATURE_WIN32)
    if (th->interrupt_data) {
        #if defined(LISP_FEATURE_WIN32)
        pthread_mutex_destroy(&th->interrupt_data->win32_data.lock);
        #endif
        os_invalidate_free((os_vm_address_t) th->interrupt_data,
                      (sizeof (struct interrupt_data)));
    }
    os_invalidate_free((os_vm_address_t) th->os_address,
                  THREAD_STRUCT_SIZE);
#else
    if (th->interrupt_data)
        os_invalidate((os_vm_address_t) th->interrupt_data,
                      (sizeof (struct interrupt_data)));
    os_invalidate((os_vm_address_t) th->os_address,
                  THREAD_STRUCT_SIZE);
#endif
}

/* this is called from any other thread to create the new one, and
 * initialize all parts of it that can be initialized from another
 * thread
 */

static struct thread *
create_thread_struct(lispobj initial_function) {
    union per_thread_data *per_thread;
    struct thread *th=0;        /*  subdue gcc */
    void *spaces=0;
    void *aligned_spaces=0;
#ifdef LISP_FEATURE_SB_THREAD
    unsigned int i;
#endif
    odprintf("create_thread_struct");

    /* May as well allocate all the spaces at once: it saves us from
     * having to decide what to do if only some of the allocations
     * succeed. SPACES must be appropriately aligned, since the GC
     * expects the control stack to start at a page boundary -- and
     * the OS may have even more rigorous requirements. We can't rely
     * on the alignment passed from os_validate, since that might
     * assume the current (e.g. 4k) pagesize, while we calculate with
     * the biggest (e.g. 64k) pagesize allowed by the ABI. */
    spaces=os_validate(0, THREAD_STRUCT_SIZE);
    if(!spaces) {
        odprintf("spaces == null");
        return NULL;
    }
    /* Aligning up is safe as THREAD_STRUCT_SIZE has
     * THREAD_ALIGNMENT_BYTES padding. */
    aligned_spaces = (void *)((((unsigned long)(char *)spaces)
                               + THREAD_ALIGNMENT_BYTES-1)
                              &~(unsigned long)(THREAD_ALIGNMENT_BYTES-1));
    per_thread=(union per_thread_data *)
        (aligned_spaces+
         thread_control_stack_size+
         BINDING_STACK_SIZE+
         ALIEN_STACK_SIZE +
         THREAD_STATE_LOCK_SIZE);

#ifdef LISP_FEATURE_SB_THREAD
    for(i = 0; i < (dynamic_values_bytes / sizeof(lispobj)); i++)
        per_thread->dynamic_values[i] = NO_TLS_VALUE_MARKER_WIDETAG;
    if (all_threads == 0) {
        if(SymbolValue(FREE_TLS_INDEX,0)==UNBOUND_MARKER_WIDETAG) {
            SetSymbolValue
                (FREE_TLS_INDEX,
                 /* FIXME: should be MAX_INTERRUPTS -1 ? */
                 make_fixnum(MAX_INTERRUPTS+
                             sizeof(struct thread)/sizeof(lispobj)),
                 0);
            SetSymbolValue(TLS_INDEX_LOCK,make_fixnum(0),0);
        }
#define STATIC_TLS_INIT(sym,field) \
  ((struct symbol *)(sym-OTHER_POINTER_LOWTAG))->tls_index= \
  make_fixnum(THREAD_SLOT_OFFSET_WORDS(field))

        STATIC_TLS_INIT(BINDING_STACK_START,binding_stack_start);
        STATIC_TLS_INIT(BINDING_STACK_POINTER,binding_stack_pointer);
        STATIC_TLS_INIT(CONTROL_STACK_START,control_stack_start);
        STATIC_TLS_INIT(CONTROL_STACK_END,control_stack_end);
        STATIC_TLS_INIT(ALIEN_STACK,alien_stack_pointer);
#if defined(LISP_FEATURE_X86) || defined (LISP_FEATURE_X86_64)
        STATIC_TLS_INIT(PSEUDO_ATOMIC_BITS,pseudo_atomic_bits);
#endif
#undef STATIC_TLS_INIT
    }
#endif

    th=&per_thread->thread;
    th->os_address = spaces;
    th->control_stack_start = aligned_spaces;
    th->binding_stack_start=
        (lispobj*)((void*)th->control_stack_start+thread_control_stack_size);
    th->control_stack_end = th->binding_stack_start;
    th->control_stack_guard_page_protected = T;
    th->alien_stack_start=
        (lispobj*)((void*)th->binding_stack_start+BINDING_STACK_SIZE);
    th->binding_stack_pointer=th->binding_stack_start;
    th->this=th;
    memset(&th->os_thread, 0, sizeof(th->os_thread));
#ifdef LISP_FEATURE_SB_THREAD
    th->os_attr=malloc(sizeof(pthread_attr_t));
    th->state_lock=(pthread_mutex_t *)((void *)th->alien_stack_start +
                                       ALIEN_STACK_SIZE);
    pthread_mutex_init(th->state_lock, NULL);
    th->state_cond=(pthread_cond_t *)((void *)th->state_lock +
                                      (sizeof(pthread_mutex_t)));
    pthread_cond_init(th->state_cond, NULL);
#endif
    th->state=STATE_RUNNING;
#ifdef LISP_FEATURE_STACK_GROWS_DOWNWARD_NOT_UPWARD
    th->alien_stack_pointer=((void *)th->alien_stack_start
                             + ALIEN_STACK_SIZE-N_WORD_BYTES);
#else
    th->alien_stack_pointer=((void *)th->alien_stack_start);
#endif
#if defined(LISP_FEATURE_X86) || defined (LISP_FEATURE_X86_64)
    th->pseudo_atomic_bits=0;
#endif
#ifdef LISP_FEATURE_GENCGC
    gc_set_region_empty(&th->alloc_region);
#endif

#ifndef LISP_FEATURE_SB_THREAD
    /* the tls-points-into-struct-thread trick is only good for threaded
     * sbcl, because unithread sbcl doesn't have tls.  So, we copy the
     * appropriate values from struct thread here, and make sure that
     * we use the appropriate SymbolValue macros to access any of the
     * variable quantities from the C runtime.  It's not quite OAOOM,
     * it just feels like it */
    SetSymbolValue(BINDING_STACK_START,(lispobj)th->binding_stack_start,th);
    SetSymbolValue(CONTROL_STACK_START,(lispobj)th->control_stack_start,th);
    SetSymbolValue(CONTROL_STACK_END,(lispobj)th->control_stack_end,th);
#if defined(LISP_FEATURE_X86) || defined (LISP_FEATURE_X86_64)
    SetSymbolValue(BINDING_STACK_POINTER,(lispobj)th->binding_stack_pointer,th);
    SetSymbolValue(ALIEN_STACK,(lispobj)th->alien_stack_pointer,th);
    SetSymbolValue(PSEUDO_ATOMIC_BITS,(lispobj)th->pseudo_atomic_bits,th);
#else
    current_binding_stack_pointer=th->binding_stack_pointer;
    current_control_stack_pointer=th->control_stack_start;
#endif
#endif
    bind_variable(CURRENT_CATCH_BLOCK,make_fixnum(0),th);
    bind_variable(CURRENT_UNWIND_PROTECT_BLOCK,make_fixnum(0),th);
    bind_variable(FREE_INTERRUPT_CONTEXT_INDEX,make_fixnum(0),th);
    bind_variable(INTERRUPT_PENDING, NIL,th);
    bind_variable(INTERRUPTS_ENABLED,T,th);
    bind_variable(ALLOW_WITH_INTERRUPTS,T,th);
    bind_variable(GC_PENDING,NIL,th);
    bind_variable(ALLOC_SIGNAL,NIL,th);
#ifdef LISP_FEATURE_SB_THREAD
    bind_variable(STOP_FOR_GC_PENDING,NIL,th);
#endif
#if defined(LISP_FEATURE_WIN32) && defined(LISP_FEATURE_SB_THREAD)
    th->gc_safe = 0;
#endif

    th->interrupt_data = (struct interrupt_data *)
        os_validate(0,(sizeof (struct interrupt_data)));
    if (!th->interrupt_data) {
        odprintf("th->interrupt_data == null");
        free_thread_struct(th);
        return 0;
    }
    th->interrupt_data->pending_handler = 0;
    th->interrupt_data->gc_blocked_deferrables = 0;
#ifdef LISP_FEATURE_PPC
    th->interrupt_data->allocation_trap_context = 0;
#endif
    th->no_tls_value_marker=initial_function;
    
#if defined(LISP_FEATURE_WIN32) && defined(LISP_FEATURE_SB_THREAD)
    th->interrupt_data->win32_data.interrupts_count = 0;
    pthread_mutex_init(&th->interrupt_data->win32_data.lock, NULL);
#endif

    th->stepping = NIL;
    odprintf("create_thread_struct ok");
    return th;
}

#ifdef LISP_FEATURE_MACH_EXCEPTION_HANDLER
mach_port_t setup_mach_exception_handling_thread();
kern_return_t mach_thread_init(mach_port_t thread_exception_port);

#endif

void create_initial_thread(lispobj initial_function) {
    struct thread *th=create_thread_struct(initial_function);
#ifdef LISP_FEATURE_SB_THREAD
    pthread_key_create(&lisp_thread, 0);
#endif
    if(th) {
#ifdef LISP_FEATURE_MACH_EXCEPTION_HANDLER
        setup_mach_exception_handling_thread();
#endif
        initial_thread_trampoline(th); /* no return */
    } else lose("can't create initial thread\n");
}

#ifdef LISP_FEATURE_SB_THREAD

#ifndef __USE_XOPEN2K
extern int pthread_attr_setstack (pthread_attr_t *__attr, void *__stackaddr,
                                  size_t __stacksize);
#endif

boolean create_os_thread(struct thread *th,os_thread_t *kid_tid)
{
    /* The new thread inherits the restrictive signal mask set here,
     * and enables signals again when it is set up properly. */
    sigset_t oldset;
    boolean r=1;
    int retcode = 0, initcode;

    FSHOW_SIGNAL((stderr,"/create_os_thread: creating new thread\n"));

    /* Blocking deferrable signals is enough, no need to block
     * SIG_STOP_FOR_GC because the child process is not linked onto
     * all_threads until it's ready. */
    block_deferrable_signals(0, &oldset);

#ifdef LOCK_CREATE_THREAD
    retcode = pthread_mutex_lock(&create_thread_lock);
    gc_assert(retcode == 0);
    FSHOW_SIGNAL((stderr,"/create_os_thread: got lock\n"));
#endif

    if((initcode = pthread_attr_init(th->os_attr)) ||
       /* call_into_lisp_first_time switches the stack for the initial
        * thread. For the others, we use this. */
#if defined(LISP_FEATURE_WIN32)
       (pthread_attr_setstacksize(th->os_attr, thread_control_stack_size)) ||
#else
       (pthread_attr_setstack(th->os_attr,th->control_stack_start,
                              thread_control_stack_size)) ||
#endif
       (retcode = pthread_create
        (kid_tid,th->os_attr,(void *(*)(void *))new_thread_trampoline,th))
        ) {
        FSHOW_SIGNAL((stderr, "init = %d\n", initcode));
        FSHOW_SIGNAL((stderr, "pthread_create returned %d, errno %d\n",
                      retcode, errno));
        if(retcode < 0) {
            perror("create_os_thread");
        }
        r=0;
    }

#ifdef LOCK_CREATE_THREAD
    retcode = pthread_mutex_unlock(&create_thread_lock);
    gc_assert(retcode == 0);
    FSHOW_SIGNAL((stderr,"/create_os_thread: released lock\n"));
#endif
    thread_sigmask(SIG_SETMASK,&oldset,0);
    return r;
}

os_thread_t create_thread(lispobj initial_function) {
    struct thread *th, *thread = arch_os_get_current_thread();
    os_thread_t kid_tid;
    memset(&kid_tid, 0, sizeof(kid_tid));

    /* Must defend against async unwinds. */
    if (SymbolValue(INTERRUPTS_ENABLED, thread) != NIL)
        lose("create_thread is not safe when interrupts are enabled.\n");

    /* Assuming that a fresh thread struct has no lisp objects in it,
     * linking it to all_threads can be left to the thread itself
     * without fear of gc lossage. initial_function violates this
     * assumption and must stay pinned until the child starts up. */
    th = create_thread_struct(initial_function);
    if (!th)
      odprintf("create_thread_struct failed");
    if (th && !create_os_thread(th,&kid_tid)) {
        odprintf("create_os_thread failed");
        free_thread_struct(th);
        memset(&kid_tid, 0, sizeof(kid_tid));
    }
    return kid_tid;
}

/* stopping the world is a two-stage process.  From this thread we signal
 * all the others with SIG_STOP_FOR_GC.  The handler for this signal does
 * the usual pseudo-atomic checks (we don't want to stop a thread while
 * it's in the middle of allocation) then waits for another SIG_STOP_FOR_GC.
 */

#if defined(LISP_FEATURE_WIN32)

struct threads_suspend_info suspend_info = {
  0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
  SUSPEND_REASON_NONE, 0, NULL, NULL
};

// returns: 0 if all is ok
// -1 if max interrupts reached
int schedule_thread_interrupt(struct thread * th, lispobj interrupt_fn)
{
  odprintf("Locking 0x%p (th->interrupt_data = 0x%p)", &th->interrupt_data->win32_data.lock, th->interrupt_data);
  pthread_mutex_lock(&th->interrupt_data->win32_data.lock);
  odprintf("Locked 0x%p", &th->interrupt_data->win32_data.lock);
  if (th->interrupt_data->win32_data.interrupts_count == MAX_INTERRUPTS) {
    pthread_mutex_unlock(&th->interrupt_data->win32_data.lock);
    odprintf("Max interrupts");
    return -1;
  } else {
    ++th->interrupt_data->win32_data.interrupts_count;
    th->interrupt_data->win32_data.interrupts[th->interrupt_data->win32_data.interrupts_count - 1] = interrupt_fn;
    odprintf("Thread now has %d interrupts", th->interrupt_data->win32_data.interrupts_count);
    pthread_mutex_unlock(&th->interrupt_data->win32_data.lock);
    SetSymbolValue(INTERRUPT_PENDING, T, th);
    return 0;
  }
}

void gc_stop_the_world();
void gc_start_the_world();

struct thread * find_thread_by_os_thread(pthread_t thread)
{
  struct thread * retval = NULL;
  struct thread * p;
  
  pthread_mutex_lock(&all_threads_lock);
  for(p = all_threads; p; p = p->next) {
    if (p->os_thread == thread)
      retval = p;
  }
  pthread_mutex_unlock(&all_threads_lock);
  
  return retval;
}

void roll_thread_to_safepoint(struct thread * thread)
{
  int gc_safe;
  struct thread * p;
  pthread_mutex_lock(&all_threads_lock);
  pthread_mutex_lock(&suspend_info.world_lock);
  
  pthread_mutex_lock(&suspend_info.lock);
  suspend_info.reason = SUSPEND_REASON_INTERRUPT;
  suspend_info.interrupted_thread = thread;
  suspend_info.phase = 1;
  suspend_info.suspend = 1;
  pthread_mutex_unlock(&suspend_info.lock);
  
  unmap_gc_page();
  
  // Phase 1: Make sure that th is in gc-safe code or noted the need to interrupt
  
  while ((gc_safe = thread->gc_safe) == GC_SAFE_CHANGING) Sleep(0);
  if (!gc_safe) {
    wait_for_thread_state_change(thread, STATE_RUNNING);
  }
  
  map_gc_page();
  
  pthread_mutex_lock(&suspend_info.lock);
  suspend_info.suspend = 0;
  pthread_mutex_unlock(&suspend_info.lock);
  
  for (p = all_threads; p; p = p->next) {
    if (thread_state(p) != STATE_DEAD)
      set_thread_state(p, STATE_RUNNING);
  }
  
  pthread_mutex_unlock(&suspend_info.world_lock);
  pthread_mutex_unlock(&all_threads_lock);
}

// returns: 0 if interrupt is queued
// -1 if max interrupts reached
int interrupt_lisp_thread(pthread_t thread, lispobj interrupt_fn)
{
  struct thread * th = find_thread_by_os_thread(thread);
  odprintf("Interrupting 0x%p with 0x%p", thread, interrupt_fn);
  if (schedule_thread_interrupt(th, interrupt_fn) != 0) {
    odprintf("schedule_thread_interrupt returned non-zero");
    return -1;
  }
  
  odprintf("schedule_thread_interrupt returned zero");
  
  gc_stop_the_world();
  gc_start_the_world();
  
  odprintf("restarted the world");
  
  return 0;
}

void check_pending_interrupts()
{
  struct thread * p = arch_os_get_current_thread();
  sigset_t sigset;
  if (p->interrupt_data->win32_data.interrupts_count == 0) {
    return;
  }
  get_current_sigmask(&sigset);
  if (sigismember(&sigset, SIGHUP)) {
    odprintf("SIGHUP is blocked");
    pthread_np_add_pending_signal(p->os_thread, SIGHUP);
    return;
  }
  
  if (SymbolValue(INTERRUPTS_ENABLED, p) == NIL) {
    if (p->interrupt_data->win32_data.interrupts_count > 0) {
      SetSymbolValue(INTERRUPT_PENDING, T, p);
      odprintf("INTERRUPTS_ENABLED == NIL");
    }
    return;
  }
  
  while (1) {
    pthread_mutex_lock(&p->interrupt_data->win32_data.lock);
    if (p->interrupt_data->win32_data.interrupts_count > 0) {
      lispobj fn = p->interrupt_data->win32_data.interrupts[p->interrupt_data->win32_data.interrupts_count - 1];
      p->interrupt_data->win32_data.interrupts_count--;
      pthread_mutex_unlock(&p->interrupt_data->win32_data.lock);
      odprintf("calling interrupt 0x%p", fn);
      funcall0(fn);
    } else {
      SetSymbolValue(INTERRUPT_PENDING, NIL, p);
      pthread_mutex_unlock(&p->interrupt_data->win32_data.lock);
      break;
    }
  }
}

// returns old gc_safe
int set_gc_safe(int gc_safe)
{
  struct thread * self = arch_os_get_current_thread();
  int old_gc_safe = self->gc_safe;
  self->gc_safe = GC_SAFE_CHANGING;
  if (suspend_info.suspend) {
    odprintf("entering safepoint from set_gc_safe(%d) from %d", gc_safe, old_gc_safe);
    gc_safepoint();
  }
  self->gc_safe = gc_safe;
  return old_gc_safe;
}

void gc_enter_safe_region()
{
  struct thread * self = arch_os_get_current_thread();
  set_gc_safe(self->gc_safe + 1);
}

void gc_leave_safe_region()
{
  struct thread * self = arch_os_get_current_thread();
  set_gc_safe(self->gc_safe - 1);
}

void safepoint_cycle_state(int state)
{
  struct thread * self = arch_os_get_current_thread();
  odprintf("safepoint_cycle_state(%s) begin", get_thread_state_string(state));
  set_thread_state(self, state);
  pthread_mutex_unlock(&suspend_info.lock);
  odprintf("waiting for state change from %s", get_thread_state_string(state));
  wait_for_thread_state_change(self, state);
  odprintf("state changed from %s to %s", get_thread_state_string(state), get_thread_state_string(thread_state(self)));
  odprintf("safepoint_cycle_state(%s) end", get_thread_state_string(state));
}

void suspend()
{
  odprintf("suspend() begin");
  safepoint_cycle_state(STATE_SUSPENDED);
  odprintf("suspend() end");
}

void suspend_briefly()
{
  odprintf("suspend_briefly() begin");
  if (suspend_info.phase == 1 || suspend_info.reason == SUSPEND_REASON_INTERRUPT) {
    safepoint_cycle_state(STATE_SUSPENDED_BRIEFLY);
  } else {
    pthread_mutex_unlock(&suspend_info.lock);
  }
  odprintf("suspend_briefly() end");
}

int thread_may_gc()
{
  // Thread may gc if all of these are true:
  // 1) SIG_STOP_FOR_GC is unblocked
  // 2) GC_INHIBIT is NIL
  // 3) INTERRUPTS_ENABLED is not-NIL

  struct thread * self = arch_os_get_current_thread();
  
  sigset_t ss;
  pthread_sigmask(SIG_BLOCK, NULL, &ss);
  if (sigismember(&ss, SIG_STOP_FOR_GC)) {
    odprintf("SIG_STOP_FOR_GC is blocked");
    SetSymbolValue(STOP_FOR_GC_PENDING, T, self);
    return 0;
  }
    
  if (SymbolValue(GC_INHIBIT, self) != NIL) {
    odprintf("GC_INHIBIT != NIL");
    SetSymbolValue(STOP_FOR_GC_PENDING, T, self);
    return 0;
  }
    
  if (SymbolValue(INTERRUPTS_ENABLED, self) == NIL) {
    odprintf("INTERRUPTS_ENABLED == NIL");
    SetSymbolValue(STOP_FOR_GC_PENDING, T, self);
    SetSymbolValue(INTERRUPT_PENDING, T, self);
    return 0;
  }
    
  return 1;
}

int thread_may_interrupt()
{
  // Thread may be interrupted if all of these are true:
  // 1) SIGUSR1 is unblocked
  // 2) INTERRUPTS_ENABLED is not-nil
  struct thread * self = arch_os_get_current_thread();
  
  sigset_t ss;
  pthread_sigmask(SIG_BLOCK, NULL, &ss);
  if (sigismember(&ss, SIGHUP))
    return 0;
    
  if (SymbolValue(INTERRUPTS_ENABLED, self) == NIL)
    return 0;
  
  return 1;
}

void gc_safepoint()
{
  struct thread * self = arch_os_get_current_thread();
  if (!suspend_info.suspend) {
    check_pending_interrupts();
    SetSymbolValue(STOP_FOR_GC_PENDING, NIL, self);
    SetSymbolValue(INTERRUPT_PENDING, NIL, self);
    return;
  }
  pthread_mutex_lock(&suspend_info.lock);
  if (!suspend_info.suspend) {
    pthread_mutex_unlock(&suspend_info.lock);
    check_pending_interrupts();
  }
  odprintf("gc_safepoint, suspend_info.suspend = 1");
  if (suspend_info.reason == SUSPEND_REASON_GC) {
    odprintf("suspend_info.reason == SUSPEND_REASON_GC");
    if (self == suspend_info.gc_thread) {
      odprintf("suspend_info.gc_thread == self");
      pthread_mutex_unlock(&suspend_info.lock);
    } else
    if (thread_may_gc()) {
      odprintf("thread_may_gc() is true");
      suspend();
      check_pending_interrupts();
    } else {
      odprintf("thread_may_gc() is false");
      if (suspend_info.phase == 1) {
        odprintf("suspend_info.phase == 1");
        suspend_briefly();
      } else {
        pthread_mutex_unlock(&suspend_info.lock);
      }
    }
  } else
  if (suspend_info.reason == SUSPEND_REASON_INTERRUPT) {
    if (suspend_info.interrupted_thread != self) {
      suspend_briefly();
    } else
    if (thread_may_interrupt()) {
      suspend();
      check_pending_interrupts();
    } else {
      suspend_briefly();
    }
  }
}

void pthread_np_safepoint()
{
}

void pthread_np_pending_signal_handler(int signum)
{
  if (signum == SIG_STOP_FOR_GC || signum == SIGHUP) {
    gc_safepoint();
  }
}

lispobj fn_by_pc(unsigned int pc)
{
  lispobj obj = search_read_only_space((void*)pc);
  if (!obj)
    obj = search_static_space((void*)pc);
  if (!obj)
    obj = search_dynamic_space((void*)pc);
  return obj;
}

int pc_in_lisp_code(unsigned int pc)
{
  return
    search_read_only_space((void*)pc) != NULL ||
    search_static_space((void*)pc) != NULL ||
    search_dynamic_space((void*)pc) != NULL;
}

int thread_get_pc(struct thread *th)
{
  CONTEXT ctx;
  pthread_np_get_thread_context(th->os_thread, &ctx);
  return ctx.Eip;
}

int thread_get_pc_susp(struct thread *th)
{
  CONTEXT ctx;
  pthread_np_suspend(th->os_thread);
  pthread_np_get_thread_context(th->os_thread, &ctx);
  pthread_np_resume(th->os_thread);
  return ctx.Eip;
}

int thread_in_lisp_code(struct thread *th)
{
  return pc_in_lisp_code(thread_get_pc(th));
}

const char * fn_name(lispobj fn)
{
  return "unknown";
}

#endif

#if defined(LISP_FEATURE_WIN32)
/* To avoid deadlocks when gc stops the world all clients of each
 * mutex must enable or disable SIG_STOP_FOR_GC for the duration of
 * holding the lock, but they must agree on which. */
void gc_stop_the_world()
{
    struct thread *p,*th=arch_os_get_current_thread();
    int lock_ret;
    int gc_safe;
    odprintf("gc_stop_the_world() begin");
    
#ifdef LOCK_CREATE_THREAD
    lock_ret = pthread_mutex_lock(&create_thread_lock);
    gc_assert(lock_ret == 0);
#endif
    pthread_mutex_lock(&suspend_info.world_lock);
    
    odprintf("begin phase 1");
    
    pthread_mutex_lock(&suspend_info.lock);
    suspend_info.reason = SUSPEND_REASON_GC;
    suspend_info.gc_thread = arch_os_get_current_thread();
    suspend_info.phase = 1;
    suspend_info.suspend = 1;
    pthread_mutex_unlock(&suspend_info.lock);
    
    unmap_gc_page();

    lock_ret = pthread_mutex_lock(&all_threads_lock);
    gc_assert(lock_ret == 0);

    // Phase 1, make sure that all threads are: 1) have noted the need to interrupt; or 2) in gc-safe code
    
    for(p=all_threads; p; p=p->next) {
        gc_assert(p->os_thread != 0);
        if (p != th) {
          odprintf("looking at 0x%p, state is %s, gc_safe = %d", p->os_thread, get_thread_state_string(thread_state(p)), p->gc_safe);
          while ((gc_safe = th->gc_safe) == GC_SAFE_CHANGING) Sleep(0);
          if (gc_safe) continue;
          odprintf("Waiting until 0x%p suspends, eip = 0x%p", p->os_thread, thread_get_pc_susp(p));
          wait_for_thread_state_change(p, STATE_RUNNING);
          odprintf("0x%p is suspended", p->os_thread);
        }
    }
    
    // Phase 2, wait until all threads 1) suspend themselves; or 2) are in gc-safe code
    
    odprintf("begin phase 2");
    
    pthread_mutex_lock(&suspend_info.lock);
    map_gc_page();
    suspend_info.phase = 2;
    pthread_mutex_unlock(&suspend_info.lock);
    
    for (p = all_threads; p; p = p->next) {
      int gc_safe;
      gc_assert(p->os_thread != 0);
      if (p == th) continue;
      while ((gc_safe = th->gc_safe) == GC_SAFE_CHANGING) Sleep(0);
      if (!gc_safe) {
        if (thread_state(th) == STATE_SUSPENDED_BRIEFLY) {
          set_thread_state(th, STATE_RUNNING);
          wait_for_thread_state_change(th, STATE_RUNNING);
        }
      }
    }

    odprintf("gc_stop_the_world() end");
}

void gc_start_the_world()
{
  struct thread * self = arch_os_get_current_thread();
  struct thread * p;
  lispobj state;
  int lock_ret;
  
  odprintf("gc_start_the_world() begin");

  pthread_mutex_lock(&suspend_info.lock);
  suspend_info.suspend = 0;
  pthread_mutex_unlock(&suspend_info.lock);
  
  for(p = all_threads; p; p=p->next) {
    gc_assert(p->os_thread != 0);
    if (p == self) continue;
    state = thread_state(p);
    
    if (state == STATE_DEAD) continue;
    
    set_thread_state(p, STATE_RUNNING);
  }

  lock_ret = pthread_mutex_unlock(&all_threads_lock);
  gc_assert(lock_ret == 0);
  pthread_mutex_unlock(&suspend_info.world_lock);
#ifdef LOCK_CREATE_THREAD
  lock_ret = pthread_mutex_unlock(&create_thread_lock);
  gc_assert(lock_ret == 0);
#endif
  odprintf("gc_start_the_world() end");
}

#else

/* To avoid deadlocks when gc stops the world all clients of each
 * mutex must enable or disable SIG_STOP_FOR_GC for the duration of
 * holding the lock, but they must agree on which. */
void gc_stop_the_world()
{
    struct thread *p,*th=arch_os_get_current_thread();
    int status, lock_ret;
    odprintf("stopping the world\n");
    pthread_mutex_lock(&suspend_info.world_lock);
    
    pthread_mutex_lock(&suspend_info.lock);
    suspend_info.reason = SUSPEND_REASON_GC;
    suspend_info.gc_thread = arch_os_get_current_thread();
    suspend_info.phase = 1;
    suspend_info.suspend = 1;
    pthread_mutex_unlock(&suspend_info.lock);
    
    unmap_gc_page();
    
#ifdef LOCK_CREATE_THREAD
    /* KLUDGE: Stopping the thread during pthread_create() causes deadlock
     * on FreeBSD. */
    FSHOW_SIGNAL((stderr,"/gc_stop_the_world:waiting on create_thread_lock\n"));
    lock_ret = pthread_mutex_lock(&create_thread_lock);
    gc_assert(lock_ret == 0);
    FSHOW_SIGNAL((stderr,"/gc_stop_the_world:got create_thread_lock\n"));
#endif
#if defined(LISP_FEATURE_WIN32)
    pthread_lock_structures();
#endif
    FSHOW_SIGNAL((stderr,"/gc_stop_the_world:waiting on lock\n"));
    /* keep threads from starting while the world is stopped. */
    lock_ret = pthread_mutex_lock(&all_threads_lock);
    gc_assert(lock_ret == 0);

    FSHOW_SIGNAL((stderr,"/gc_stop_the_world:got lock\n"));
    /* stop all other threads by sending them SIG_STOP_FOR_GC */
    for(p=all_threads; p; p=p->next) {
        gc_assert(p->os_thread != 0);
        FSHOW_SIGNAL((stderr,"/gc_stop_the_world: thread=%lu, state=%x\n",
                      p->os_thread, thread_state(p)));
        if (p != th)
          odprintf("looking at 0x%p, state is %s, gc_safe = %d", p->os_thread, get_thread_state_string(thread_state(p)), p->gc_safe);
        if((p!=th)) {
            int gc_safe;
            FSHOW_SIGNAL((stderr,"/gc_stop_the_world: suspending thread %lu\n",
                          p->os_thread));
#if defined(LISP_FEATURE_WIN32)
            while ((gc_safe = th->gc_safe) == GC_SAFE_CHANGING) sleep(0);
            if (gc_safe) continue;
            odprintf("Waiting until 0x%p suspends, eip = 0x%p", p->os_thread, thread_get_pc_susp(p));
            wait_for_thread_state_change(p, STATE_RUNNING);
            odprintf("0x%p is suspended", p->os_thread);
#else
            /* We already hold all_thread_lock, P can become DEAD but
             * cannot exit, ergo it's safe to use pthread_kill. */
            status=pthread_kill(p->os_thread,SIG_STOP_FOR_GC);

            if (status==ESRCH) {
                /* This thread has exited. */
                gc_assert(thread_state(p)==STATE_DEAD);
            } else if (status) {
                lose("cannot send suspend thread=%lu: %d, %s\n",
                     p->os_thread,status,strerror(status));
            }
#endif
        }
    }
    FSHOW_SIGNAL((stderr,"/gc_stop_the_world:signals sent\n"));
#if !defined(LISP_FEATURE_WIN32)
    for(p=all_threads;p;p=p->next) {
        if (p!=th) {
            FSHOW_SIGNAL
                ((stderr,
                  "/gc_stop_the_world: waiting for thread=%lu: state=%x\n",
                  p->os_thread, thread_state(p)));
            wait_for_thread_state_change(p, STATE_RUNNING);
            if (p->state == STATE_RUNNING)
                lose("/gc_stop_the_world: unexpected state");
        }
    }
#endif
#if defined(LISP_FEATURE_WIN32)
    pthread_unlock_structures();
#endif
    map_gc_page();
    FSHOW_SIGNAL((stderr,"/gc_stop_the_world:end\n"));
    odprintf(" stopped the world\n");
}

void gc_start_the_world()
{
    struct thread *p,*th=arch_os_get_current_thread();
    int lock_ret;
    odprintf(" starting the world\n");
    pthread_mutex_lock(&stop_for_gc_lock);
    stop_for_gc = 0;
    pthread_mutex_unlock(&stop_for_gc_lock);
    gc_thread = NULL;
    /* if a resumed thread creates a new thread before we're done with
     * this loop, the new thread will get consed on the front of
     * all_threads, but it won't have been stopped so won't need
     * restarting */
    FSHOW_SIGNAL((stderr,"/gc_start_the_world:begin\n"));
    for(p=all_threads;p;p=p->next) {
        gc_assert(p->os_thread!=0);
        if (p!=th) {
            lispobj state = thread_state(p);
            if (state != STATE_DEAD) {
                if(state != STATE_SUSPENDED && state != STATE_RUNNING) {
                    lose("gc_start_the_world: wrong thread state is %d\n",
                         fixnum_value(state));
                }
                FSHOW_SIGNAL((stderr, "/gc_start_the_world: resuming %lu\n",
                              p->os_thread));
#if defined(LISP_FEATURE_WIN32)
                odprintf("resuming 0x%p from state %s, gc_safe = %d", p->os_thread, get_thread_state_string(thread_state(p)), p->gc_safe);
#endif
                set_thread_state(p, STATE_RUNNING);
            }
        }
    }

    lock_ret = pthread_mutex_unlock(&all_threads_lock);
    gc_assert(lock_ret == 0);
#ifdef LOCK_CREATE_THREAD
    lock_ret = pthread_mutex_unlock(&create_thread_lock);
    gc_assert(lock_ret == 0);
#endif
    pthread_mutex_unlock(&world_stop_lock);

    FSHOW_SIGNAL((stderr,"/gc_start_the_world:end\n"));
    odprintf(" started the world\n");
}
#endif
#endif

int
thread_yield()
{
#ifdef LISP_FEATURE_SB_THREAD
    return sched_yield();
#else
    return 0;
#endif
}

/* If the thread id given does not belong to a running thread (it has
 * exited or never even existed) pthread_kill _may_ fail with ESRCH,
 * but it is also allowed to just segfault, see
 * <http://udrepper.livejournal.com/16844.html>.
 *
 * Relying on thread ids can easily backfire since ids are recycled
 * (NPTL recycles them extremely fast) so a signal can be sent to
 * another process if the one it was sent to exited.
 *
 * We send signals in two places: signal_interrupt_thread sends a
 * signal that's harmless if delivered to another thread, but
 * SIG_STOP_FOR_GC is fatal.
 *
 * For these reasons, we must make sure that the thread is still alive
 * when the pthread_kill is called and return if the thread is
 * exiting. */
int
kill_safely(os_thread_t os_thread, int signal)
{
    FSHOW_SIGNAL((stderr,"/kill_safely: %lu, %d\n", os_thread, signal));
#if defined(LISP_FEATURE_WIN32)
    return 0;
#else
    {
#ifdef LISP_FEATURE_SB_THREAD
        sigset_t oldset;
        struct thread *thread;
        /* pthread_kill is not async signal safe and we don't want to be
         * interrupted while holding the lock. */
        block_deferrable_signals(0, &oldset);
        pthread_mutex_lock(&all_threads_lock);
        for (thread = all_threads; thread; thread = thread->next) {
            if (thread->os_thread == os_thread) {
                int status = pthread_kill(*os_thread, signal);
                if (status)
                    lose("kill_safely: pthread_kill failed with %d\n", status);
                break;
            }
        }
        pthread_mutex_unlock(&all_threads_lock);
        thread_sigmask(SIG_SETMASK,&oldset,0);
        if (thread)
            return 0;
        else
            return -1;
#else
        int status;
        if (os_thread != 0)
            lose("kill_safely: who do you want to kill? %d?\n", os_thread);
        /* Dubious (as in don't know why it works) workaround for the
         * signal sometimes not being generated on darwin. */
#ifdef LISP_FEATURE_DARWIN
        {
            sigset_t oldset;
            sigprocmask(SIG_BLOCK, &deferrable_sigset, &oldset);
            status = raise(signal);
            sigprocmask(SIG_SETMASK,&oldset,0);
        }
#else
        status = raise(signal);
#endif
        if (status == 0) {
            return 0;
        } else {
            lose("cannot raise signal %d, %d %s\n",
                 signal, status, strerror(errno));
        }
#endif
    }
#endif
}
