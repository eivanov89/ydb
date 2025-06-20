/* Safe automatic memory allocation.
   Copyright (C) 2003-2007, 2009-2025 Free Software Foundation, Inc.
   Written by Bruno Haible <bruno@clisp.org>, 2003.

   This file is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of the
   License, or (at your option) any later version.

   This file is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

#ifndef _MALLOCA_H
#define _MALLOCA_H

/* This file uses _GL_ATTRIBUTE_ALLOC_SIZE, _GL_ATTRIBUTE_DEALLOC,
   _GL_ATTRIBUTE_MALLOC, HAVE_ALLOCA.  */
#if !_GL_CONFIG_H_INCLUDED
 #error "Please include config.h first."
#endif

#include <alloca.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#if defined __CHERI_PURE_CAPABILITY__
# error #include <cheri.h>
#endif

#include "xalloc-oversized.h"


#ifdef __cplusplus
extern "C" {
#endif


/* safe_alloca(N) is equivalent to alloca(N) when it is safe to call
   alloca(N); otherwise it returns NULL.  It either returns N bytes of
   memory allocated on the stack, that lasts until the function returns,
   or NULL.
   Use of safe_alloca should be avoided:
     - inside arguments of function calls - undefined behaviour,
     - in inline functions - the allocation may actually last until the
       calling function returns.
*/
#if HAVE_ALLOCA
/* The OS usually guarantees only one guard page at the bottom of the stack,
   and a page size can be as small as 4096 bytes.  So we cannot safely
   allocate anything larger than 4096 bytes.  Also care for the possibility
   of a few compiler-allocated temporary stack slots.
   This must be a macro, not a function.  */
# define safe_alloca(N) ((N) < 4032 ? alloca (N) : NULL)
#else
# define safe_alloca(N) ((void) (N), NULL)
#endif

/* Free a block of memory allocated through malloca().  */
#if HAVE_ALLOCA
extern void freea (void *p);
#else
# define freea free
#endif

/* malloca(N) is a safe variant of alloca(N).  It allocates N bytes of
   memory allocated on the stack, that must be freed using freea() before
   the function returns.  Upon failure, it returns NULL.  */
#if HAVE_ALLOCA
# if defined __CHERI_PURE_CAPABILITY__
#  define malloca(N) \
    ((N) < 4032 - (2 * sa_alignment_max - 1)                                  \
     ? cheri_bounds_set ((void *) (((uintptr_t)                               \
                                     (char *)                                 \
                                      alloca ((N) + 2 * sa_alignment_max - 1) \
                                    + (2 * sa_alignment_max - 1))             \
                                   & ~(uintptr_t)(2 * sa_alignment_max - 1)), \
                         (N))                                                 \
     : mmalloca (N))
# else
#  define malloca(N) \
    ((N) < 4032 - (2 * sa_alignment_max - 1)                                   \
     ? (void *) (((uintptr_t) (char *) alloca ((N) + 2 * sa_alignment_max - 1) \
                  + (2 * sa_alignment_max - 1))                                \
                 & ~(uintptr_t)(2 * sa_alignment_max - 1))                     \
     : mmalloca (N))
# endif
#else
# define malloca(N) \
  mmalloca (N)
#endif
extern void *mmalloca (size_t n)
  _GL_ATTRIBUTE_MALLOC _GL_ATTRIBUTE_DEALLOC (freea, 1)
  _GL_ATTRIBUTE_ALLOC_SIZE ((1));

/* nmalloca(N,S) is an overflow-safe variant of malloca (N * S).
   It allocates an array of N objects, each with S bytes of memory,
   on the stack.  N and S should be nonnegative and free of side effects.
   The array must be freed using freea() before the function returns.  */
#define nmalloca(n, s) \
  (xalloc_oversized (n, s) ? NULL : malloca ((n) * (size_t) (s)))


#ifdef __cplusplus
}
#endif


/* ------------------- Auxiliary, non-public definitions ------------------- */

/* Determine the alignment of a type at compile time.  */
#if defined __GNUC__ || defined __clang__ || defined __IBM__ALIGNOF__
# define sa_alignof __alignof__
#elif defined __cplusplus
  template <class type> struct sa_alignof_helper { char __slot1; type __slot2; };
# define sa_alignof(type) offsetof (sa_alignof_helper<type>, __slot2)
#elif defined __hpux
  /* Work around a HP-UX 10.20 cc bug with enums constants defined as offsetof
     values.  */
# define sa_alignof(type) (sizeof (type) <= 4 ? 4 : 8)
#elif defined _AIX
  /* Work around an AIX 3.2.5 xlc bug with enums constants defined as offsetof
     values.  */
# define sa_alignof(type) (sizeof (type) <= 4 ? 4 : 8)
#else
# define sa_alignof(type) offsetof (struct { char __slot1; type __slot2; }, __slot2)
#endif

enum
{
/* The desired alignment of memory allocations is the maximum alignment
   among all elementary types.  */
  sa_alignment_long = sa_alignof (long),
  sa_alignment_double = sa_alignof (double),
  sa_alignment_longlong = sa_alignof (long long),
  sa_alignment_longdouble = sa_alignof (long double),
  sa_alignment_max = ((sa_alignment_long - 1) | (sa_alignment_double - 1)
                      | (sa_alignment_longlong - 1)
                      | (sa_alignment_longdouble - 1)
                     ) + 1
};

#endif /* _MALLOCA_H */
