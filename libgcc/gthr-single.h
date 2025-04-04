/* Threads compatibility routines for libgcc2 and libobjc.  */
/* Compile this one with gcc.  */
/* Copyright (C) 1997-2025 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

Under Section 7 of GPL version 3, you are granted additional
permissions described in the GCC Runtime Library Exception, version
3.1, as published by the Free Software Foundation.

You should have received a copy of the GNU General Public License and
a copy of the GCC Runtime Library Exception along with this program;
see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
<http://www.gnu.org/licenses/>.  */

#ifndef GCC_GTHR_SINGLE_H
#define GCC_GTHR_SINGLE_H

/* Just provide compatibility for mutex handling.  */

typedef int __gthread_key_t;
typedef int __gthread_once_t;
typedef int __gthread_mutex_t;
typedef int __gthread_recursive_mutex_t;

#define __GTHREAD_ONCE_INIT 0
#define __GTHREAD_MUTEX_INIT 0
#define __GTHREAD_MUTEX_INIT_FUNCTION(mx) do {} while (0)
#define __GTHREAD_RECURSIVE_MUTEX_INIT 0

#ifdef __has_attribute
# if __has_attribute(__always_inline__)
#  define __GTHREAD_ALWAYS_INLINE __attribute__((__always_inline__))
# endif
#endif
#ifndef __GTHREAD_ALWAYS_INLINE
# define __GTHREAD_ALWAYS_INLINE
#endif

#ifdef __cplusplus
# define __GTHREAD_INLINE inline __GTHREAD_ALWAYS_INLINE
#else
# define __GTHREAD_INLINE static inline
#endif

#define UNUSED __attribute__((__unused__))

#ifdef _LIBOBJC

/* Thread local storage for a single thread */
static void *thread_local_storage = NULL;

/* Backend initialization functions */

/* Initialize the threads subsystem.  */
static inline int
__gthread_objc_init_thread_system (void)
{
  /* No thread support available */
  return -1;
}

/* Close the threads subsystem.  */
static inline int
__gthread_objc_close_thread_system (void)
{
  /* No thread support available */
  return -1;
}

/* Backend thread functions */

/* Create a new thread of execution.  */
static inline objc_thread_t
__gthread_objc_thread_detach (void (* func)(void *), void * arg UNUSED)
{
  /* No thread support available */
  return NULL;
}

/* Set the current thread's priority.  */
static inline int
__gthread_objc_thread_set_priority (int priority UNUSED)
{
  /* No thread support available */
  return -1;
}

/* Return the current thread's priority.  */
static inline int
__gthread_objc_thread_get_priority (void)
{
  return OBJC_THREAD_INTERACTIVE_PRIORITY;
}

/* Yield our process time to another thread.  */
static inline void
__gthread_objc_thread_yield (void)
{
  return;
}

/* Terminate the current thread.  */
static inline int
__gthread_objc_thread_exit (void)
{
  /* No thread support available */
  /* Should we really exit the program */
  /* exit (&__objc_thread_exit_status); */
  return -1;
}

/* Returns an integer value which uniquely describes a thread.  */
static inline objc_thread_t
__gthread_objc_thread_id (void)
{
  /* No thread support, use 1.  */
  return (objc_thread_t) 1;
}

/* Sets the thread's local storage pointer.  */
static inline int
__gthread_objc_thread_set_data (void *value)
{
  thread_local_storage = value;
  return 0;
}

/* Returns the thread's local storage pointer.  */
static inline void *
__gthread_objc_thread_get_data (void)
{
  return thread_local_storage;
}

/* Backend mutex functions */

/* Allocate a mutex.  */
static inline int
__gthread_objc_mutex_allocate (objc_mutex_t mutex UNUSED)
{
  return 0;
}

/* Deallocate a mutex.  */
static inline int
__gthread_objc_mutex_deallocate (objc_mutex_t mutex UNUSED)
{
  return 0;
}

