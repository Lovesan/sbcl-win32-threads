#ifndef SBCL_INCLUDED_OS_DEFAULT_H
#define SBCL_INCLUDED_OS_DEFAULT_H

#ifndef HAVE_os_vm_mmap_unit_size
#define os_vm_mmap_unit_size os_vm_page_size
#endif

#ifndef HAVE_os_open_core
#define os_open_core(filename,mode) open(filename,mode)
#endif

#ifndef HAVE_os_fopen_runtime
#define os_fopen_runtime(filename,mode) fopen(filename,mode)
#endif

#ifndef HAVE_os_invalidate_free
#define os_invalidate_free os_invalidate
#endif

#ifndef HAVE_os_validate_recommit
#define os_validate_recommit os_validate
#endif

#ifndef HAVE_os_allocate_lazily
#define os_allocate_lazily(len) os_validate(NULL,len)
#endif

#ifndef HAVE_os_number_of_processors
/* To be used for things like spinning-vs-yielding-vs-sleeping.  The
   default value of 0, provided here, means "unknown" (OS-specific
   implementation may set the real os_number_of_processors to 0 in run
   time as well, to designate the same thing: unknown, couldn't
   query it, doesn't make sense). */

#define os_number_of_processors 0
#endif

#ifndef PUSH_ERRNO
#define PUSH_ERRNO				\
    {						\
    int sbcl__lastErrno = errno;		\

#define POP_ERRNO				\
    errno = sbcl__lastErrno;			\
    }
#endif


#ifndef THREAD_ALIEN_RESERVE
#define THREAD_ALIEN_RESERVE (0)
#endif

#ifndef PSEUDO_ATOMIC_SET_HIGHLEVEL
#define PSEUDO_ATOMIC_SET_HIGHLEVEL do {} while(0)
#endif

#ifndef PSEUDO_ATOMIC_FLUSH_LOWLEVEL
#define PSEUDO_ATOMIC_FLUSH_LOWLEVEL do {} while(0)
#endif

#ifndef AMD64_SYSV_ABI
#define AMD64_SYSV_ABI
#endif

#ifndef odxprint
#define odxprint(tag,...) do {} while(0)
#endif

#ifndef fast_aligned_fill_words
static inline void fast_aligned_fill_words(void*addr, size_t len, lispobj pattern)
{
    lispobj* ptr = addr;
    while(len--)
	*(ptr++) = pattern;
}
#endif

#endif /* SBCL_INCLUDED_OS_DEFAULT_H */