/* Grab a lock on a mutex.  */
static inline int
__gthread_objc_mutex_lock (objc_mutex_t mutex UNUSED)
{
  /* There can only be one thread, so we always get the lock */
  return 0;
}

/* Try to grab a lock on a mutex.  */
static inline int
__gthread_objc_mutex_trylock (objc_mutex_t mutex UNUSED)
{
  /* There can only be one thread, so we always get the lock */
  return 0;
}

/* Unlock the mutex */
static inline int
__gthread_objc_mutex_unlock (objc_mutex_t mutex UNUSED)
{
  return 0;
}

/* Backend condition mutex functions */

/* Allocate a condition.  */
static inline int
__gthread_objc_condition_allocate (objc_condition_t condition UNUSED)
{
  return 0;
}

/* Deallocate a condition.  */
static inline int
__gthread_objc_condition_deallocate (objc_condition_t condition UNUSED)
{
  return 0;
}

/* Wait on the condition */
static inline int
__gthread_objc_condition_wait (objc_condition_t condition UNUSED,
			       objc_mutex_t mutex UNUSED)
{
  return 0;
}

/* Wake up all threads waiting on this condition.  */
static inline int
__gthread_objc_condition_broadcast (objc_condition_t condition UNUSED)
{
  return 0;
}

/* Wake up one thread waiting on this condition.  */
static inline int
__gthread_objc_condition_signal (objc_condition_t condition UNUSED)
{
  return 0;
}

#else /* _LIBOBJC */

__GTHREAD_INLINE int
__gthread_active_p (void)
{
  return 0;
}

__GTHREAD_INLINE int
__gthread_once (__gthread_once_t *__once UNUSED, void (*__func) (void) UNUSED)
{
  return 0;
}

__GTHREAD_INLINE int UNUSED
__gthread_key_create (__gthread_key_t *__key UNUSED, void (*__func) (void *) UNUSED)
{
  return 0;
}

__GTHREAD_INLINE int UNUSED
__gthread_key_delete (__gthread_key_t __key UNUSED)
{
  return 0;
}

__GTHREAD_INLINE void *
__gthread_getspecific (__gthread_key_t __key UNUSED)
{
  return 0;
}

__GTHREAD_INLINE int
__gthread_setspecific (__gthread_key_t __key UNUSED, const void *__v UNUSED)
{
  return 0;
}

__GTHREAD_INLINE int
__gthread_mutex_destroy (__gthread_mutex_t *__mutex UNUSED)
{
  return 0;
}

__GTHREAD_INLINE int
__gthread_mutex_lock (__gthread_mutex_t *__mutex UNUSED)
{
  return 0;
}

__GTHREAD_INLINE int
__gthread_mutex_trylock (__gthread_mutex_t *__mutex UNUSED)
{
  return 0;
}

__GTHREAD_INLINE int
__gthread_mutex_unlock (__gthread_mutex_t *__mutex UNUSED)
{
  return 0;
}

__GTHREAD_INLINE int
__gthread_recursive_mutex_lock (__gthread_recursive_mutex_t *__mutex)
{
  return __gthread_mutex_lock (__mutex);
}

__GTHREAD_INLINE int
__gthread_recursive_mutex_trylock (__gthread_recursive_mutex_t *__mutex)
{
  return __gthread_mutex_trylock (__mutex);
}

__GTHREAD_INLINE int
__gthread_recursive_mutex_unlock (__gthread_recursive_mutex_t *__mutex)
{
  return __gthread_mutex_unlock (__mutex);
}

__GTHREAD_INLINE int
__gthread_recursive_mutex_destroy (__gthread_recursive_mutex_t *__mutex)
{
  return __gthread_mutex_destroy (__mutex);
}

#endif /* _LIBOBJC */

#undef UNUSED
#undef __GTHREAD_INLINE
#undef __GTHREAD_ALWAYS_INLINE

#endif /* ! GCC_GTHR_SINGLE_H */
