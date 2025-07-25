/*! \file */
/*
 * kmp.h -- KPTS runtime header file.
 */

//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef KMP_H
#define KMP_H

#include "kmp_config.h"

/* #define BUILD_PARALLEL_ORDERED 1 */

/* This fix replaces gettimeofday with clock_gettime for better scalability on
   the Altix.  Requires user code to be linked with -lrt. */
//#define FIX_SGI_CLOCK

/* Defines for OpenMP 3.0 tasking and auto scheduling */

#ifndef KMP_STATIC_STEAL_ENABLED
#define KMP_STATIC_STEAL_ENABLED 1
#endif
#define KMP_WEIGHTED_ITERATIONS_SUPPORTED                                      \
  (KMP_AFFINITY_SUPPORTED && KMP_STATIC_STEAL_ENABLED &&                       \
   (KMP_ARCH_X86 || KMP_ARCH_X86_64))

#define TASK_CURRENT_NOT_QUEUED 0
#define TASK_CURRENT_QUEUED 1

#ifdef BUILD_TIED_TASK_STACK
#define TASK_STACK_EMPTY 0 // entries when the stack is empty
#define TASK_STACK_BLOCK_BITS 5 // Used in TASK_STACK_SIZE and TASK_STACK_MASK
// Number of entries in each task stack array
#define TASK_STACK_BLOCK_SIZE (1 << TASK_STACK_BLOCK_BITS)
// Mask for determining index into stack block
#define TASK_STACK_INDEX_MASK (TASK_STACK_BLOCK_SIZE - 1)
#endif // BUILD_TIED_TASK_STACK

#define TASK_NOT_PUSHED 1
#define TASK_SUCCESSFULLY_PUSHED 0
#define TASK_TIED 1
#define TASK_UNTIED 0
#define TASK_EXPLICIT 1
#define TASK_IMPLICIT 0
#define TASK_PROXY 1
#define TASK_FULL 0
#define TASK_DETACHABLE 1
#define TASK_UNDETACHABLE 0

#define KMP_CANCEL_THREADS
#define KMP_THREAD_ATTR

// Android does not have pthread_cancel.  Undefine KMP_CANCEL_THREADS if being
// built on Android
#if defined(__ANDROID__)
#undef KMP_CANCEL_THREADS
#endif

// Some WASI targets (e.g., wasm32-wasi-threads) do not support thread
// cancellation.
#if KMP_OS_WASI
#undef KMP_CANCEL_THREADS
#endif

#if !KMP_OS_WASI
#include <signal.h>
#endif
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits>
#include <type_traits>
/* include <ctype.h> don't use; problems with /MD on Windows* OS NT due to bad
   Microsoft library. Some macros provided below to replace these functions  */
#ifndef __ABSOFT_WIN
#include <sys/types.h>
#endif
#include <limits.h>
#include <time.h>

#include <errno.h>

#include "kmp_os.h"

#include "kmp_safe_c_api.h"

#if KMP_STATS_ENABLED
class kmp_stats_list;
#endif

#if KMP_USE_HIER_SCHED
// Only include hierarchical scheduling if affinity is supported
#undef KMP_USE_HIER_SCHED
#define KMP_USE_HIER_SCHED KMP_AFFINITY_SUPPORTED
#endif

// OMPD_SKIP_HWLOC used in libompd/omp-icv.cpp to avoid OMPD depending on hwloc
#if KMP_USE_HWLOC && KMP_AFFINITY_SUPPORTED && !defined(OMPD_SKIP_HWLOC)
#include "hwloc.h"
#ifndef HWLOC_OBJ_NUMANODE
#define HWLOC_OBJ_NUMANODE HWLOC_OBJ_NODE
#endif
#ifndef HWLOC_OBJ_PACKAGE
#define HWLOC_OBJ_PACKAGE HWLOC_OBJ_SOCKET
#endif
#endif

#if KMP_ARCH_X86 || KMP_ARCH_X86_64
#include <xmmintrin.h>
#endif

// The below has to be defined before including "kmp_barrier.h".
#define KMP_INTERNAL_MALLOC(sz) malloc(sz)
#define KMP_INTERNAL_FREE(p) free(p)
#define KMP_INTERNAL_REALLOC(p, sz) realloc((p), (sz))
#define KMP_INTERNAL_CALLOC(n, sz) calloc((n), (sz))

#include "kmp_debug.h"
#include "kmp_lock.h"
#include "kmp_version.h"
#include "kmp_barrier.h"
#if USE_DEBUGGER
#error #include "kmp_debugger.h"
#endif
#include "kmp_i18n.h"

#define KMP_HANDLE_SIGNALS ((KMP_OS_UNIX && !KMP_OS_WASI) || KMP_OS_WINDOWS)

#include "kmp_wrapper_malloc.h"
#if KMP_OS_UNIX
#include <unistd.h>
#if !defined NSIG && defined _NSIG
#define NSIG _NSIG
#endif
#endif

#if KMP_OS_LINUX
#pragma weak clock_gettime
#endif

#if OMPT_SUPPORT
#include "ompt-internal.h"
#endif

#if OMPD_SUPPORT
#error #include "ompd-specific.h"
#endif

#ifndef UNLIKELY
#define UNLIKELY(x) (x)
#endif

// Affinity format function
#include "kmp_str.h"

// 0 - no fast memory allocation, alignment: 8-byte on x86, 16-byte on x64.
// 3 - fast allocation using sync, non-sync free lists of any size, non-self
// free lists of limited size.
#ifndef USE_FAST_MEMORY
#define USE_FAST_MEMORY 3
#endif

#ifndef KMP_NESTED_HOT_TEAMS
#define KMP_NESTED_HOT_TEAMS 0
#define USE_NESTED_HOT_ARG(x)
#else
#if KMP_NESTED_HOT_TEAMS
#define USE_NESTED_HOT_ARG(x) , x
#else
#define USE_NESTED_HOT_ARG(x)
#endif
#endif

// Assume using BGET compare_exchange instruction instead of lock by default.
#ifndef USE_CMP_XCHG_FOR_BGET
#define USE_CMP_XCHG_FOR_BGET 1
#endif

// Test to see if queuing lock is better than bootstrap lock for bget
// #ifndef USE_QUEUING_LOCK_FOR_BGET
// #define USE_QUEUING_LOCK_FOR_BGET
// #endif

#define KMP_NSEC_PER_SEC 1000000000L
#define KMP_USEC_PER_SEC 1000000L
#define KMP_NSEC_PER_USEC 1000L

/*!
@ingroup BASIC_TYPES
@{
*/

/*!
Values for bit flags used in the ident_t to describe the fields.
*/
enum {
  /*! Use trampoline for internal microtasks */
  KMP_IDENT_IMB = 0x01,
  /*! Use c-style ident structure */
  KMP_IDENT_KMPC = 0x02,
  /* 0x04 is no longer used */
  /*! Entry point generated by auto-parallelization */
  KMP_IDENT_AUTOPAR = 0x08,
  /*! Compiler generates atomic reduction option for kmpc_reduce* */
  KMP_IDENT_ATOMIC_REDUCE = 0x10,
  /*! To mark a 'barrier' directive in user code */
  KMP_IDENT_BARRIER_EXPL = 0x20,
  /*! To Mark implicit barriers. */
  KMP_IDENT_BARRIER_IMPL = 0x0040,
  KMP_IDENT_BARRIER_IMPL_MASK = 0x01C0,
  KMP_IDENT_BARRIER_IMPL_FOR = 0x0040,
  KMP_IDENT_BARRIER_IMPL_SECTIONS = 0x00C0,

  KMP_IDENT_BARRIER_IMPL_SINGLE = 0x0140,
  KMP_IDENT_BARRIER_IMPL_WORKSHARE = 0x01C0,

  /*! To mark a static loop in OMPT callbacks */
  KMP_IDENT_WORK_LOOP = 0x200,
  /*! To mark a sections directive in OMPT callbacks */
  KMP_IDENT_WORK_SECTIONS = 0x400,
  /*! To mark a distribute construct in OMPT callbacks */
  KMP_IDENT_WORK_DISTRIBUTE = 0x800,
  /*! Atomic hint; bottom four bits as omp_sync_hint_t. Top four reserved and
      not currently used. If one day we need more bits, then we can use
      an invalid combination of hints to mean that another, larger field
      should be used in a different flag. */
  KMP_IDENT_ATOMIC_HINT_MASK = 0xFF0000,
  KMP_IDENT_ATOMIC_HINT_UNCONTENDED = 0x010000,
  KMP_IDENT_ATOMIC_HINT_CONTENDED = 0x020000,
  KMP_IDENT_ATOMIC_HINT_NONSPECULATIVE = 0x040000,
  KMP_IDENT_ATOMIC_HINT_SPECULATIVE = 0x080000,
  KMP_IDENT_OPENMP_SPEC_VERSION_MASK = 0xFF000000
};

/*!
 * The ident structure that describes a source location.
 */
typedef struct ident {
  kmp_int32 reserved_1; /**<  might be used in Fortran; see above  */
  kmp_int32 flags; /**<  also f.flags; KMP_IDENT_xxx flags; KMP_IDENT_KMPC
                      identifies this union member  */
  kmp_int32 reserved_2; /**<  not really used in Fortran any more; see above */
#if USE_ITT_BUILD
/*  but currently used for storing region-specific ITT */
/*  contextual information. */
#endif /* USE_ITT_BUILD */
  kmp_int32 reserved_3; /**< source[4] in Fortran, do not use for C++  */
  char const *psource; /**< String describing the source location.
                       The string is composed of semi-colon separated fields
                       which describe the source file, the function and a pair
                       of line numbers that delimit the construct. */
  // Returns the OpenMP version in form major*10+minor (e.g., 50 for 5.0)
  kmp_int32 get_openmp_version() {
    return (((flags & KMP_IDENT_OPENMP_SPEC_VERSION_MASK) >> 24) & 0xFF);
  }
} ident_t;
/*!
@}
*/

// Some forward declarations.
typedef union kmp_team kmp_team_t;
typedef struct kmp_taskdata kmp_taskdata_t;
typedef union kmp_task_team kmp_task_team_t;
typedef union kmp_team kmp_team_p;
typedef union kmp_info kmp_info_p;
typedef union kmp_root kmp_root_p;

template <bool C = false, bool S = true> class kmp_flag_32;
template <bool C = false, bool S = true> class kmp_flag_64;
template <bool C = false, bool S = true> class kmp_atomic_flag_64;
class kmp_flag_oncore;

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */

/* Pack two 32-bit signed integers into a 64-bit signed integer */
/* ToDo: Fix word ordering for big-endian machines. */
#define KMP_PACK_64(HIGH_32, LOW_32)                                           \
  ((kmp_int64)((((kmp_uint64)(HIGH_32)) << 32) | (kmp_uint64)(LOW_32)))

// Generic string manipulation macros. Assume that _x is of type char *
#define SKIP_WS(_x)                                                            \
  {                                                                            \
    while (*(_x) == ' ' || *(_x) == '\t')                                      \
      (_x)++;                                                                  \
  }
#define SKIP_DIGITS(_x)                                                        \
  {                                                                            \
    while (*(_x) >= '0' && *(_x) <= '9')                                       \
      (_x)++;                                                                  \
  }
#define SKIP_TOKEN(_x)                                                         \
  {                                                                            \
    while ((*(_x) >= '0' && *(_x) <= '9') || (*(_x) >= 'a' && *(_x) <= 'z') || \
           (*(_x) >= 'A' && *(_x) <= 'Z') || *(_x) == '_')                     \
      (_x)++;                                                                  \
  }
#define SKIP_TO(_x, _c)                                                        \
  {                                                                            \
    while (*(_x) != '\0' && *(_x) != (_c))                                     \
      (_x)++;                                                                  \
  }

/* ------------------------------------------------------------------------ */

#define KMP_MAX(x, y) ((x) > (y) ? (x) : (y))
#define KMP_MIN(x, y) ((x) < (y) ? (x) : (y))

/* ------------------------------------------------------------------------ */
/* Enumeration types */

enum kmp_state_timer {
  ts_stop,
  ts_start,
  ts_pause,

  ts_last_state
};

enum dynamic_mode {
  dynamic_default,
#ifdef USE_LOAD_BALANCE
  dynamic_load_balance,
#endif /* USE_LOAD_BALANCE */
  dynamic_random,
  dynamic_thread_limit,
  dynamic_max
};

/* external schedule constants, duplicate enum omp_sched in omp.h in order to
 * not include it here */
#ifndef KMP_SCHED_TYPE_DEFINED
#define KMP_SCHED_TYPE_DEFINED
typedef enum kmp_sched {
  kmp_sched_lower = 0, // lower and upper bounds are for routine parameter check
  // Note: need to adjust __kmp_sch_map global array in case enum is changed
  kmp_sched_static = 1, // mapped to kmp_sch_static_chunked           (33)
  kmp_sched_dynamic = 2, // mapped to kmp_sch_dynamic_chunked          (35)
  kmp_sched_guided = 3, // mapped to kmp_sch_guided_chunked           (36)
  kmp_sched_auto = 4, // mapped to kmp_sch_auto                     (38)
  kmp_sched_upper_std = 5, // upper bound for standard schedules
  kmp_sched_lower_ext = 100, // lower bound of Intel extension schedules
  kmp_sched_trapezoidal = 101, // mapped to kmp_sch_trapezoidal (39)
#if KMP_STATIC_STEAL_ENABLED
  kmp_sched_static_steal = 102, // mapped to kmp_sch_static_steal (44)
#endif
  kmp_sched_upper,
  kmp_sched_default = kmp_sched_static, // default scheduling
  kmp_sched_monotonic = 0x80000000
} kmp_sched_t;
#endif

/*!
 @ingroup WORK_SHARING
 * Describes the loop schedule to be used for a parallel for loop.
 */
enum sched_type : kmp_int32 {
  kmp_sch_lower = 32, /**< lower bound for unordered values */
  kmp_sch_static_chunked = 33,
  kmp_sch_static = 34, /**< static unspecialized */
  kmp_sch_dynamic_chunked = 35,
  kmp_sch_guided_chunked = 36, /**< guided unspecialized */
  kmp_sch_runtime = 37,
  kmp_sch_auto = 38, /**< auto */
  kmp_sch_trapezoidal = 39,

  /* accessible only through KMP_SCHEDULE environment variable */
  kmp_sch_static_greedy = 40,
  kmp_sch_static_balanced = 41,
  /* accessible only through KMP_SCHEDULE environment variable */
  kmp_sch_guided_iterative_chunked = 42,
  kmp_sch_guided_analytical_chunked = 43,
  /* accessible only through KMP_SCHEDULE environment variable */
  kmp_sch_static_steal = 44,

  /* static with chunk adjustment (e.g., simd) */
  kmp_sch_static_balanced_chunked = 45,
  kmp_sch_guided_simd = 46, /**< guided with chunk adjustment */
  kmp_sch_runtime_simd = 47, /**< runtime with chunk adjustment */

  /* accessible only through KMP_SCHEDULE environment variable */
  kmp_sch_upper, /**< upper bound for unordered values */

  kmp_ord_lower = 64, /**< lower bound for ordered values, must be power of 2 */
  kmp_ord_static_chunked = 65,
  kmp_ord_static = 66, /**< ordered static unspecialized */
  kmp_ord_dynamic_chunked = 67,
  kmp_ord_guided_chunked = 68,
  kmp_ord_runtime = 69,
  kmp_ord_auto = 70, /**< ordered auto */
  kmp_ord_trapezoidal = 71,
  kmp_ord_upper, /**< upper bound for ordered values */

  /* Schedules for Distribute construct */
  kmp_distribute_static_chunked = 91, /**< distribute static chunked */
  kmp_distribute_static = 92, /**< distribute static unspecialized */

  /* For the "nomerge" versions, kmp_dispatch_next*() will always return a
     single iteration/chunk, even if the loop is serialized. For the schedule
     types listed above, the entire iteration vector is returned if the loop is
     serialized. This doesn't work for gcc/gcomp sections. */
  kmp_nm_lower = 160, /**< lower bound for nomerge values */

  kmp_nm_static_chunked =
      (kmp_sch_static_chunked - kmp_sch_lower + kmp_nm_lower),
  kmp_nm_static = 162, /**< static unspecialized */
  kmp_nm_dynamic_chunked = 163,
  kmp_nm_guided_chunked = 164, /**< guided unspecialized */
  kmp_nm_runtime = 165,
  kmp_nm_auto = 166, /**< auto */
  kmp_nm_trapezoidal = 167,

  /* accessible only through KMP_SCHEDULE environment variable */
  kmp_nm_static_greedy = 168,
  kmp_nm_static_balanced = 169,
  /* accessible only through KMP_SCHEDULE environment variable */
  kmp_nm_guided_iterative_chunked = 170,
  kmp_nm_guided_analytical_chunked = 171,
  kmp_nm_static_steal =
      172, /* accessible only through OMP_SCHEDULE environment variable */

  kmp_nm_ord_static_chunked = 193,
  kmp_nm_ord_static = 194, /**< ordered static unspecialized */
  kmp_nm_ord_dynamic_chunked = 195,
  kmp_nm_ord_guided_chunked = 196,
  kmp_nm_ord_runtime = 197,
  kmp_nm_ord_auto = 198, /**< auto */
  kmp_nm_ord_trapezoidal = 199,
  kmp_nm_upper, /**< upper bound for nomerge values */

  /* Support for OpenMP 4.5 monotonic and nonmonotonic schedule modifiers. Since
     we need to distinguish the three possible cases (no modifier, monotonic
     modifier, nonmonotonic modifier), we need separate bits for each modifier.
     The absence of monotonic does not imply nonmonotonic, especially since 4.5
     says that the behaviour of the "no modifier" case is implementation defined
     in 4.5, but will become "nonmonotonic" in 5.0.

     Since we're passing a full 32 bit value, we can use a couple of high bits
     for these flags; out of paranoia we avoid the sign bit.

     These modifiers can be or-ed into non-static schedules by the compiler to
     pass the additional information. They will be stripped early in the
     processing in __kmp_dispatch_init when setting up schedules, so most of the
     code won't ever see schedules with these bits set.  */
  kmp_sch_modifier_monotonic =
      (1 << 29), /**< Set if the monotonic schedule modifier was present */
  kmp_sch_modifier_nonmonotonic =
      (1 << 30), /**< Set if the nonmonotonic schedule modifier was present */

#define SCHEDULE_WITHOUT_MODIFIERS(s)                                          \
  (enum sched_type)(                                                           \
      (s) & ~(kmp_sch_modifier_nonmonotonic | kmp_sch_modifier_monotonic))
#define SCHEDULE_HAS_MONOTONIC(s) (((s)&kmp_sch_modifier_monotonic) != 0)
#define SCHEDULE_HAS_NONMONOTONIC(s) (((s)&kmp_sch_modifier_nonmonotonic) != 0)
#define SCHEDULE_HAS_NO_MODIFIERS(s)                                           \
  (((s) & (kmp_sch_modifier_nonmonotonic | kmp_sch_modifier_monotonic)) == 0)
#define SCHEDULE_GET_MODIFIERS(s)                                              \
  ((enum sched_type)(                                                          \
      (s) & (kmp_sch_modifier_nonmonotonic | kmp_sch_modifier_monotonic)))
#define SCHEDULE_SET_MODIFIERS(s, m)                                           \
  (s = (enum sched_type)((kmp_int32)s | (kmp_int32)m))
#define SCHEDULE_NONMONOTONIC 0
#define SCHEDULE_MONOTONIC 1

  kmp_sch_default = kmp_sch_static /**< default scheduling algorithm */
};

// Apply modifiers on internal kind to standard kind
static inline void
__kmp_sched_apply_mods_stdkind(kmp_sched_t *kind,
                               enum sched_type internal_kind) {
  if (SCHEDULE_HAS_MONOTONIC(internal_kind)) {
    *kind = (kmp_sched_t)((int)*kind | (int)kmp_sched_monotonic);
  }
}

// Apply modifiers on standard kind to internal kind
static inline void
__kmp_sched_apply_mods_intkind(kmp_sched_t kind,
                               enum sched_type *internal_kind) {
  if ((int)kind & (int)kmp_sched_monotonic) {
    *internal_kind = (enum sched_type)((int)*internal_kind |
                                       (int)kmp_sch_modifier_monotonic);
  }
}

// Get standard schedule without modifiers
static inline kmp_sched_t __kmp_sched_without_mods(kmp_sched_t kind) {
  return (kmp_sched_t)((int)kind & ~((int)kmp_sched_monotonic));
}

/* Type to keep runtime schedule set via OMP_SCHEDULE or omp_set_schedule() */
typedef union kmp_r_sched {
  struct {
    enum sched_type r_sched_type;
    int chunk;
  };
  kmp_int64 sched;
} kmp_r_sched_t;

extern enum sched_type __kmp_sch_map[]; // map OMP 3.0 schedule types with our
// internal schedule types

enum library_type {
  library_none,
  library_serial,
  library_turnaround,
  library_throughput
};

#if KMP_OS_LINUX
enum clock_function_type {
  clock_function_gettimeofday,
  clock_function_clock_gettime
};
#endif /* KMP_OS_LINUX */

#if KMP_MIC_SUPPORTED
enum mic_type { non_mic, mic1, mic2, mic3, dummy };
#endif

// OpenMP 3.1 - Nested num threads array
typedef struct kmp_nested_nthreads_t {
  int *nth;
  int size;
  int used;
} kmp_nested_nthreads_t;

extern kmp_nested_nthreads_t __kmp_nested_nth;

/* -- fast reduction stuff ------------------------------------------------ */

#undef KMP_FAST_REDUCTION_BARRIER
#define KMP_FAST_REDUCTION_BARRIER 1

#undef KMP_FAST_REDUCTION_CORE_DUO
#if KMP_ARCH_X86 || KMP_ARCH_X86_64
#define KMP_FAST_REDUCTION_CORE_DUO 1
#endif

enum _reduction_method {
  reduction_method_not_defined = 0,
  critical_reduce_block = (1 << 8),
  atomic_reduce_block = (2 << 8),
  tree_reduce_block = (3 << 8),
  empty_reduce_block = (4 << 8)
};

// Description of the packed_reduction_method variable:
// The packed_reduction_method variable consists of two enum types variables
// that are packed together into 0-th byte and 1-st byte:
// 0: (packed_reduction_method & 0x000000FF) is a 'enum barrier_type' value of
// barrier that will be used in fast reduction: bs_plain_barrier or
// bs_reduction_barrier
// 1: (packed_reduction_method & 0x0000FF00) is a reduction method that will
// be used in fast reduction;
// Reduction method is of 'enum _reduction_method' type and it's defined the way
// so that the bits of 0-th byte are empty, so no need to execute a shift
// instruction while packing/unpacking

#if KMP_FAST_REDUCTION_BARRIER
#define PACK_REDUCTION_METHOD_AND_BARRIER(reduction_method, barrier_type)      \
  ((reduction_method) | (barrier_type))

#define UNPACK_REDUCTION_METHOD(packed_reduction_method)                       \
  ((enum _reduction_method)((packed_reduction_method) & (0x0000FF00)))

#define UNPACK_REDUCTION_BARRIER(packed_reduction_method)                      \
  ((enum barrier_type)((packed_reduction_method) & (0x000000FF)))
#else
#define PACK_REDUCTION_METHOD_AND_BARRIER(reduction_method, barrier_type)      \
  (reduction_method)

#define UNPACK_REDUCTION_METHOD(packed_reduction_method)                       \
  (packed_reduction_method)

#define UNPACK_REDUCTION_BARRIER(packed_reduction_method) (bs_plain_barrier)
#endif

#define TEST_REDUCTION_METHOD(packed_reduction_method, which_reduction_block)  \
  ((UNPACK_REDUCTION_METHOD(packed_reduction_method)) ==                       \
   (which_reduction_block))

#if KMP_FAST_REDUCTION_BARRIER
#define TREE_REDUCE_BLOCK_WITH_REDUCTION_BARRIER                               \
  (PACK_REDUCTION_METHOD_AND_BARRIER(tree_reduce_block, bs_reduction_barrier))

#define TREE_REDUCE_BLOCK_WITH_PLAIN_BARRIER                                   \
  (PACK_REDUCTION_METHOD_AND_BARRIER(tree_reduce_block, bs_plain_barrier))
#endif

typedef int PACKED_REDUCTION_METHOD_T;

/* -- end of fast reduction stuff ----------------------------------------- */

#if KMP_OS_WINDOWS
#define USE_CBLKDATA
#if KMP_MSVC_COMPAT
#pragma warning(push)
#pragma warning(disable : 271 310)
#endif
#include <windows.h>
#if KMP_MSVC_COMPAT
#pragma warning(pop)
#endif
#endif

#if KMP_OS_UNIX
#if !KMP_OS_WASI
#include <dlfcn.h>
#endif
#include <pthread.h>
#endif

enum kmp_hw_t : int {
  KMP_HW_UNKNOWN = -1,
  KMP_HW_SOCKET = 0,
  KMP_HW_PROC_GROUP,
  KMP_HW_NUMA,
  KMP_HW_DIE,
  KMP_HW_LLC,
  KMP_HW_L3,
  KMP_HW_TILE,
  KMP_HW_MODULE,
  KMP_HW_L2,
  KMP_HW_L1,
  KMP_HW_CORE,
  KMP_HW_THREAD,
  KMP_HW_LAST
};

typedef enum kmp_hw_core_type_t {
  KMP_HW_CORE_TYPE_UNKNOWN = 0x0,
#if KMP_ARCH_X86 || KMP_ARCH_X86_64
  KMP_HW_CORE_TYPE_ATOM = 0x20,
  KMP_HW_CORE_TYPE_CORE = 0x40,
  KMP_HW_MAX_NUM_CORE_TYPES = 3,
#else
  KMP_HW_MAX_NUM_CORE_TYPES = 1,
#endif
} kmp_hw_core_type_t;

#define KMP_HW_MAX_NUM_CORE_EFFS 8

#define KMP_DEBUG_ASSERT_VALID_HW_TYPE(type)                                   \
  KMP_DEBUG_ASSERT(type >= (kmp_hw_t)0 && type < KMP_HW_LAST)
#define KMP_ASSERT_VALID_HW_TYPE(type)                                         \
  KMP_ASSERT(type >= (kmp_hw_t)0 && type < KMP_HW_LAST)

#define KMP_FOREACH_HW_TYPE(type)                                              \
  for (kmp_hw_t type = (kmp_hw_t)0; type < KMP_HW_LAST;                        \
       type = (kmp_hw_t)((int)type + 1))

const char *__kmp_hw_get_keyword(kmp_hw_t type, bool plural = false);
const char *__kmp_hw_get_catalog_string(kmp_hw_t type, bool plural = false);
const char *__kmp_hw_get_core_type_string(kmp_hw_core_type_t type);

/* Only Linux* OS and Windows* OS support thread affinity. */
#if KMP_AFFINITY_SUPPORTED

// GROUP_AFFINITY is already defined for _MSC_VER>=1600 (VS2010 and later).
#if KMP_OS_WINDOWS
#if _MSC_VER < 1600 && KMP_MSVC_COMPAT
typedef struct GROUP_AFFINITY {
  KAFFINITY Mask;
  WORD Group;
  WORD Reserved[3];
} GROUP_AFFINITY;
#endif /* _MSC_VER < 1600 */
#if KMP_GROUP_AFFINITY
extern int __kmp_num_proc_groups;
#else
static const int __kmp_num_proc_groups = 1;
#endif /* KMP_GROUP_AFFINITY */
typedef DWORD (*kmp_GetActiveProcessorCount_t)(WORD);
extern kmp_GetActiveProcessorCount_t __kmp_GetActiveProcessorCount;

typedef WORD (*kmp_GetActiveProcessorGroupCount_t)(void);
extern kmp_GetActiveProcessorGroupCount_t __kmp_GetActiveProcessorGroupCount;

typedef BOOL (*kmp_GetThreadGroupAffinity_t)(HANDLE, GROUP_AFFINITY *);
extern kmp_GetThreadGroupAffinity_t __kmp_GetThreadGroupAffinity;

typedef BOOL (*kmp_SetThreadGroupAffinity_t)(HANDLE, const GROUP_AFFINITY *,
                                             GROUP_AFFINITY *);
extern kmp_SetThreadGroupAffinity_t __kmp_SetThreadGroupAffinity;
#endif /* KMP_OS_WINDOWS */

#if KMP_USE_HWLOC && !defined(OMPD_SKIP_HWLOC)
extern hwloc_topology_t __kmp_hwloc_topology;
extern int __kmp_hwloc_error;
#endif

extern size_t __kmp_affin_mask_size;
#define KMP_AFFINITY_CAPABLE() (__kmp_affin_mask_size > 0)
#define KMP_AFFINITY_DISABLE() (__kmp_affin_mask_size = 0)
#define KMP_AFFINITY_ENABLE(mask_size) (__kmp_affin_mask_size = mask_size)
#define KMP_CPU_SET_ITERATE(i, mask)                                           \
  for (i = (mask)->begin(); (int)i != (mask)->end(); i = (mask)->next(i))
#define KMP_CPU_SET(i, mask) (mask)->set(i)
#define KMP_CPU_ISSET(i, mask) (mask)->is_set(i)
#define KMP_CPU_CLR(i, mask) (mask)->clear(i)
#define KMP_CPU_ZERO(mask) (mask)->zero()
#define KMP_CPU_ISEMPTY(mask) (mask)->empty()
#define KMP_CPU_COPY(dest, src) (dest)->copy(src)
#define KMP_CPU_AND(dest, src) (dest)->bitwise_and(src)
#define KMP_CPU_COMPLEMENT(max_bit_number, mask) (mask)->bitwise_not()
#define KMP_CPU_UNION(dest, src) (dest)->bitwise_or(src)
#define KMP_CPU_EQUAL(dest, src) (dest)->is_equal(src)
#define KMP_CPU_ALLOC(ptr) (ptr = __kmp_affinity_dispatch->allocate_mask())
#define KMP_CPU_FREE(ptr) __kmp_affinity_dispatch->deallocate_mask(ptr)
#define KMP_CPU_ALLOC_ON_STACK(ptr) KMP_CPU_ALLOC(ptr)
#define KMP_CPU_FREE_FROM_STACK(ptr) KMP_CPU_FREE(ptr)
#define KMP_CPU_INTERNAL_ALLOC(ptr) KMP_CPU_ALLOC(ptr)
#define KMP_CPU_INTERNAL_FREE(ptr) KMP_CPU_FREE(ptr)
#define KMP_CPU_INDEX(arr, i) __kmp_affinity_dispatch->index_mask_array(arr, i)
#define KMP_CPU_ALLOC_ARRAY(arr, n)                                            \
  (arr = __kmp_affinity_dispatch->allocate_mask_array(n))
#define KMP_CPU_FREE_ARRAY(arr, n)                                             \
  __kmp_affinity_dispatch->deallocate_mask_array(arr)
#define KMP_CPU_INTERNAL_ALLOC_ARRAY(arr, n) KMP_CPU_ALLOC_ARRAY(arr, n)
#define KMP_CPU_INTERNAL_FREE_ARRAY(arr, n) KMP_CPU_FREE_ARRAY(arr, n)
#define __kmp_get_system_affinity(mask, abort_bool)                            \
  (mask)->get_system_affinity(abort_bool)
#define __kmp_set_system_affinity(mask, abort_bool)                            \
  (mask)->set_system_affinity(abort_bool)
#define __kmp_get_proc_group(mask) (mask)->get_proc_group()

class KMPAffinity {
public:
  class Mask {
  public:
    void *operator new(size_t n);
    void operator delete(void *p);
    void *operator new[](size_t n);
    void operator delete[](void *p);
    virtual ~Mask() {}
    // Set bit i to 1
    virtual void set(int i) {}
    // Return bit i
    virtual bool is_set(int i) const { return false; }
    // Set bit i to 0
    virtual void clear(int i) {}
    // Zero out entire mask
    virtual void zero() {}
    // Check whether mask is empty
    virtual bool empty() const { return true; }
    // Copy src into this mask
    virtual void copy(const Mask *src) {}
    // this &= rhs
    virtual void bitwise_and(const Mask *rhs) {}
    // this |= rhs
    virtual void bitwise_or(const Mask *rhs) {}
    // this = ~this
    virtual void bitwise_not() {}
    // this == rhs
    virtual bool is_equal(const Mask *rhs) const { return false; }
    // API for iterating over an affinity mask
    // for (int i = mask->begin(); i != mask->end(); i = mask->next(i))
    virtual int begin() const { return 0; }
    virtual int end() const { return 0; }
    virtual int next(int previous) const { return 0; }
#if KMP_OS_WINDOWS
    virtual int set_process_affinity(bool abort_on_error) const { return -1; }
#endif
    // Set the system's affinity to this affinity mask's value
    virtual int set_system_affinity(bool abort_on_error) const { return -1; }
    // Set this affinity mask to the current system affinity
    virtual int get_system_affinity(bool abort_on_error) { return -1; }
    // Only 1 DWORD in the mask should have any procs set.
    // Return the appropriate index, or -1 for an invalid mask.
    virtual int get_proc_group() const { return -1; }
    int get_max_cpu() const {
      int cpu;
      int max_cpu = -1;
      KMP_CPU_SET_ITERATE(cpu, this) {
        if (cpu > max_cpu)
          max_cpu = cpu;
      }
      return max_cpu;
    }
  };
  void *operator new(size_t n);
  void operator delete(void *p);
  // Need virtual destructor
  virtual ~KMPAffinity() = default;
  // Determine if affinity is capable
  virtual void determine_capable(const char *env_var) {}
  // Bind the current thread to os proc
  virtual void bind_thread(int proc) {}
  // Factory functions to allocate/deallocate a mask
  virtual Mask *allocate_mask() { return nullptr; }
  virtual void deallocate_mask(Mask *m) {}
  virtual Mask *allocate_mask_array(int num) { return nullptr; }
  virtual void deallocate_mask_array(Mask *m) {}
  virtual Mask *index_mask_array(Mask *m, int index) { return nullptr; }
  static void pick_api();
  static void destroy_api();
  enum api_type {
    NATIVE_OS
#if KMP_USE_HWLOC
    ,
    HWLOC
#endif
  };
  virtual api_type get_api_type() const {
    KMP_ASSERT(0);
    return NATIVE_OS;
  }

private:
  static bool picked_api;
};

typedef KMPAffinity::Mask kmp_affin_mask_t;
extern KMPAffinity *__kmp_affinity_dispatch;

#ifndef KMP_OS_AIX
class kmp_affinity_raii_t {
  kmp_affin_mask_t *mask;
  bool restored;

public:
  kmp_affinity_raii_t(const kmp_affin_mask_t *new_mask = nullptr)
      : mask(nullptr), restored(false) {
    if (KMP_AFFINITY_CAPABLE()) {
      KMP_CPU_ALLOC(mask);
      KMP_ASSERT(mask != NULL);
      __kmp_get_system_affinity(mask, /*abort_on_error=*/true);
      if (new_mask)
        __kmp_set_system_affinity(new_mask, /*abort_on_error=*/true);
    }
  }
  void restore() {
    if (mask && KMP_AFFINITY_CAPABLE() && !restored) {
      __kmp_set_system_affinity(mask, /*abort_on_error=*/true);
      KMP_CPU_FREE(mask);
    }
    restored = true;
  }
  ~kmp_affinity_raii_t() { restore(); }
};
#endif // !KMP_OS_AIX

// Declare local char buffers with this size for printing debug and info
// messages, using __kmp_affinity_print_mask().
#define KMP_AFFIN_MASK_PRINT_LEN 1024

enum affinity_type {
  affinity_none = 0,
  affinity_physical,
  affinity_logical,
  affinity_compact,
  affinity_scatter,
  affinity_explicit,
  affinity_balanced,
  affinity_disabled, // not used outsize the env var parser
  affinity_default
};

enum affinity_top_method {
  affinity_top_method_all = 0, // try all (supported) methods, in order
#if KMP_ARCH_X86 || KMP_ARCH_X86_64
  affinity_top_method_apicid,
  affinity_top_method_x2apicid,
  affinity_top_method_x2apicid_1f,
#endif /* KMP_ARCH_X86 || KMP_ARCH_X86_64 */
  affinity_top_method_cpuinfo, // KMP_CPUINFO_FILE is usable on Windows* OS, too
#if KMP_GROUP_AFFINITY
  affinity_top_method_group,
#endif /* KMP_GROUP_AFFINITY */
  affinity_top_method_flat,
#if KMP_USE_HWLOC
  affinity_top_method_hwloc,
#endif
  affinity_top_method_default
};

#define affinity_respect_mask_default (2)

typedef struct kmp_affinity_flags_t {
  unsigned dups : 1;
  unsigned verbose : 1;
  unsigned warnings : 1;
  unsigned respect : 2;
  unsigned reset : 1;
  unsigned initialized : 1;
  unsigned core_types_gran : 1;
  unsigned core_effs_gran : 1;
  unsigned omp_places : 1;
  unsigned reserved : 22;
} kmp_affinity_flags_t;
KMP_BUILD_ASSERT(sizeof(kmp_affinity_flags_t) == 4);

typedef struct kmp_affinity_ids_t {
  int os_id;
  int ids[KMP_HW_LAST];
} kmp_affinity_ids_t;

typedef struct kmp_affinity_attrs_t {
  int core_type : 8;
  int core_eff : 8;
  unsigned valid : 1;
  unsigned reserved : 15;
} kmp_affinity_attrs_t;
#define KMP_AFFINITY_ATTRS_UNKNOWN                                             \
  { KMP_HW_CORE_TYPE_UNKNOWN, kmp_hw_attr_t::UNKNOWN_CORE_EFF, 0, 0 }

typedef struct kmp_affinity_t {
  char *proclist;
  enum affinity_type type;
  kmp_hw_t gran;
  int gran_levels;
  kmp_affinity_attrs_t core_attr_gran;
  int compact;
  int offset;
  kmp_affinity_flags_t flags;
  unsigned num_masks;
  kmp_affin_mask_t *masks;
  kmp_affinity_ids_t *ids;
  kmp_affinity_attrs_t *attrs;
  unsigned num_os_id_masks;
  kmp_affin_mask_t *os_id_masks;
  const char *env_var;
} kmp_affinity_t;

#define KMP_AFFINITY_INIT(env)                                                 \
  {                                                                            \
    nullptr, affinity_default, KMP_HW_UNKNOWN, -1, KMP_AFFINITY_ATTRS_UNKNOWN, \
        0, 0,                                                                  \
        {TRUE,  FALSE, TRUE, affinity_respect_mask_default, FALSE, FALSE,      \
         FALSE, FALSE, FALSE},                                                 \
        0, nullptr, nullptr, nullptr, 0, nullptr, env                          \
  }

extern enum affinity_top_method __kmp_affinity_top_method;
extern kmp_affinity_t __kmp_affinity;
extern kmp_affinity_t __kmp_hh_affinity;
extern kmp_affinity_t *__kmp_affinities[2];

extern void __kmp_affinity_bind_thread(int which);

extern kmp_affin_mask_t *__kmp_affin_fullMask;
extern kmp_affin_mask_t *__kmp_affin_origMask;
extern char *__kmp_cpuinfo_file;

#if KMP_WEIGHTED_ITERATIONS_SUPPORTED
extern int __kmp_first_osid_with_ecore;
#endif

#endif /* KMP_AFFINITY_SUPPORTED */

// This needs to be kept in sync with the values in omp.h !!!
typedef enum kmp_proc_bind_t {
  proc_bind_false = 0,
  proc_bind_true,
  proc_bind_primary,
  proc_bind_close,
  proc_bind_spread,
  proc_bind_intel, // use KMP_AFFINITY interface
  proc_bind_default
} kmp_proc_bind_t;

typedef struct kmp_nested_proc_bind_t {
  kmp_proc_bind_t *bind_types;
  int size;
  int used;
} kmp_nested_proc_bind_t;

extern kmp_nested_proc_bind_t __kmp_nested_proc_bind;
extern kmp_proc_bind_t __kmp_teams_proc_bind;

extern int __kmp_display_affinity;
extern char *__kmp_affinity_format;
static const size_t KMP_AFFINITY_FORMAT_SIZE = 512;
#if OMPT_SUPPORT
extern int __kmp_tool;
extern char *__kmp_tool_libraries;
#endif // OMPT_SUPPORT

#if KMP_AFFINITY_SUPPORTED
#define KMP_PLACE_ALL (-1)
#define KMP_PLACE_UNDEFINED (-2)
// Is KMP_AFFINITY is being used instead of OMP_PROC_BIND/OMP_PLACES?
#define KMP_AFFINITY_NON_PROC_BIND                                             \
  ((__kmp_nested_proc_bind.bind_types[0] == proc_bind_false ||                 \
    __kmp_nested_proc_bind.bind_types[0] == proc_bind_intel) &&                \
   (__kmp_affinity.num_masks > 0 || __kmp_affinity.type == affinity_balanced))
#endif /* KMP_AFFINITY_SUPPORTED */

extern int __kmp_affinity_num_places;

typedef enum kmp_cancel_kind_t {
  cancel_noreq = 0,
  cancel_parallel = 1,
  cancel_loop = 2,
  cancel_sections = 3,
  cancel_taskgroup = 4
} kmp_cancel_kind_t;

// KMP_HW_SUBSET support:
typedef struct kmp_hws_item {
  int num;
  int offset;
} kmp_hws_item_t;

extern kmp_hws_item_t __kmp_hws_socket;
extern kmp_hws_item_t __kmp_hws_die;
extern kmp_hws_item_t __kmp_hws_node;
extern kmp_hws_item_t __kmp_hws_tile;
extern kmp_hws_item_t __kmp_hws_core;
extern kmp_hws_item_t __kmp_hws_proc;
extern int __kmp_hws_requested;
extern int __kmp_hws_abs_flag; // absolute or per-item number requested

/* ------------------------------------------------------------------------ */

#define KMP_PAD(type, sz)                                                      \
  (sizeof(type) + (sz - ((sizeof(type) - 1) % (sz)) - 1))

// We need to avoid using -1 as a GTID as +1 is added to the gtid
// when storing it in a lock, and the value 0 is reserved.
#define KMP_GTID_DNE (-2) /* Does not exist */
#define KMP_GTID_SHUTDOWN (-3) /* Library is shutting down */
#define KMP_GTID_MONITOR (-4) /* Monitor thread ID */
#define KMP_GTID_UNKNOWN (-5) /* Is not known */
#define KMP_GTID_MIN (-6) /* Minimal gtid for low bound check in DEBUG */

/* OpenMP 5.0 Memory Management support */

#ifndef __OMP_H
// Duplicate type definitions from omp.h
typedef uintptr_t omp_uintptr_t;

typedef enum {
  omp_atk_sync_hint = 1,
  omp_atk_alignment = 2,
  omp_atk_access = 3,
  omp_atk_pool_size = 4,
  omp_atk_fallback = 5,
  omp_atk_fb_data = 6,
  omp_atk_pinned = 7,
  omp_atk_partition = 8
} omp_alloctrait_key_t;

typedef enum {
  omp_atv_false = 0,
  omp_atv_true = 1,
  omp_atv_contended = 3,
  omp_atv_uncontended = 4,
  omp_atv_serialized = 5,
  omp_atv_sequential = omp_atv_serialized, // (deprecated)
  omp_atv_private = 6,
  omp_atv_all = 7,
  omp_atv_thread = 8,
  omp_atv_pteam = 9,
  omp_atv_cgroup = 10,
  omp_atv_default_mem_fb = 11,
  omp_atv_null_fb = 12,
  omp_atv_abort_fb = 13,
  omp_atv_allocator_fb = 14,
  omp_atv_environment = 15,
  omp_atv_nearest = 16,
  omp_atv_blocked = 17,
  omp_atv_interleaved = 18
} omp_alloctrait_value_t;
#define omp_atv_default ((omp_uintptr_t)-1)

typedef void *omp_memspace_handle_t;
extern omp_memspace_handle_t const omp_default_mem_space;
extern omp_memspace_handle_t const omp_large_cap_mem_space;
extern omp_memspace_handle_t const omp_const_mem_space;
extern omp_memspace_handle_t const omp_high_bw_mem_space;
extern omp_memspace_handle_t const omp_low_lat_mem_space;
extern omp_memspace_handle_t const llvm_omp_target_host_mem_space;
extern omp_memspace_handle_t const llvm_omp_target_shared_mem_space;
extern omp_memspace_handle_t const llvm_omp_target_device_mem_space;

typedef struct {
  omp_alloctrait_key_t key;
  omp_uintptr_t value;
} omp_alloctrait_t;

typedef void *omp_allocator_handle_t;
extern omp_allocator_handle_t const omp_null_allocator;
extern omp_allocator_handle_t const omp_default_mem_alloc;
extern omp_allocator_handle_t const omp_large_cap_mem_alloc;
extern omp_allocator_handle_t const omp_const_mem_alloc;
extern omp_allocator_handle_t const omp_high_bw_mem_alloc;
extern omp_allocator_handle_t const omp_low_lat_mem_alloc;
extern omp_allocator_handle_t const omp_cgroup_mem_alloc;
extern omp_allocator_handle_t const omp_pteam_mem_alloc;
extern omp_allocator_handle_t const omp_thread_mem_alloc;
extern omp_allocator_handle_t const llvm_omp_target_host_mem_alloc;
extern omp_allocator_handle_t const llvm_omp_target_shared_mem_alloc;
extern omp_allocator_handle_t const llvm_omp_target_device_mem_alloc;
extern omp_allocator_handle_t const kmp_max_mem_alloc;
extern omp_allocator_handle_t __kmp_def_allocator;

// end of duplicate type definitions from omp.h
#endif

extern int __kmp_memkind_available;

typedef omp_memspace_handle_t kmp_memspace_t; // placeholder

typedef struct kmp_allocator_t {
  omp_memspace_handle_t memspace;
  void **memkind; // pointer to memkind
  size_t alignment;
  omp_alloctrait_value_t fb;
  kmp_allocator_t *fb_data;
  kmp_uint64 pool_size;
  kmp_uint64 pool_used;
  bool pinned;
} kmp_allocator_t;

extern omp_allocator_handle_t __kmpc_init_allocator(int gtid,
                                                    omp_memspace_handle_t,
                                                    int ntraits,
                                                    omp_alloctrait_t traits[]);
extern void __kmpc_destroy_allocator(int gtid, omp_allocator_handle_t al);
extern void __kmpc_set_default_allocator(int gtid, omp_allocator_handle_t al);
extern omp_allocator_handle_t __kmpc_get_default_allocator(int gtid);
// external interfaces, may be used by compiler
extern void *__kmpc_alloc(int gtid, size_t sz, omp_allocator_handle_t al);
extern void *__kmpc_aligned_alloc(int gtid, size_t align, size_t sz,
                                  omp_allocator_handle_t al);
extern void *__kmpc_calloc(int gtid, size_t nmemb, size_t sz,
                           omp_allocator_handle_t al);
extern void *__kmpc_realloc(int gtid, void *ptr, size_t sz,
                            omp_allocator_handle_t al,
                            omp_allocator_handle_t free_al);
extern void __kmpc_free(int gtid, void *ptr, omp_allocator_handle_t al);
// internal interfaces, contain real implementation
extern void *__kmp_alloc(int gtid, size_t align, size_t sz,
                         omp_allocator_handle_t al);
extern void *__kmp_calloc(int gtid, size_t align, size_t nmemb, size_t sz,
                          omp_allocator_handle_t al);
extern void *__kmp_realloc(int gtid, void *ptr, size_t sz,
                           omp_allocator_handle_t al,
                           omp_allocator_handle_t free_al);
extern void ___kmpc_free(int gtid, void *ptr, omp_allocator_handle_t al);

extern void __kmp_init_memkind();
extern void __kmp_fini_memkind();
extern void __kmp_init_target_mem();

/* ------------------------------------------------------------------------ */

#if ENABLE_LIBOMPTARGET
extern void __kmp_init_target_task();
#endif

/* ------------------------------------------------------------------------ */

#define KMP_UINT64_MAX                                                         \
  (~((kmp_uint64)1 << ((sizeof(kmp_uint64) * (1 << 3)) - 1)))

#define KMP_MIN_NTH 1

#ifndef KMP_MAX_NTH
#if defined(PTHREAD_THREADS_MAX) && PTHREAD_THREADS_MAX < INT_MAX
#define KMP_MAX_NTH PTHREAD_THREADS_MAX
#else
#ifdef __ve__
// VE's pthread supports only up to 64 threads per a VE process.
// Please check p. 14 of following documentation for more details.
// https://sxauroratsubasa.sakura.ne.jp/documents/veos/en/VEOS_high_level_design.pdf
#define KMP_MAX_NTH 64
#else
#define KMP_MAX_NTH INT_MAX
#endif
#endif
#endif /* KMP_MAX_NTH */

#ifdef PTHREAD_STACK_MIN
#define KMP_MIN_STKSIZE ((size_t)PTHREAD_STACK_MIN)
#else
#define KMP_MIN_STKSIZE ((size_t)(32 * 1024))
#endif

#if KMP_OS_AIX && KMP_ARCH_PPC
#define KMP_MAX_STKSIZE 0x10000000 /* 256Mb max size on 32-bit AIX */
#else
#define KMP_MAX_STKSIZE (~((size_t)1 << ((sizeof(size_t) * (1 << 3)) - 1)))
#endif

#if KMP_ARCH_X86
#define KMP_DEFAULT_STKSIZE ((size_t)(2 * 1024 * 1024))
#elif KMP_ARCH_X86_64
#define KMP_DEFAULT_STKSIZE ((size_t)(4 * 1024 * 1024))
#define KMP_BACKUP_STKSIZE ((size_t)(2 * 1024 * 1024))
#elif KMP_ARCH_VE
// Minimum stack size for pthread for VE is 4MB.
//   https://www.hpc.nec/documents/veos/en/glibc/Difference_Points_glibc.htm
#define KMP_DEFAULT_STKSIZE ((size_t)(4 * 1024 * 1024))
#elif KMP_OS_AIX
// The default stack size for worker threads on AIX is 4MB.
#define KMP_DEFAULT_STKSIZE ((size_t)(4 * 1024 * 1024))
#else
#define KMP_DEFAULT_STKSIZE ((size_t)(1024 * 1024))
#endif

#define KMP_DEFAULT_MALLOC_POOL_INCR ((size_t)(1024 * 1024))
#define KMP_MIN_MALLOC_POOL_INCR ((size_t)(4 * 1024))
#define KMP_MAX_MALLOC_POOL_INCR                                               \
  (~((size_t)1 << ((sizeof(size_t) * (1 << 3)) - 1)))

#define KMP_MIN_STKOFFSET (0)
#define KMP_MAX_STKOFFSET KMP_MAX_STKSIZE
#if KMP_OS_DARWIN
#define KMP_DEFAULT_STKOFFSET KMP_MIN_STKOFFSET
#else
#define KMP_DEFAULT_STKOFFSET CACHE_LINE
#endif

#define KMP_MIN_STKPADDING (0)
#define KMP_MAX_STKPADDING (2 * 1024 * 1024)

#define KMP_BLOCKTIME_MULTIPLIER                                               \
  (1000000) /* number of blocktime units per second */
#define KMP_MIN_BLOCKTIME (0)
#define KMP_MAX_BLOCKTIME                                                      \
  (INT_MAX) /* Must be this for "infinite" setting the work */

/* __kmp_blocktime is in microseconds */
#define KMP_DEFAULT_BLOCKTIME (__kmp_is_hybrid_cpu() ? (0) : (200000))

#if KMP_USE_MONITOR
#define KMP_DEFAULT_MONITOR_STKSIZE ((size_t)(1024 * 1024))
#define KMP_MIN_MONITOR_WAKEUPS (1) // min times monitor wakes up per second
#define KMP_MAX_MONITOR_WAKEUPS (1000) // max times monitor can wake up per sec

/* Calculate new number of monitor wakeups for a specific block time based on
   previous monitor_wakeups. Only allow increasing number of wakeups */
#define KMP_WAKEUPS_FROM_BLOCKTIME(blocktime, monitor_wakeups)                 \
  (((blocktime) == KMP_MAX_BLOCKTIME)   ? (monitor_wakeups)                    \
   : ((blocktime) == KMP_MIN_BLOCKTIME) ? KMP_MAX_MONITOR_WAKEUPS              \
   : ((monitor_wakeups) > (KMP_BLOCKTIME_MULTIPLIER / (blocktime)))            \
       ? (monitor_wakeups)                                                     \
       : (KMP_BLOCKTIME_MULTIPLIER) / (blocktime))

/* Calculate number of intervals for a specific block time based on
   monitor_wakeups */
#define KMP_INTERVALS_FROM_BLOCKTIME(blocktime, monitor_wakeups)               \
  (((blocktime) + (KMP_BLOCKTIME_MULTIPLIER / (monitor_wakeups)) - 1) /        \
   (KMP_BLOCKTIME_MULTIPLIER / (monitor_wakeups)))
#else
#define KMP_BLOCKTIME(team, tid)                                               \
  (get__bt_set(team, tid) ? get__blocktime(team, tid) : __kmp_dflt_blocktime)
#if KMP_OS_UNIX && (KMP_ARCH_X86 || KMP_ARCH_X86_64)
// HW TSC is used to reduce overhead (clock tick instead of nanosecond).
extern kmp_uint64 __kmp_ticks_per_msec;
extern kmp_uint64 __kmp_ticks_per_usec;
#if KMP_COMPILER_ICC || KMP_COMPILER_ICX
#define KMP_NOW() ((kmp_uint64)_rdtsc())
#else
#define KMP_NOW() __kmp_hardware_timestamp()
#endif
#define KMP_BLOCKTIME_INTERVAL(team, tid)                                      \
  ((kmp_uint64)KMP_BLOCKTIME(team, tid) * __kmp_ticks_per_usec)
#define KMP_BLOCKING(goal, count) ((goal) > KMP_NOW())
#else
// System time is retrieved sporadically while blocking.
extern kmp_uint64 __kmp_now_nsec();
#define KMP_NOW() __kmp_now_nsec()
#define KMP_BLOCKTIME_INTERVAL(team, tid)                                      \
  ((kmp_uint64)KMP_BLOCKTIME(team, tid) * (kmp_uint64)KMP_NSEC_PER_USEC)
#define KMP_BLOCKING(goal, count) ((count) % 1000 != 0 || (goal) > KMP_NOW())
#endif
#endif // KMP_USE_MONITOR

#define KMP_MIN_STATSCOLS 40
#define KMP_MAX_STATSCOLS 4096
#define KMP_DEFAULT_STATSCOLS 80

#define KMP_MIN_INTERVAL 0
#define KMP_MAX_INTERVAL (INT_MAX - 1)
#define KMP_DEFAULT_INTERVAL 0

#define KMP_MIN_CHUNK 1
#define KMP_MAX_CHUNK (INT_MAX - 1)
#define KMP_DEFAULT_CHUNK 1

#define KMP_MIN_DISP_NUM_BUFF 1
#define KMP_DFLT_DISP_NUM_BUFF 7
#define KMP_MAX_DISP_NUM_BUFF 4096

#define KMP_MAX_ORDERED 8

#define KMP_MAX_FIELDS 32

#define KMP_MAX_BRANCH_BITS 31

#define KMP_MAX_ACTIVE_LEVELS_LIMIT INT_MAX

#define KMP_MAX_DEFAULT_DEVICE_LIMIT INT_MAX

#define KMP_MAX_TASK_PRIORITY_LIMIT INT_MAX

/* Minimum number of threads before switch to TLS gtid (experimentally
   determined) */
/* josh TODO: what about OS X* tuning? */
#if KMP_ARCH_X86 || KMP_ARCH_X86_64
#define KMP_TLS_GTID_MIN 5
#else
#define KMP_TLS_GTID_MIN INT_MAX
#endif

#define KMP_MASTER_TID(tid) (0 == (tid))
#define KMP_WORKER_TID(tid) (0 != (tid))

#define KMP_MASTER_GTID(gtid) (0 == __kmp_tid_from_gtid((gtid)))
#define KMP_WORKER_GTID(gtid) (0 != __kmp_tid_from_gtid((gtid)))
#define KMP_INITIAL_GTID(gtid) (0 == (gtid))

#ifndef TRUE
#define FALSE 0
#define TRUE (!FALSE)
#endif

/* NOTE: all of the following constants must be even */

#if KMP_OS_WINDOWS
#define KMP_INIT_WAIT 64U /* initial number of spin-tests   */
#define KMP_NEXT_WAIT 32U /* susequent number of spin-tests */
#elif KMP_OS_LINUX
#define KMP_INIT_WAIT 1024U /* initial number of spin-tests   */
#define KMP_NEXT_WAIT 512U /* susequent number of spin-tests */
#elif KMP_OS_DARWIN
/* TODO: tune for KMP_OS_DARWIN */
#define KMP_INIT_WAIT 1024U /* initial number of spin-tests   */
#define KMP_NEXT_WAIT 512U /* susequent number of spin-tests */
#elif KMP_OS_DRAGONFLY
/* TODO: tune for KMP_OS_DRAGONFLY */
#define KMP_INIT_WAIT 1024U /* initial number of spin-tests   */
#define KMP_NEXT_WAIT 512U /* susequent number of spin-tests */
#elif KMP_OS_FREEBSD
/* TODO: tune for KMP_OS_FREEBSD */
#define KMP_INIT_WAIT 1024U /* initial number of spin-tests   */
#define KMP_NEXT_WAIT 512U /* susequent number of spin-tests */
#elif KMP_OS_NETBSD
/* TODO: tune for KMP_OS_NETBSD */
#define KMP_INIT_WAIT 1024U /* initial number of spin-tests   */
#define KMP_NEXT_WAIT 512U /* susequent number of spin-tests */
#elif KMP_OS_OPENBSD
/* TODO: tune for KMP_OS_OPENBSD */
#define KMP_INIT_WAIT 1024U /* initial number of spin-tests   */
#define KMP_NEXT_WAIT 512U /* susequent number of spin-tests */
#elif KMP_OS_HURD
/* TODO: tune for KMP_OS_HURD */
#define KMP_INIT_WAIT 1024U /* initial number of spin-tests   */
#define KMP_NEXT_WAIT 512U /* susequent number of spin-tests */
#elif KMP_OS_SOLARIS
/* TODO: tune for KMP_OS_SOLARIS */
#define KMP_INIT_WAIT 1024U /* initial number of spin-tests   */
#define KMP_NEXT_WAIT 512U /* susequent number of spin-tests */
#elif KMP_OS_WASI
/* TODO: tune for KMP_OS_WASI */
#define KMP_INIT_WAIT 1024U /* initial number of spin-tests   */
#define KMP_NEXT_WAIT 512U /* susequent number of spin-tests */
#elif KMP_OS_AIX
/* TODO: tune for KMP_OS_AIX */
#define KMP_INIT_WAIT 1024U /* initial number of spin-tests   */
#define KMP_NEXT_WAIT 512U /* susequent number of spin-tests */
#endif

#if KMP_ARCH_X86 || KMP_ARCH_X86_64
typedef struct kmp_cpuid {
  kmp_uint32 eax;
  kmp_uint32 ebx;
  kmp_uint32 ecx;
  kmp_uint32 edx;
} kmp_cpuid_t;

typedef struct kmp_cpuinfo_flags_t {
  unsigned sse2 : 1; // 0 if SSE2 instructions are not supported, 1 otherwise.
  unsigned rtm : 1; // 0 if RTM instructions are not supported, 1 otherwise.
  unsigned hybrid : 1;
  unsigned reserved : 29; // Ensure size of 32 bits
} kmp_cpuinfo_flags_t;

typedef struct kmp_cpuinfo {
  int initialized; // If 0, other fields are not initialized.
  int signature; // CPUID(1).EAX
  int family; // CPUID(1).EAX[27:20]+CPUID(1).EAX[11:8] (Extended Family+Family)
  int model; // ( CPUID(1).EAX[19:16] << 4 ) + CPUID(1).EAX[7:4] ( ( Extended
  // Model << 4 ) + Model)
  int stepping; // CPUID(1).EAX[3:0] ( Stepping )
  kmp_cpuinfo_flags_t flags;
  int apic_id;
  kmp_uint64 frequency; // Nominal CPU frequency in Hz.
  char name[3 * sizeof(kmp_cpuid_t)]; // CPUID(0x80000002,0x80000003,0x80000004)
} kmp_cpuinfo_t;

extern void __kmp_query_cpuid(kmp_cpuinfo_t *p);

#if KMP_OS_UNIX
// subleaf is only needed for cache and topology discovery and can be set to
// zero in most cases
static inline void __kmp_x86_cpuid(int leaf, int subleaf, struct kmp_cpuid *p) {
  __asm__ __volatile__("cpuid"
                       : "=a"(p->eax), "=b"(p->ebx), "=c"(p->ecx), "=d"(p->edx)
                       : "a"(leaf), "c"(subleaf));
}
// Load p into FPU control word
static inline void __kmp_load_x87_fpu_control_word(const kmp_int16 *p) {
  __asm__ __volatile__("fldcw %0" : : "m"(*p));
}
// Store FPU control word into p
static inline void __kmp_store_x87_fpu_control_word(kmp_int16 *p) {
  __asm__ __volatile__("fstcw %0" : "=m"(*p));
}
static inline void __kmp_clear_x87_fpu_status_word() {
#if KMP_MIC
  // 32-bit protected mode x87 FPU state
  struct x87_fpu_state {
    unsigned cw;
    unsigned sw;
    unsigned tw;
    unsigned fip;
    unsigned fips;
    unsigned fdp;
    unsigned fds;
  };
  struct x87_fpu_state fpu_state = {0, 0, 0, 0, 0, 0, 0};
  __asm__ __volatile__("fstenv %0\n\t" // store FP env
                       "andw $0x7f00, %1\n\t" // clear 0-7,15 bits of FP SW
                       "fldenv %0\n\t" // load FP env back
                       : "+m"(fpu_state), "+m"(fpu_state.sw));
#else
  __asm__ __volatile__("fnclex");
#endif // KMP_MIC
}
#if __SSE__
static inline void __kmp_load_mxcsr(const kmp_uint32 *p) { _mm_setcsr(*p); }
static inline void __kmp_store_mxcsr(kmp_uint32 *p) { *p = _mm_getcsr(); }
#else
static inline void __kmp_load_mxcsr(const kmp_uint32 *p) {}
static inline void __kmp_store_mxcsr(kmp_uint32 *p) { *p = 0; }
#endif
#else
// Windows still has these as external functions in assembly file
extern void __kmp_x86_cpuid(int mode, int mode2, struct kmp_cpuid *p);
extern void __kmp_load_x87_fpu_control_word(const kmp_int16 *p);
extern void __kmp_store_x87_fpu_control_word(kmp_int16 *p);
extern void __kmp_clear_x87_fpu_status_word();
static inline void __kmp_load_mxcsr(const kmp_uint32 *p) { _mm_setcsr(*p); }
static inline void __kmp_store_mxcsr(kmp_uint32 *p) { *p = _mm_getcsr(); }
#endif // KMP_OS_UNIX

#define KMP_X86_MXCSR_MASK 0xffffffc0 /* ignore status flags (6 lsb) */

// User-level Monitor/Mwait
#if KMP_HAVE_UMWAIT
// We always try for UMWAIT first
#if KMP_HAVE_WAITPKG_INTRINSICS
#if KMP_HAVE_IMMINTRIN_H
#include <immintrin.h>
#elif KMP_HAVE_INTRIN_H
#include <intrin.h>
#endif
#endif // KMP_HAVE_WAITPKG_INTRINSICS

KMP_ATTRIBUTE_TARGET_WAITPKG
static inline int __kmp_tpause(uint32_t hint, uint64_t counter) {
#if !KMP_HAVE_WAITPKG_INTRINSICS
  uint32_t timeHi = uint32_t(counter >> 32);
  uint32_t timeLo = uint32_t(counter & 0xffffffff);
  char flag;
  __asm__ volatile("#tpause\n.byte 0x66, 0x0F, 0xAE, 0xF1\n"
                   "setb   %0"
                   // The "=q" restraint means any register accessible as rl
                   //   in 32-bit mode: a, b, c, and d;
                   //   in 64-bit mode: any integer register
                   : "=q"(flag)
                   : "a"(timeLo), "d"(timeHi), "c"(hint)
                   :);
  return flag;
#else
  return _tpause(hint, counter);
#endif
}
KMP_ATTRIBUTE_TARGET_WAITPKG
static inline void __kmp_umonitor(void *cacheline) {
#if !KMP_HAVE_WAITPKG_INTRINSICS
  __asm__ volatile("# umonitor\n.byte 0xF3, 0x0F, 0xAE, 0x01 "
                   :
                   : "a"(cacheline)
                   :);
#else
  _umonitor(cacheline);
#endif
}
KMP_ATTRIBUTE_TARGET_WAITPKG
static inline int __kmp_umwait(uint32_t hint, uint64_t counter) {
#if !KMP_HAVE_WAITPKG_INTRINSICS
  uint32_t timeHi = uint32_t(counter >> 32);
  uint32_t timeLo = uint32_t(counter & 0xffffffff);
  char flag;
  __asm__ volatile("#umwait\n.byte 0xF2, 0x0F, 0xAE, 0xF1\n"
                   "setb   %0"
                   // The "=q" restraint means any register accessible as rl
                   //   in 32-bit mode: a, b, c, and d;
                   //   in 64-bit mode: any integer register
                   : "=q"(flag)
                   : "a"(timeLo), "d"(timeHi), "c"(hint)
                   :);
  return flag;
#else
  return _umwait(hint, counter);
#endif
}
#elif KMP_HAVE_MWAIT
#if KMP_OS_UNIX
#include <pmmintrin.h>
#else
#include <intrin.h>
#endif
#if KMP_OS_UNIX
__attribute__((target("sse3")))
#endif
static inline void
__kmp_mm_monitor(void *cacheline, unsigned extensions, unsigned hints) {
  _mm_monitor(cacheline, extensions, hints);
}
#if KMP_OS_UNIX
__attribute__((target("sse3")))
#endif
static inline void
__kmp_mm_mwait(unsigned extensions, unsigned hints) {
  _mm_mwait(extensions, hints);
}
#endif // KMP_HAVE_UMWAIT

#if KMP_ARCH_X86
extern void __kmp_x86_pause(void);
#elif KMP_MIC
// Performance testing on KNC (C0QS-7120 P/A/X/D, 61-core, 16 GB Memory) showed
// regression after removal of extra PAUSE from spin loops. Changing
// the delay from 100 to 300 showed even better performance than double PAUSE
// on Spec OMP2001 and LCPC tasking tests, no regressions on EPCC.
static inline void __kmp_x86_pause(void) { _mm_delay_32(300); }
#else
static inline void __kmp_x86_pause(void) { _mm_pause(); }
#endif
#define KMP_CPU_PAUSE() __kmp_x86_pause()
#elif KMP_ARCH_PPC64
#define KMP_PPC64_PRI_LOW() __asm__ volatile("or 1, 1, 1")
#define KMP_PPC64_PRI_MED() __asm__ volatile("or 2, 2, 2")
#define KMP_PPC64_PRI_LOC_MB() __asm__ volatile("" : : : "memory")
#define KMP_CPU_PAUSE()                                                        \
  do {                                                                         \
    KMP_PPC64_PRI_LOW();                                                       \
    KMP_PPC64_PRI_MED();                                                       \
    KMP_PPC64_PRI_LOC_MB();                                                    \
  } while (0)
#else
#define KMP_CPU_PAUSE() /* nothing to do */
#endif

#define KMP_INIT_YIELD(count)                                                  \
  { (count) = __kmp_yield_init; }

#define KMP_INIT_BACKOFF(time)                                                 \
  { (time) = __kmp_pause_init; }

#define KMP_OVERSUBSCRIBED                                                     \
  (TCR_4(__kmp_nth) > (__kmp_avail_proc ? __kmp_avail_proc : __kmp_xproc))

#define KMP_TRY_YIELD                                                          \
  ((__kmp_use_yield == 1) || (__kmp_use_yield == 2 && (KMP_OVERSUBSCRIBED)))

#define KMP_TRY_YIELD_OVERSUB                                                  \
  ((__kmp_use_yield == 1 || __kmp_use_yield == 2) && (KMP_OVERSUBSCRIBED))

#define KMP_YIELD(cond)                                                        \
  {                                                                            \
    KMP_CPU_PAUSE();                                                           \
    if ((cond) && (KMP_TRY_YIELD))                                             \
      __kmp_yield();                                                           \
  }

#define KMP_YIELD_OVERSUB()                                                    \
  {                                                                            \
    KMP_CPU_PAUSE();                                                           \
    if ((KMP_TRY_YIELD_OVERSUB))                                               \
      __kmp_yield();                                                           \
  }

// Note the decrement of 2 in the following Macros. With KMP_LIBRARY=turnaround,
// there should be no yielding since initial value from KMP_INIT_YIELD() is odd.
#define KMP_YIELD_SPIN(count)                                                  \
  {                                                                            \
    KMP_CPU_PAUSE();                                                           \
    if (KMP_TRY_YIELD) {                                                       \
      (count) -= 2;                                                            \
      if (!(count)) {                                                          \
        __kmp_yield();                                                         \
        (count) = __kmp_yield_next;                                            \
      }                                                                        \
    }                                                                          \
  }

// If TPAUSE is available & enabled, use it. If oversubscribed, use the slower
// (C0.2) state, which improves performance of other SMT threads on the same
// core, otherwise, use the fast (C0.1) default state, or whatever the user has
// requested. Uses a timed TPAUSE, and exponential backoff. If TPAUSE isn't
// available, fall back to the regular CPU pause and yield combination.
#if KMP_HAVE_UMWAIT
#define KMP_TPAUSE_MAX_MASK ((kmp_uint64)0xFFFF)
#define KMP_YIELD_OVERSUB_ELSE_SPIN(count, time)                               \
  {                                                                            \
    if (__kmp_tpause_enabled) {                                                \
      if (KMP_OVERSUBSCRIBED) {                                                \
        __kmp_tpause(0, (time));                                               \
      } else {                                                                 \
        __kmp_tpause(__kmp_tpause_hint, (time));                               \
      }                                                                        \
      (time) = (time << 1 | 1) & KMP_TPAUSE_MAX_MASK;                          \
    } else {                                                                   \
      KMP_CPU_PAUSE();                                                         \
      if ((KMP_TRY_YIELD_OVERSUB)) {                                           \
        __kmp_yield();                                                         \
      } else if (__kmp_use_yield == 1) {                                       \
        (count) -= 2;                                                          \
        if (!(count)) {                                                        \
          __kmp_yield();                                                       \
          (count) = __kmp_yield_next;                                          \
        }                                                                      \
      }                                                                        \
    }                                                                          \
  }
#else
#define KMP_YIELD_OVERSUB_ELSE_SPIN(count, time)                               \
  {                                                                            \
    KMP_CPU_PAUSE();                                                           \
    if ((KMP_TRY_YIELD_OVERSUB))                                               \
      __kmp_yield();                                                           \
    else if (__kmp_use_yield == 1) {                                           \
      (count) -= 2;                                                            \
      if (!(count)) {                                                          \
        __kmp_yield();                                                         \
        (count) = __kmp_yield_next;                                            \
      }                                                                        \
    }                                                                          \
  }
#endif // KMP_HAVE_UMWAIT

/* ------------------------------------------------------------------------ */
/* Support datatypes for the orphaned construct nesting checks.             */
/* ------------------------------------------------------------------------ */

/* When adding to this enum, add its corresponding string in cons_text_c[]
 * array in kmp_error.cpp */
enum cons_type {
  ct_none,
  ct_parallel,
  ct_pdo,
  ct_pdo_ordered,
  ct_psections,
  ct_psingle,
  ct_critical,
  ct_ordered_in_parallel,
  ct_ordered_in_pdo,
  ct_master,
  ct_reduce,
  ct_barrier,
  ct_masked
};

#define IS_CONS_TYPE_ORDERED(ct) ((ct) == ct_pdo_ordered)

struct cons_data {
  ident_t const *ident;
  enum cons_type type;
  int prev;
  kmp_user_lock_p
      name; /* address exclusively for critical section name comparison */
};

struct cons_header {
  int p_top, w_top, s_top;
  int stack_size, stack_top;
  struct cons_data *stack_data;
};

struct kmp_region_info {
  char *text;
  int offset[KMP_MAX_FIELDS];
  int length[KMP_MAX_FIELDS];
};

/* ---------------------------------------------------------------------- */
/* ---------------------------------------------------------------------- */

#if KMP_OS_WINDOWS
typedef HANDLE kmp_thread_t;
typedef DWORD kmp_key_t;
#endif /* KMP_OS_WINDOWS */

#if KMP_OS_UNIX
typedef pthread_t kmp_thread_t;
typedef pthread_key_t kmp_key_t;
#endif

extern kmp_key_t __kmp_gtid_threadprivate_key;

typedef struct kmp_sys_info {
  long maxrss; /* the maximum resident set size utilized (in kilobytes)     */
  long minflt; /* the number of page faults serviced without any I/O        */
  long majflt; /* the number of page faults serviced that required I/O      */
  long nswap; /* the number of times a process was "swapped" out of memory */
  long inblock; /* the number of times the file system had to perform input  */
  long oublock; /* the number of times the file system had to perform output */
  long nvcsw; /* the number of times a context switch was voluntarily      */
  long nivcsw; /* the number of times a context switch was forced           */
} kmp_sys_info_t;

#if USE_ITT_BUILD
// We cannot include "kmp_itt.h" due to circular dependency. Declare the only
// required type here. Later we will check the type meets requirements.
typedef int kmp_itt_mark_t;
#define KMP_ITT_DEBUG 0
#endif /* USE_ITT_BUILD */

typedef kmp_int32 kmp_critical_name[8];

/*!
@ingroup PARALLEL
The type for a microtask which gets passed to @ref __kmpc_fork_call().
The arguments to the outlined function are
@param global_tid the global thread identity of the thread executing the
function.
@param bound_tid  the local identity of the thread executing the function
@param ... pointers to shared variables accessed by the function.
*/
typedef void (*kmpc_micro)(kmp_int32 *global_tid, kmp_int32 *bound_tid, ...);
typedef void (*kmpc_micro_bound)(kmp_int32 *bound_tid, kmp_int32 *bound_nth,
                                 ...);

/*!
@ingroup THREADPRIVATE
@{
*/
/* ---------------------------------------------------------------------------
 */
/* Threadprivate initialization/finalization function declarations */

/*  for non-array objects:  __kmpc_threadprivate_register()  */

/*!
 Pointer to the constructor function.
 The first argument is the <tt>this</tt> pointer
*/
typedef void *(*kmpc_ctor)(void *);

/*!
 Pointer to the destructor function.
 The first argument is the <tt>this</tt> pointer
*/
typedef void (*kmpc_dtor)(
    void * /*, size_t */); /* 2nd arg: magic number for KCC unused by Intel
                              compiler */
/*!
 Pointer to an alternate constructor.
 The first argument is the <tt>this</tt> pointer.
*/
typedef void *(*kmpc_cctor)(void *, void *);

/* for array objects: __kmpc_threadprivate_register_vec() */
/* First arg: "this" pointer */
/* Last arg: number of array elements */
/*!
 Array constructor.
 First argument is the <tt>this</tt> pointer
 Second argument the number of array elements.
*/
typedef void *(*kmpc_ctor_vec)(void *, size_t);
/*!
 Pointer to the array destructor function.
 The first argument is the <tt>this</tt> pointer
 Second argument the number of array elements.
*/
typedef void (*kmpc_dtor_vec)(void *, size_t);
/*!
 Array constructor.
 First argument is the <tt>this</tt> pointer
 Third argument the number of array elements.
*/
typedef void *(*kmpc_cctor_vec)(void *, void *,
                                size_t); /* function unused by compiler */

/*!
@}
*/

/* keeps tracked of threadprivate cache allocations for cleanup later */
typedef struct kmp_cached_addr {
  void **addr; /* address of allocated cache */
  void ***compiler_cache; /* pointer to compiler's cache */
  void *data; /* pointer to global data */
  struct kmp_cached_addr *next; /* pointer to next cached address */
} kmp_cached_addr_t;

struct private_data {
  struct private_data *next; /* The next descriptor in the list      */
  void *data; /* The data buffer for this descriptor  */
  int more; /* The repeat count for this descriptor */
  size_t size; /* The data size for this descriptor    */
};

struct private_common {
  struct private_common *next;
  struct private_common *link;
  void *gbl_addr;
  void *par_addr; /* par_addr == gbl_addr for PRIMARY thread */
  size_t cmn_size;
};

struct shared_common {
  struct shared_common *next;
  struct private_data *pod_init;
  void *obj_init;
  void *gbl_addr;
  union {
    kmpc_ctor ctor;
    kmpc_ctor_vec ctorv;
  } ct;
  union {
    kmpc_cctor cctor;
    kmpc_cctor_vec cctorv;
  } cct;
  union {
    kmpc_dtor dtor;
    kmpc_dtor_vec dtorv;
  } dt;
  size_t vec_len;
  int is_vec;
  size_t cmn_size;
};

#define KMP_HASH_TABLE_LOG2 9 /* log2 of the hash table size */
#define KMP_HASH_TABLE_SIZE                                                    \
  (1 << KMP_HASH_TABLE_LOG2) /* size of the hash table */
#define KMP_HASH_SHIFT 3 /* throw away this many low bits from the address */
#define KMP_HASH(x)                                                            \
  ((((kmp_uintptr_t)x) >> KMP_HASH_SHIFT) & (KMP_HASH_TABLE_SIZE - 1))

struct common_table {
  struct private_common *data[KMP_HASH_TABLE_SIZE];
};

struct shared_table {
  struct shared_common *data[KMP_HASH_TABLE_SIZE];
};

/* ------------------------------------------------------------------------ */

#if KMP_USE_HIER_SCHED
// Shared barrier data that exists inside a single unit of the scheduling
// hierarchy
typedef struct kmp_hier_private_bdata_t {
  kmp_int32 num_active;
  kmp_uint64 index;
  kmp_uint64 wait_val[2];
} kmp_hier_private_bdata_t;
#endif

typedef struct kmp_sched_flags {
  unsigned ordered : 1;
  unsigned nomerge : 1;
  unsigned contains_last : 1;
  unsigned use_hier : 1; // Used in KMP_USE_HIER_SCHED code
  unsigned use_hybrid : 1; // Used in KMP_WEIGHTED_ITERATIONS_SUPPORTED code
  unsigned unused : 27;
} kmp_sched_flags_t;

KMP_BUILD_ASSERT(sizeof(kmp_sched_flags_t) == 4);

#if KMP_STATIC_STEAL_ENABLED
typedef struct KMP_ALIGN_CACHE dispatch_private_info32 {
  kmp_int32 count;
  kmp_int32 ub;
  /* Adding KMP_ALIGN_CACHE here doesn't help / can hurt performance */
  kmp_int32 lb;
  kmp_int32 st;
  kmp_int32 tc;
  kmp_lock_t *steal_lock; // lock used for chunk stealing

  kmp_uint32 ordered_lower;
  kmp_uint32 ordered_upper;

  // KMP_ALIGN(32) ensures (if the KMP_ALIGN macro is turned on)
  //    a) parm3 is properly aligned and
  //    b) all parm1-4 are on the same cache line.
  // Because of parm1-4 are used together, performance seems to be better
  // if they are on the same cache line (not measured though).

  struct KMP_ALIGN(32) {
    kmp_int32 parm1;
    kmp_int32 parm2;
    kmp_int32 parm3;
    kmp_int32 parm4;
  };

#if KMP_WEIGHTED_ITERATIONS_SUPPORTED
  kmp_uint32 pchunks;
  kmp_uint32 num_procs_with_pcore;
  kmp_int32 first_thread_with_ecore;
#endif
#if KMP_OS_WINDOWS
  kmp_int32 last_upper;
#endif /* KMP_OS_WINDOWS */
} dispatch_private_info32_t;

#if CACHE_LINE <= 128
KMP_BUILD_ASSERT(sizeof(dispatch_private_info32_t) <= 128);
#endif

typedef struct KMP_ALIGN_CACHE dispatch_private_info64 {
  kmp_int64 count; // current chunk number for static & static-steal scheduling
  kmp_int64 ub; /* upper-bound */
  /* Adding KMP_ALIGN_CACHE here doesn't help / can hurt performance */
  kmp_int64 lb; /* lower-bound */
  kmp_int64 st; /* stride */
  kmp_int64 tc; /* trip count (number of iterations) */
  kmp_lock_t *steal_lock; // lock used for chunk stealing

  kmp_uint64 ordered_lower;
  kmp_uint64 ordered_upper;
  /* parm[1-4] are used in different ways by different scheduling algorithms */

  // KMP_ALIGN(32) ensures ( if the KMP_ALIGN macro is turned on )
  //    a) parm3 is properly aligned and
  //    b) all parm1-4 are in the same cache line.
  // Because of parm1-4 are used together, performance seems to be better
  // if they are in the same line (not measured though).
  struct KMP_ALIGN(32) {
    kmp_int64 parm1;
    kmp_int64 parm2;
    kmp_int64 parm3;
    kmp_int64 parm4;
  };

#if KMP_WEIGHTED_ITERATIONS_SUPPORTED
  kmp_uint64 pchunks;
  kmp_uint64 num_procs_with_pcore;
  kmp_int64 first_thread_with_ecore;
#endif

#if KMP_OS_WINDOWS
  kmp_int64 last_upper;
#endif /* KMP_OS_WINDOWS */
} dispatch_private_info64_t;

#if CACHE_LINE <= 128
KMP_BUILD_ASSERT(sizeof(dispatch_private_info64_t) <= 128);
#endif

#else /* KMP_STATIC_STEAL_ENABLED */
typedef struct KMP_ALIGN_CACHE dispatch_private_info32 {
  kmp_int32 lb;
  kmp_int32 ub;
  kmp_int32 st;
  kmp_int32 tc;

  kmp_int32 parm1;
  kmp_int32 parm2;
  kmp_int32 parm3;
  kmp_int32 parm4;

  kmp_int32 count;

  kmp_uint32 ordered_lower;
  kmp_uint32 ordered_upper;
#if KMP_OS_WINDOWS
  kmp_int32 last_upper;
#endif /* KMP_OS_WINDOWS */
} dispatch_private_info32_t;

typedef struct KMP_ALIGN_CACHE dispatch_private_info64 {
  kmp_int64 lb; /* lower-bound */
  kmp_int64 ub; /* upper-bound */
  kmp_int64 st; /* stride */
  kmp_int64 tc; /* trip count (number of iterations) */

  /* parm[1-4] are used in different ways by different scheduling algorithms */
  kmp_int64 parm1;
  kmp_int64 parm2;
  kmp_int64 parm3;
  kmp_int64 parm4;

  kmp_int64 count; /* current chunk number for static scheduling */

  kmp_uint64 ordered_lower;
  kmp_uint64 ordered_upper;
#if KMP_OS_WINDOWS
  kmp_int64 last_upper;
#endif /* KMP_OS_WINDOWS */
} dispatch_private_info64_t;
#endif /* KMP_STATIC_STEAL_ENABLED */

typedef struct KMP_ALIGN_CACHE dispatch_private_info {
  union private_info {
    dispatch_private_info32_t p32;
    dispatch_private_info64_t p64;
  } u;
  enum sched_type schedule; /* scheduling algorithm */
  kmp_sched_flags_t flags; /* flags (e.g., ordered, nomerge, etc.) */
  std::atomic<kmp_uint32> steal_flag; // static_steal only, state of a buffer
  kmp_int32 ordered_bumped;
  // Stack of buffers for nest of serial regions
  struct dispatch_private_info *next;
  kmp_int32 type_size; /* the size of types in private_info */
#if KMP_USE_HIER_SCHED
  kmp_int32 hier_id;
  void *parent; /* hierarchical scheduling parent pointer */
#endif
  enum cons_type pushed_ws;
} dispatch_private_info_t;

typedef struct dispatch_shared_info32 {
  /* chunk index under dynamic, number of idle threads under static-steal;
     iteration index otherwise */
  volatile kmp_uint32 iteration;
  volatile kmp_int32 num_done;
  volatile kmp_uint32 ordered_iteration;
  // Dummy to retain the structure size after making ordered_iteration scalar
  kmp_int32 ordered_dummy[KMP_MAX_ORDERED - 1];
} dispatch_shared_info32_t;

typedef struct dispatch_shared_info64 {
  /* chunk index under dynamic, number of idle threads under static-steal;
     iteration index otherwise */
  volatile kmp_uint64 iteration;
  volatile kmp_int64 num_done;
  volatile kmp_uint64 ordered_iteration;
  // Dummy to retain the structure size after making ordered_iteration scalar
  kmp_int64 ordered_dummy[KMP_MAX_ORDERED - 3];
} dispatch_shared_info64_t;

typedef struct dispatch_shared_info {
  union shared_info {
    dispatch_shared_info32_t s32;
    dispatch_shared_info64_t s64;
  } u;
  volatile kmp_uint32 buffer_index;
  volatile kmp_int32 doacross_buf_idx; // teamwise index
  volatile kmp_uint32 *doacross_flags; // shared array of iteration flags (0/1)
  kmp_int32 doacross_num_done; // count finished threads
#if KMP_USE_HIER_SCHED
  void *hier;
#endif
#if KMP_USE_HWLOC
  // When linking with libhwloc, the ORDERED EPCC test slows down on big
  // machines (> 48 cores). Performance analysis showed that a cache thrash
  // was occurring and this padding helps alleviate the problem.
  char padding[64];
#endif
} dispatch_shared_info_t;

typedef struct kmp_disp {
  /* Vector for ORDERED SECTION */
  void (*th_deo_fcn)(int *gtid, int *cid, ident_t *);
  /* Vector for END ORDERED SECTION */
  void (*th_dxo_fcn)(int *gtid, int *cid, ident_t *);

  dispatch_shared_info_t *th_dispatch_sh_current;
  dispatch_private_info_t *th_dispatch_pr_current;

  dispatch_private_info_t *th_disp_buffer;
  kmp_uint32 th_disp_index;
  kmp_int32 th_doacross_buf_idx; // thread's doacross buffer index
  volatile kmp_uint32 *th_doacross_flags; // pointer to shared array of flags
  kmp_int64 *th_doacross_info; // info on loop bounds
#if KMP_USE_INTERNODE_ALIGNMENT
  char more_padding[INTERNODE_CACHE_LINE];
#endif
} kmp_disp_t;

/* ------------------------------------------------------------------------ */
/* Barrier stuff */

/* constants for barrier state update */
#define KMP_INIT_BARRIER_STATE 0 /* should probably start from zero */
#define KMP_BARRIER_SLEEP_BIT 0 /* bit used for suspend/sleep part of state */
#define KMP_BARRIER_UNUSED_BIT 1 // bit that must never be set for valid state
#define KMP_BARRIER_BUMP_BIT 2 /* lsb used for bump of go/arrived state */

#define KMP_BARRIER_SLEEP_STATE (1 << KMP_BARRIER_SLEEP_BIT)
#define KMP_BARRIER_UNUSED_STATE (1 << KMP_BARRIER_UNUSED_BIT)
#define KMP_BARRIER_STATE_BUMP (1 << KMP_BARRIER_BUMP_BIT)

#if (KMP_BARRIER_SLEEP_BIT >= KMP_BARRIER_BUMP_BIT)
#error "Barrier sleep bit must be smaller than barrier bump bit"
#endif
#if (KMP_BARRIER_UNUSED_BIT >= KMP_BARRIER_BUMP_BIT)
#error "Barrier unused bit must be smaller than barrier bump bit"
#endif

// Constants for release barrier wait state: currently, hierarchical only
#define KMP_BARRIER_NOT_WAITING 0 // Normal state; worker not in wait_sleep
#define KMP_BARRIER_OWN_FLAG                                                   \
  1 // Normal state; worker waiting on own b_go flag in release
#define KMP_BARRIER_PARENT_FLAG                                                \
  2 // Special state; worker waiting on parent's b_go flag in release
#define KMP_BARRIER_SWITCH_TO_OWN_FLAG                                         \
  3 // Special state; tells worker to shift from parent to own b_go
#define KMP_BARRIER_SWITCHING                                                  \
  4 // Special state; worker resets appropriate flag on wake-up

#define KMP_NOT_SAFE_TO_REAP                                                   \
  0 // Thread th_reap_state: not safe to reap (tasking)
#define KMP_SAFE_TO_REAP 1 // Thread th_reap_state: safe to reap (not tasking)

// The flag_type describes the storage used for the flag.
enum flag_type {
  flag32, /**< atomic 32 bit flags */
  flag64, /**< 64 bit flags */
  atomic_flag64, /**< atomic 64 bit flags */
  flag_oncore, /**< special 64-bit flag for on-core barrier (hierarchical) */
  flag_unset
};

enum barrier_type {
  bs_plain_barrier = 0, /* 0, All non-fork/join barriers (except reduction
                           barriers if enabled) */
  bs_forkjoin_barrier, /* 1, All fork/join (parallel region) barriers */
#if KMP_FAST_REDUCTION_BARRIER
  bs_reduction_barrier, /* 2, All barriers that are used in reduction */
#endif // KMP_FAST_REDUCTION_BARRIER
  bs_last_barrier /* Just a placeholder to mark the end */
};

// to work with reduction barriers just like with plain barriers
#if !KMP_FAST_REDUCTION_BARRIER
#define bs_reduction_barrier bs_plain_barrier
#endif // KMP_FAST_REDUCTION_BARRIER

typedef enum kmp_bar_pat { /* Barrier communication patterns */
                           bp_linear_bar =
                               0, /* Single level (degenerate) tree */
                           bp_tree_bar =
                               1, /* Balanced tree with branching factor 2^n */
                           bp_hyper_bar = 2, /* Hypercube-embedded tree with min
                                                branching factor 2^n */
                           bp_hierarchical_bar = 3, /* Machine hierarchy tree */
                           bp_dist_bar = 4, /* Distributed barrier */
                           bp_last_bar /* Placeholder to mark the end */
} kmp_bar_pat_e;

#define KMP_BARRIER_ICV_PUSH 1

/* Record for holding the values of the internal controls stack records */
typedef struct kmp_internal_control {
  int serial_nesting_level; /* corresponds to the value of the
                               th_team_serialized field */
  kmp_int8 dynamic; /* internal control for dynamic adjustment of threads (per
                       thread) */
  kmp_int8
      bt_set; /* internal control for whether blocktime is explicitly set */
  int blocktime; /* internal control for blocktime */
#if KMP_USE_MONITOR
  int bt_intervals; /* internal control for blocktime intervals */
#endif
  int nproc; /* internal control for #threads for next parallel region (per
                thread) */
  int thread_limit; /* internal control for thread-limit-var */
  int task_thread_limit; /* internal control for thread-limit-var of a task*/
  int max_active_levels; /* internal control for max_active_levels */
  kmp_r_sched_t
      sched; /* internal control for runtime schedule {sched,chunk} pair */
  kmp_proc_bind_t proc_bind; /* internal control for affinity  */
  kmp_int32 default_device; /* internal control for default device */
  struct kmp_internal_control *next;
} kmp_internal_control_t;

static inline void copy_icvs(kmp_internal_control_t *dst,
                             kmp_internal_control_t *src) {
  *dst = *src;
}

/* Thread barrier needs volatile barrier fields */
typedef struct KMP_ALIGN_CACHE kmp_bstate {
  // th_fixed_icvs is aligned by virtue of kmp_bstate being aligned (and all
  // uses of it). It is not explicitly aligned below, because we *don't* want
  // it to be padded -- instead, we fit b_go into the same cache line with
  // th_fixed_icvs, enabling NGO cache lines stores in the hierarchical barrier.
  kmp_internal_control_t th_fixed_icvs; // Initial ICVs for the thread
  // Tuck b_go into end of th_fixed_icvs cache line, so it can be stored with
  // same NGO store
  volatile kmp_uint64 b_go; // STATE => task should proceed (hierarchical)
  KMP_ALIGN_CACHE volatile kmp_uint64
      b_arrived; // STATE => task reached synch point.
  kmp_uint32 *skip_per_level;
  kmp_uint32 my_level;
  kmp_int32 parent_tid;
  kmp_int32 old_tid;
  kmp_uint32 depth;
  struct kmp_bstate *parent_bar;
  kmp_team_t *team;
  kmp_uint64 leaf_state;
  kmp_uint32 nproc;
  kmp_uint8 base_leaf_kids;
  kmp_uint8 leaf_kids;
  kmp_uint8 offset;
  kmp_uint8 wait_flag;
  kmp_uint8 use_oncore_barrier;
#if USE_DEBUGGER
  // The following field is intended for the debugger solely. Only the worker
  // thread itself accesses this field: the worker increases it by 1 when it
  // arrives to a barrier.
  KMP_ALIGN_CACHE kmp_uint b_worker_arrived;
#endif /* USE_DEBUGGER */
} kmp_bstate_t;

union KMP_ALIGN_CACHE kmp_barrier_union {
  double b_align; /* use worst case alignment */
  char b_pad[KMP_PAD(kmp_bstate_t, CACHE_LINE)];
  kmp_bstate_t bb;
};

typedef union kmp_barrier_union kmp_balign_t;

/* Team barrier needs only non-volatile arrived counter */
union KMP_ALIGN_CACHE kmp_barrier_team_union {
  double b_align; /* use worst case alignment */
  char b_pad[CACHE_LINE];
  struct {
    kmp_uint64 b_arrived; /* STATE => task reached synch point. */
#if USE_DEBUGGER
    // The following two fields are indended for the debugger solely. Only
    // primary thread of the team accesses these fields: the first one is
    // increased by 1 when the primary thread arrives to a barrier, the second
    // one is increased by one when all the threads arrived.
    kmp_uint b_master_arrived;
    kmp_uint b_team_arrived;
#endif
  };
};

typedef union kmp_barrier_team_union kmp_balign_team_t;

/* Padding for Linux* OS pthreads condition variables and mutexes used to signal
   threads when a condition changes.  This is to workaround an NPTL bug where
   padding was added to pthread_cond_t which caused the initialization routine
   to write outside of the structure if compiled on pre-NPTL threads.  */
#if KMP_OS_WINDOWS
typedef struct kmp_win32_mutex {
  /* The Lock */
  CRITICAL_SECTION cs;
} kmp_win32_mutex_t;

typedef struct kmp_win32_cond {
  /* Count of the number of waiters. */
  int waiters_count_;

  /* Serialize access to <waiters_count_> */
  kmp_win32_mutex_t waiters_count_lock_;

  /* Number of threads to release via a <cond_broadcast> or a <cond_signal> */
  int release_count_;

  /* Keeps track of the current "generation" so that we don't allow */
  /* one thread to steal all the "releases" from the broadcast. */
  int wait_generation_count_;

  /* A manual-reset event that's used to block and release waiting threads. */
  HANDLE event_;
} kmp_win32_cond_t;
#endif

#if KMP_OS_UNIX

union KMP_ALIGN_CACHE kmp_cond_union {
  double c_align;
  char c_pad[CACHE_LINE];
  pthread_cond_t c_cond;
};

typedef union kmp_cond_union kmp_cond_align_t;

union KMP_ALIGN_CACHE kmp_mutex_union {
  double m_align;
  char m_pad[CACHE_LINE];
  pthread_mutex_t m_mutex;
};

typedef union kmp_mutex_union kmp_mutex_align_t;

#endif /* KMP_OS_UNIX */

typedef struct kmp_desc_base {
  void *ds_stackbase;
  size_t ds_stacksize;
  int ds_stackgrow;
  kmp_thread_t ds_thread;
  volatile int ds_tid;
  int ds_gtid;
#if KMP_OS_WINDOWS
  volatile int ds_alive;
  DWORD ds_thread_id;
/* ds_thread keeps thread handle on Windows* OS. It is enough for RTL purposes.
   However, debugger support (libomp_db) cannot work with handles, because they
   uncomparable. For example, debugger requests info about thread with handle h.
   h is valid within debugger process, and meaningless within debugee process.
   Even if h is duped by call to DuplicateHandle(), so the result h' is valid
   within debugee process, but it is a *new* handle which does *not* equal to
   any other handle in debugee... The only way to compare handles is convert
   them to system-wide ids. GetThreadId() function is available only in
   Longhorn and Server 2003. :-( In contrast, GetCurrentThreadId() is available
   on all Windows* OS flavours (including Windows* 95). Thus, we have to get
   thread id by call to GetCurrentThreadId() from within the thread and save it
   to let libomp_db identify threads.  */
#endif /* KMP_OS_WINDOWS */
} kmp_desc_base_t;

typedef union KMP_ALIGN_CACHE kmp_desc {
  double ds_align; /* use worst case alignment */
  char ds_pad[KMP_PAD(kmp_desc_base_t, CACHE_LINE)];
  kmp_desc_base_t ds;
} kmp_desc_t;

typedef struct kmp_local {
  volatile int this_construct; /* count of single's encountered by thread */
  void *reduce_data;
#if KMP_USE_BGET
  void *bget_data;
  void *bget_list;
#if !USE_CMP_XCHG_FOR_BGET
#ifdef USE_QUEUING_LOCK_FOR_BGET
  kmp_lock_t bget_lock; /* Lock for accessing bget free list */
#else
  kmp_bootstrap_lock_t bget_lock; // Lock for accessing bget free list. Must be
// bootstrap lock so we can use it at library
// shutdown.
#endif /* USE_LOCK_FOR_BGET */
#endif /* ! USE_CMP_XCHG_FOR_BGET */
#endif /* KMP_USE_BGET */

  PACKED_REDUCTION_METHOD_T
  packed_reduction_method; /* stored by __kmpc_reduce*(), used by
                              __kmpc_end_reduce*() */

} kmp_local_t;

#define KMP_CHECK_UPDATE(a, b)                                                 \
  if ((a) != (b))                                                              \
  (a) = (b)
#define KMP_CHECK_UPDATE_SYNC(a, b)                                            \
  if ((a) != (b))                                                              \
  TCW_SYNC_PTR((a), (b))

#define get__blocktime(xteam, xtid)                                            \
  ((xteam)->t.t_threads[(xtid)]->th.th_current_task->td_icvs.blocktime)
#define get__bt_set(xteam, xtid)                                               \
  ((xteam)->t.t_threads[(xtid)]->th.th_current_task->td_icvs.bt_set)
#if KMP_USE_MONITOR
#define get__bt_intervals(xteam, xtid)                                         \
  ((xteam)->t.t_threads[(xtid)]->th.th_current_task->td_icvs.bt_intervals)
#endif

#define get__dynamic_2(xteam, xtid)                                            \
  ((xteam)->t.t_threads[(xtid)]->th.th_current_task->td_icvs.dynamic)
#define get__nproc_2(xteam, xtid)                                              \
  ((xteam)->t.t_threads[(xtid)]->th.th_current_task->td_icvs.nproc)
#define get__sched_2(xteam, xtid)                                              \
  ((xteam)->t.t_threads[(xtid)]->th.th_current_task->td_icvs.sched)

#define set__blocktime_team(xteam, xtid, xval)                                 \
  (((xteam)->t.t_threads[(xtid)]->th.th_current_task->td_icvs.blocktime) =     \
       (xval))

#if KMP_USE_MONITOR
#define set__bt_intervals_team(xteam, xtid, xval)                              \
  (((xteam)->t.t_threads[(xtid)]->th.th_current_task->td_icvs.bt_intervals) =  \
       (xval))
#endif

#define set__bt_set_team(xteam, xtid, xval)                                    \
  (((xteam)->t.t_threads[(xtid)]->th.th_current_task->td_icvs.bt_set) = (xval))

#define set__dynamic(xthread, xval)                                            \
  (((xthread)->th.th_current_task->td_icvs.dynamic) = (xval))
#define get__dynamic(xthread)                                                  \
  (((xthread)->th.th_current_task->td_icvs.dynamic) ? (FTN_TRUE) : (FTN_FALSE))

#define set__nproc(xthread, xval)                                              \
  (((xthread)->th.th_current_task->td_icvs.nproc) = (xval))

#define set__thread_limit(xthread, xval)                                       \
  (((xthread)->th.th_current_task->td_icvs.thread_limit) = (xval))

#define set__max_active_levels(xthread, xval)                                  \
  (((xthread)->th.th_current_task->td_icvs.max_active_levels) = (xval))

#define get__max_active_levels(xthread)                                        \
  ((xthread)->th.th_current_task->td_icvs.max_active_levels)

#define set__sched(xthread, xval)                                              \
  (((xthread)->th.th_current_task->td_icvs.sched) = (xval))

#define set__proc_bind(xthread, xval)                                          \
  (((xthread)->th.th_current_task->td_icvs.proc_bind) = (xval))
#define get__proc_bind(xthread)                                                \
  ((xthread)->th.th_current_task->td_icvs.proc_bind)

// OpenMP tasking data structures

typedef enum kmp_tasking_mode {
  tskm_immediate_exec = 0,
  tskm_extra_barrier = 1,
  tskm_task_teams = 2,
  tskm_max = 2
} kmp_tasking_mode_t;

extern kmp_tasking_mode_t
    __kmp_tasking_mode; /* determines how/when to execute tasks */
extern int __kmp_task_stealing_constraint;
extern int __kmp_enable_task_throttling;
extern kmp_int32 __kmp_default_device; // Set via OMP_DEFAULT_DEVICE if
// specified, defaults to 0 otherwise
// Set via OMP_MAX_TASK_PRIORITY if specified, defaults to 0 otherwise
extern kmp_int32 __kmp_max_task_priority;
// Set via KMP_TASKLOOP_MIN_TASKS if specified, defaults to 0 otherwise
extern kmp_uint64 __kmp_taskloop_min_tasks;

/* NOTE: kmp_taskdata_t and kmp_task_t structures allocated in single block with
   taskdata first */
#define KMP_TASK_TO_TASKDATA(task) (((kmp_taskdata_t *)task) - 1)
#define KMP_TASKDATA_TO_TASK(taskdata) (kmp_task_t *)(taskdata + 1)

// The tt_found_tasks flag is a signal to all threads in the team that tasks
// were spawned and queued since the previous barrier release.
#define KMP_TASKING_ENABLED(task_team)                                         \
  (TRUE == TCR_SYNC_4((task_team)->tt.tt_found_tasks))
/*!
@ingroup BASIC_TYPES
@{
*/

/*!
 */
typedef kmp_int32 (*kmp_routine_entry_t)(kmp_int32, void *);

typedef union kmp_cmplrdata {
  kmp_int32 priority; /**< priority specified by user for the task */
  kmp_routine_entry_t
      destructors; /* pointer to function to invoke deconstructors of
                      firstprivate C++ objects */
  /* future data */
} kmp_cmplrdata_t;

/*  sizeof_kmp_task_t passed as arg to kmpc_omp_task call  */
/*!
 */
typedef struct kmp_task { /* GEH: Shouldn't this be aligned somehow? */
  void *shareds; /**< pointer to block of pointers to shared vars   */
  kmp_routine_entry_t
      routine; /**< pointer to routine to call for executing task */
  kmp_int32 part_id; /**< part id for the task                          */
  kmp_cmplrdata_t
      data1; /* Two known optional additions: destructors and priority */
  kmp_cmplrdata_t data2; /* Process destructors first, priority second */
  /* future data */
  /*  private vars  */
} kmp_task_t;

/*!
@}
*/

typedef struct kmp_taskgroup {
  std::atomic<kmp_int32> count; // number of allocated and incomplete tasks
  std::atomic<kmp_int32>
      cancel_request; // request for cancellation of this taskgroup
  struct kmp_taskgroup *parent; // parent taskgroup
  // Block of data to perform task reduction
  void *reduce_data; // reduction related info
  kmp_int32 reduce_num_data; // number of data items to reduce
  uintptr_t *gomp_data; // gomp reduction data
} kmp_taskgroup_t;

// forward declarations
typedef union kmp_depnode kmp_depnode_t;
typedef struct kmp_depnode_list kmp_depnode_list_t;
typedef struct kmp_dephash_entry kmp_dephash_entry_t;

// macros for checking dep flag as an integer
#define KMP_DEP_IN 0x1
#define KMP_DEP_OUT 0x2
#define KMP_DEP_INOUT 0x3
#define KMP_DEP_MTX 0x4
#define KMP_DEP_SET 0x8
#define KMP_DEP_ALL 0x80
// Compiler sends us this info. Note: some test cases contain an explicit copy
// of this struct and should be in sync with any changes here.
typedef struct kmp_depend_info {
  kmp_intptr_t base_addr;
  size_t len;
  union {
    kmp_uint8 flag; // flag as an unsigned char
    struct { // flag as a set of 8 bits
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
      /* Same fields as in the #else branch, but in reverse order */
      unsigned all : 1;
      unsigned unused : 3;
      unsigned set : 1;
      unsigned mtx : 1;
      unsigned out : 1;
      unsigned in : 1;
#else
      unsigned in : 1;
      unsigned out : 1;
      unsigned mtx : 1;
      unsigned set : 1;
      unsigned unused : 3;
      unsigned all : 1;
#endif
    } flags;
  };
} kmp_depend_info_t;

// Internal structures to work with task dependencies:
struct kmp_depnode_list {
  kmp_depnode_t *node;
  kmp_depnode_list_t *next;
};

// Max number of mutexinoutset dependencies per node
#define MAX_MTX_DEPS 4

typedef struct kmp_base_depnode {
  kmp_depnode_list_t *successors; /* used under lock */
  kmp_task_t *task; /* non-NULL if depnode is active, used under lock */
  kmp_lock_t *mtx_locks[MAX_MTX_DEPS]; /* lock mutexinoutset dependent tasks */
  kmp_int32 mtx_num_locks; /* number of locks in mtx_locks array */
  kmp_lock_t lock; /* guards shared fields: task, successors */
#if KMP_SUPPORT_GRAPH_OUTPUT
  kmp_uint32 id;
#endif
  std::atomic<kmp_int32> npredecessors;
  std::atomic<kmp_int32> nrefs;
} kmp_base_depnode_t;

union KMP_ALIGN_CACHE kmp_depnode {
  double dn_align; /* use worst case alignment */
  char dn_pad[KMP_PAD(kmp_base_depnode_t, CACHE_LINE)];
  kmp_base_depnode_t dn;
};

struct kmp_dephash_entry {
  kmp_intptr_t addr;
  kmp_depnode_t *last_out;
  kmp_depnode_list_t *last_set;
  kmp_depnode_list_t *prev_set;
  kmp_uint8 last_flag;
  kmp_lock_t *mtx_lock; /* is referenced by depnodes w/mutexinoutset dep */
  kmp_dephash_entry_t *next_in_bucket;
};

typedef struct kmp_dephash {
  kmp_dephash_entry_t **buckets;
  size_t size;
  kmp_depnode_t *last_all;
  size_t generation;
  kmp_uint32 nelements;
  kmp_uint32 nconflicts;
} kmp_dephash_t;

typedef struct kmp_task_affinity_info {
  kmp_intptr_t base_addr;
  size_t len;
  struct {
    bool flag1 : 1;
    bool flag2 : 1;
    kmp_int32 reserved : 30;
  } flags;
} kmp_task_affinity_info_t;

typedef enum kmp_event_type_t {
  KMP_EVENT_UNINITIALIZED = 0,
  KMP_EVENT_ALLOW_COMPLETION = 1
} kmp_event_type_t;

typedef struct {
  kmp_event_type_t type;
  kmp_tas_lock_t lock;
  union {
    kmp_task_t *task;
  } ed;
} kmp_event_t;

#if OMPX_TASKGRAPH
// Initial number of allocated nodes while recording
#define INIT_MAPSIZE 50

typedef struct kmp_taskgraph_flags { /*This needs to be exactly 32 bits */
  unsigned nowait : 1;
  unsigned re_record : 1;
  unsigned reserved : 30;
} kmp_taskgraph_flags_t;

/// Represents a TDG node
typedef struct kmp_node_info {
  kmp_task_t *task; // Pointer to the actual task
  kmp_int32 *successors; // Array of the succesors ids
  kmp_int32 nsuccessors; // Number of succesors of the node
  std::atomic<kmp_int32>
      npredecessors_counter; // Number of predessors on the fly
  kmp_int32 npredecessors; // Total number of predecessors
  kmp_int32 successors_size; // Number of allocated succesors ids
  kmp_taskdata_t *parent_task; // Parent implicit task
} kmp_node_info_t;

/// Represent a TDG's current status
typedef enum kmp_tdg_status {
  KMP_TDG_NONE = 0,
  KMP_TDG_RECORDING = 1,
  KMP_TDG_READY = 2
} kmp_tdg_status_t;

/// Structure that contains a TDG
typedef struct kmp_tdg_info {
  kmp_int32 tdg_id; // Unique idenfifier of the TDG
  kmp_taskgraph_flags_t tdg_flags; // Flags related to a TDG
  kmp_int32 map_size; // Number of allocated TDG nodes
  kmp_int32 num_roots; // Number of roots tasks int the TDG
  kmp_int32 *root_tasks; // Array of tasks identifiers that are roots
  kmp_node_info_t *record_map; // Array of TDG nodes
  kmp_tdg_status_t tdg_status =
      KMP_TDG_NONE; // Status of the TDG (recording, ready...)
  std::atomic<kmp_int32> num_tasks; // Number of TDG nodes
  kmp_bootstrap_lock_t
      graph_lock; // Protect graph attributes when updated via taskloop_recur
  // Taskloop reduction related
  void *rec_taskred_data; // Data to pass to __kmpc_task_reduction_init or
                          // __kmpc_taskred_init
  kmp_int32 rec_num_taskred;
} kmp_tdg_info_t;

extern int __kmp_tdg_dot;
extern kmp_int32 __kmp_max_tdgs;
extern kmp_tdg_info_t **__kmp_global_tdgs;
extern kmp_int32 __kmp_curr_tdg_idx;
extern kmp_int32 __kmp_successors_size;
extern std::atomic<kmp_int32> __kmp_tdg_task_id;
extern kmp_int32 __kmp_num_tdg;
#endif

#ifdef BUILD_TIED_TASK_STACK

/* Tied Task stack definitions */
typedef struct kmp_stack_block {
  kmp_taskdata_t *sb_block[TASK_STACK_BLOCK_SIZE];
  struct kmp_stack_block *sb_next;
  struct kmp_stack_block *sb_prev;
} kmp_stack_block_t;

typedef struct kmp_task_stack {
  kmp_stack_block_t ts_first_block; // first block of stack entries
  kmp_taskdata_t **ts_top; // pointer to the top of stack
  kmp_int32 ts_entries; // number of entries on the stack
} kmp_task_stack_t;

#endif // BUILD_TIED_TASK_STACK

typedef struct kmp_tasking_flags { /* Total struct must be exactly 32 bits */
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
  /* Same fields as in the #else branch, but in reverse order */
#if OMPX_TASKGRAPH
  unsigned reserved31 : 5;
  unsigned onced : 1;
#else
  unsigned reserved31 : 6;
#endif
  unsigned target : 1;
  unsigned native : 1;
  unsigned freed : 1;
  unsigned complete : 1;
  unsigned executing : 1;
  unsigned started : 1;
  unsigned team_serial : 1;
  unsigned tasking_ser : 1;
  unsigned task_serial : 1;
  unsigned tasktype : 1;
  unsigned reserved : 8;
  unsigned hidden_helper : 1;
  unsigned detachable : 1;
  unsigned priority_specified : 1;
  unsigned proxy : 1;
  unsigned destructors_thunk : 1;
  unsigned merged_if0 : 1;
  unsigned final : 1;
  unsigned tiedness : 1;
#else
  /* Compiler flags */ /* Total compiler flags must be 16 bits */
  unsigned tiedness : 1; /* task is either tied (1) or untied (0) */
  unsigned final : 1; /* task is final(1) so execute immediately */
  unsigned merged_if0 : 1; /* no __kmpc_task_{begin/complete}_if0 calls in if0
                              code path */
  unsigned destructors_thunk : 1; /* set if the compiler creates a thunk to
                                     invoke destructors from the runtime */
  unsigned proxy : 1; /* task is a proxy task (it will be executed outside the
                         context of the RTL) */
  unsigned priority_specified : 1; /* set if the compiler provides priority
                                      setting for the task */
  unsigned detachable : 1; /* 1 == can detach */
  unsigned hidden_helper : 1; /* 1 == hidden helper task */
  unsigned reserved : 8; /* reserved for compiler use */

  /* Library flags */ /* Total library flags must be 16 bits */
  unsigned tasktype : 1; /* task is either explicit(1) or implicit (0) */
  unsigned task_serial : 1; // task is executed immediately (1) or deferred (0)
  unsigned tasking_ser : 1; // all tasks in team are either executed immediately
  // (1) or may be deferred (0)
  unsigned team_serial : 1; // entire team is serial (1) [1 thread] or parallel
  // (0) [>= 2 threads]
  /* If either team_serial or tasking_ser is set, task team may be NULL */
  /* Task State Flags: */
  unsigned started : 1; /* 1==started, 0==not started     */
  unsigned executing : 1; /* 1==executing, 0==not executing */
  unsigned complete : 1; /* 1==complete, 0==not complete   */
  unsigned freed : 1; /* 1==freed, 0==allocated        */
  unsigned native : 1; /* 1==gcc-compiled task, 0==intel */
  unsigned target : 1;
#if OMPX_TASKGRAPH
  unsigned onced : 1; /* 1==ran once already, 0==never ran, record & replay purposes */
  unsigned reserved31 : 5; /* reserved for library use */
#else
  unsigned reserved31 : 6; /* reserved for library use */
#endif
#endif
} kmp_tasking_flags_t;

typedef struct kmp_target_data {
  void *async_handle; // libomptarget async handle for task completion query
} kmp_target_data_t;

struct kmp_taskdata { /* aligned during dynamic allocation       */
  kmp_int32 td_task_id; /* id, assigned by debugger                */
  kmp_tasking_flags_t td_flags; /* task flags                              */
  kmp_team_t *td_team; /* team for this task                      */
  kmp_info_p *td_alloc_thread; /* thread that allocated data structures   */
  /* Currently not used except for perhaps IDB */
  kmp_taskdata_t *td_parent; /* parent task                             */
  kmp_int32 td_level; /* task nesting level                      */
  std::atomic<kmp_int32> td_untied_count; // untied task active parts counter
  ident_t *td_ident; /* task identifier                         */
  // Taskwait data.
  ident_t *td_taskwait_ident;
  kmp_uint32 td_taskwait_counter;
  kmp_int32 td_taskwait_thread; /* gtid + 1 of thread encountered taskwait */
  KMP_ALIGN_CACHE kmp_internal_control_t
      td_icvs; /* Internal control variables for the task */
  KMP_ALIGN_CACHE std::atomic<kmp_int32>
      td_allocated_child_tasks; /* Child tasks (+ current task) not yet
                                   deallocated */
  std::atomic<kmp_int32>
      td_incomplete_child_tasks; /* Child tasks not yet complete */
  kmp_taskgroup_t
      *td_taskgroup; // Each task keeps pointer to its current taskgroup
  kmp_dephash_t
      *td_dephash; // Dependencies for children tasks are tracked from here
  kmp_depnode_t
      *td_depnode; // Pointer to graph node if this task has dependencies
  kmp_task_team_t *td_task_team;
  size_t td_size_alloc; // Size of task structure, including shareds etc.
#if defined(KMP_GOMP_COMPAT)
  // 4 or 8 byte integers for the loop bounds in GOMP_taskloop
  kmp_int32 td_size_loop_bounds;
#endif
  kmp_taskdata_t *td_last_tied; // keep tied task for task scheduling constraint
#if defined(KMP_GOMP_COMPAT)
  // GOMP sends in a copy function for copy constructors
  void (*td_copy_func)(void *, void *);
#endif
  kmp_event_t td_allow_completion_event;
#if OMPT_SUPPORT
  ompt_task_info_t ompt_task_info;
#endif
#if OMPX_TASKGRAPH
  bool is_taskgraph = 0; // whether the task is within a TDG
  kmp_tdg_info_t *tdg; // used to associate task with a TDG
#endif
  kmp_target_data_t td_target_data;
}; // struct kmp_taskdata

// Make sure padding above worked
KMP_BUILD_ASSERT(sizeof(kmp_taskdata_t) % sizeof(void *) == 0);

// Data for task team but per thread
typedef struct kmp_base_thread_data {
  kmp_info_p *td_thr; // Pointer back to thread info
  // Used only in __kmp_execute_tasks_template, maybe not avail until task is
  // queued?
  kmp_bootstrap_lock_t td_deque_lock; // Lock for accessing deque
  kmp_taskdata_t *
      *td_deque; // Deque of tasks encountered by td_thr, dynamically allocated
  kmp_int32 td_deque_size; // Size of deck
  kmp_uint32 td_deque_head; // Head of deque (will wrap)
  kmp_uint32 td_deque_tail; // Tail of deque (will wrap)
  kmp_int32 td_deque_ntasks; // Number of tasks in deque
  // GEH: shouldn't this be volatile since used in while-spin?
  kmp_int32 td_deque_last_stolen; // Thread number of last successful steal
#ifdef BUILD_TIED_TASK_STACK
  kmp_task_stack_t td_susp_tied_tasks; // Stack of suspended tied tasks for task
// scheduling constraint
#endif // BUILD_TIED_TASK_STACK
} kmp_base_thread_data_t;

#define TASK_DEQUE_BITS 8 // Used solely to define INITIAL_TASK_DEQUE_SIZE
#define INITIAL_TASK_DEQUE_SIZE (1 << TASK_DEQUE_BITS)

#define TASK_DEQUE_SIZE(td) ((td).td_deque_size)
#define TASK_DEQUE_MASK(td) ((td).td_deque_size - 1)

typedef union KMP_ALIGN_CACHE kmp_thread_data {
  kmp_base_thread_data_t td;
  double td_align; /* use worst case alignment */
  char td_pad[KMP_PAD(kmp_base_thread_data_t, CACHE_LINE)];
} kmp_thread_data_t;

typedef struct kmp_task_pri {
  kmp_thread_data_t td;
  kmp_int32 priority;
  kmp_task_pri *next;
} kmp_task_pri_t;

// Data for task teams which are used when tasking is enabled for the team
typedef struct kmp_base_task_team {
  kmp_bootstrap_lock_t
      tt_threads_lock; /* Lock used to allocate per-thread part of task team */
  /* must be bootstrap lock since used at library shutdown*/

  // TODO: check performance vs kmp_tas_lock_t
  kmp_bootstrap_lock_t tt_task_pri_lock; /* Lock to access priority tasks */
  kmp_task_pri_t *tt_task_pri_list;

  kmp_task_team_t *tt_next; /* For linking the task team free list */
  kmp_thread_data_t
      *tt_threads_data; /* Array of per-thread structures for task team */
  /* Data survives task team deallocation */
  kmp_int32 tt_found_tasks; /* Have we found tasks and queued them while
                               executing this team? */
  /* TRUE means tt_threads_data is set up and initialized */
  kmp_int32 tt_nproc; /* #threads in team           */
  kmp_int32 tt_max_threads; // # entries allocated for threads_data array
  kmp_int32 tt_found_proxy_tasks; // found proxy tasks since last barrier
  kmp_int32 tt_untied_task_encountered;
  std::atomic<kmp_int32> tt_num_task_pri; // number of priority tasks enqueued
  // There is hidden helper thread encountered in this task team so that we must
  // wait when waiting on task team
  kmp_int32 tt_hidden_helper_task_encountered;

  KMP_ALIGN_CACHE
  std::atomic<kmp_int32> tt_unfinished_threads; /* #threads still active */

  KMP_ALIGN_CACHE
  volatile kmp_uint32
      tt_active; /* is the team still actively executing tasks */
} kmp_base_task_team_t;

union KMP_ALIGN_CACHE kmp_task_team {
  kmp_base_task_team_t tt;
  double tt_align; /* use worst case alignment */
  char tt_pad[KMP_PAD(kmp_base_task_team_t, CACHE_LINE)];
};

typedef struct kmp_task_team_list_t {
  kmp_task_team_t *task_team;
  kmp_task_team_list_t *next;
} kmp_task_team_list_t;

#if (USE_FAST_MEMORY == 3) || (USE_FAST_MEMORY == 5)
// Free lists keep same-size free memory slots for fast memory allocation
// routines
typedef struct kmp_free_list {
  void *th_free_list_self; // Self-allocated tasks free list
  void *th_free_list_sync; // Self-allocated tasks stolen/returned by other
  // threads
  void *th_free_list_other; // Non-self free list (to be returned to owner's
  // sync list)
} kmp_free_list_t;
#endif
#if KMP_NESTED_HOT_TEAMS
// Hot teams array keeps hot teams and their sizes for given thread. Hot teams
// are not put in teams pool, and they don't put threads in threads pool.
typedef struct kmp_hot_team_ptr {
  kmp_team_p *hot_team; // pointer to hot_team of given nesting level
  kmp_int32 hot_team_nth; // number of threads allocated for the hot_team
} kmp_hot_team_ptr_t;
#endif
typedef struct kmp_teams_size {
  kmp_int32 nteams; // number of teams in a league
  kmp_int32 nth; // number of threads in each team of the league
} kmp_teams_size_t;

// This struct stores a thread that acts as a "root" for a contention
// group. Contention groups are rooted at kmp_root threads, but also at
// each primary thread of each team created in the teams construct.
// This struct therefore also stores a thread_limit associated with
// that contention group, and a counter to track the number of threads
// active in that contention group. Each thread has a list of these: CG
// root threads have an entry in their list in which cg_root refers to
// the thread itself, whereas other workers in the CG will have a
// single entry where cg_root is same as the entry containing their CG
// root. When a thread encounters a teams construct, it will add a new
// entry to the front of its list, because it now roots a new CG.
typedef struct kmp_cg_root {
  kmp_info_p *cg_root; // "root" thread for a contention group
  // The CG root's limit comes from OMP_THREAD_LIMIT for root threads, or
  // thread_limit clause for teams primary threads
  kmp_int32 cg_thread_limit;
  kmp_int32 cg_nthreads; // Count of active threads in CG rooted at cg_root
  struct kmp_cg_root *up; // pointer to higher level CG root in list
} kmp_cg_root_t;

// OpenMP thread data structures

typedef struct KMP_ALIGN_CACHE kmp_base_info {
  /* Start with the readonly data which is cache aligned and padded. This is
     written before the thread starts working by the primary thread. Uber
     masters may update themselves later. Usage does not consider serialized
     regions.  */
  kmp_desc_t th_info;
  kmp_team_p *th_team; /* team we belong to */
  kmp_root_p *th_root; /* pointer to root of task hierarchy */
  kmp_info_p *th_next_pool; /* next available thread in the pool */
  kmp_disp_t *th_dispatch; /* thread's dispatch data */
  int th_in_pool; /* in thread pool (32 bits for TCR/TCW) */

  /* The following are cached from the team info structure */
  /* TODO use these in more places as determined to be needed via profiling */
  int th_team_nproc; /* number of threads in a team */
  kmp_info_p *th_team_master; /* the team's primary thread */
  int th_team_serialized; /* team is serialized */
  microtask_t th_teams_microtask; /* save entry address for teams construct */
  int th_teams_level; /* save initial level of teams construct */
/* it is 0 on device but may be any on host */

/* The blocktime info is copied from the team struct to the thread struct */
/* at the start of a barrier, and the values stored in the team are used  */
/* at points in the code where the team struct is no longer guaranteed    */
/* to exist (from the POV of worker threads).                             */
#if KMP_USE_MONITOR
  int th_team_bt_intervals;
  int th_team_bt_set;
#else
  kmp_uint64 th_team_bt_intervals;
#endif

#if KMP_AFFINITY_SUPPORTED
  kmp_affin_mask_t *th_affin_mask; /* thread's current affinity mask */
  kmp_affinity_ids_t th_topology_ids; /* thread's current topology ids */
  kmp_affinity_attrs_t th_topology_attrs; /* thread's current topology attrs */
#endif
  omp_allocator_handle_t th_def_allocator; /* default allocator */
  /* The data set by the primary thread at reinit, then R/W by the worker */
  KMP_ALIGN_CACHE int
      th_set_nproc; /* if > 0, then only use this request for the next fork */
  int *th_set_nested_nth;
  bool th_nt_strict; // num_threads clause has strict modifier
  ident_t *th_nt_loc; // loc for strict modifier
  int th_nt_sev; // error severity for strict modifier
  const char *th_nt_msg; // error message for strict modifier
  int th_set_nested_nth_sz;
#if KMP_NESTED_HOT_TEAMS
  kmp_hot_team_ptr_t *th_hot_teams; /* array of hot teams */
#endif
  kmp_proc_bind_t
      th_set_proc_bind; /* if != proc_bind_default, use request for next fork */
  kmp_teams_size_t
      th_teams_size; /* number of teams/threads in teams construct */
#if KMP_AFFINITY_SUPPORTED
  int th_current_place; /* place currently bound to */
  int th_new_place; /* place to bind to in par reg */
  int th_first_place; /* first place in partition */
  int th_last_place; /* last place in partition */
#endif
  int th_prev_level; /* previous level for affinity format */
  int th_prev_num_threads; /* previous num_threads for affinity format */
#if USE_ITT_BUILD
  kmp_uint64 th_bar_arrive_time; /* arrival to barrier timestamp */
  kmp_uint64 th_bar_min_time; /* minimum arrival time at the barrier */
  kmp_uint64 th_frame_time; /* frame timestamp */
#endif /* USE_ITT_BUILD */
  kmp_local_t th_local;
  struct private_common *th_pri_head;

  /* Now the data only used by the worker (after initial allocation) */
  /* TODO the first serial team should actually be stored in the info_t
     structure.  this will help reduce initial allocation overhead */
  KMP_ALIGN_CACHE kmp_team_p
      *th_serial_team; /*serialized team held in reserve*/

#if OMPT_SUPPORT
  ompt_thread_info_t ompt_thread_info;
#endif

  /* The following are also read by the primary thread during reinit */
  struct common_table *th_pri_common;

  volatile kmp_uint32 th_spin_here; /* thread-local location for spinning */
  /* while awaiting queuing lock acquire */

  volatile void *th_sleep_loc; // this points at a kmp_flag<T>
  flag_type th_sleep_loc_type; // enum type of flag stored in th_sleep_loc

  ident_t *th_ident;
  unsigned th_x; // Random number generator data
  unsigned th_a; // Random number generator data

  /* Tasking-related data for the thread */
  kmp_task_team_t *th_task_team; // Task team struct
  kmp_taskdata_t *th_current_task; // Innermost Task being executed
  kmp_uint8 th_task_state; // alternating 0/1 for task team identification
  kmp_uint32 th_reap_state; // Non-zero indicates thread is not
  // tasking, thus safe to reap

  /* More stuff for keeping track of active/sleeping threads (this part is
     written by the worker thread) */
  kmp_uint8 th_active_in_pool; // included in count of #active threads in pool
  int th_active; // ! sleeping; 32 bits for TCR/TCW
  std::atomic<kmp_uint32> th_used_in_team; // Flag indicating use in team
  // 0 = not used in team; 1 = used in team;
  // 2 = transitioning to not used in team; 3 = transitioning to used in team
  struct cons_header *th_cons; // used for consistency check
#if KMP_USE_HIER_SCHED
  // used for hierarchical scheduling
  kmp_hier_private_bdata_t *th_hier_bar_data;
#endif

  /* Add the syncronizing data which is cache aligned and padded. */
  KMP_ALIGN_CACHE kmp_balign_t th_bar[bs_last_barrier];

  KMP_ALIGN_CACHE volatile kmp_int32
      th_next_waiting; /* gtid+1 of next thread on lock wait queue, 0 if none */

#if (USE_FAST_MEMORY == 3) || (USE_FAST_MEMORY == 5)
#define NUM_LISTS 4
  kmp_free_list_t th_free_lists[NUM_LISTS]; // Free lists for fast memory
// allocation routines
#endif

#if KMP_OS_WINDOWS
  kmp_win32_cond_t th_suspend_cv;
  kmp_win32_mutex_t th_suspend_mx;
  std::atomic<int> th_suspend_init;
#endif
#if KMP_OS_UNIX
  kmp_cond_align_t th_suspend_cv;
  kmp_mutex_align_t th_suspend_mx;
  std::atomic<int> th_suspend_init_count;
#endif

#if USE_ITT_BUILD
  kmp_itt_mark_t th_itt_mark_single;
// alignment ???
#endif /* USE_ITT_BUILD */
#if KMP_STATS_ENABLED
  kmp_stats_list *th_stats;
#endif
#if KMP_OS_UNIX
  std::atomic<bool> th_blocking;
#endif
  kmp_cg_root_t *th_cg_roots; // list of cg_roots associated with this thread
} kmp_base_info_t;

typedef union KMP_ALIGN_CACHE kmp_info {
  double th_align; /* use worst case alignment */
  char th_pad[KMP_PAD(kmp_base_info_t, CACHE_LINE)];
  kmp_base_info_t th;
} kmp_info_t;

// OpenMP thread team data structures

typedef struct kmp_base_data {
  volatile kmp_uint32 t_value;
} kmp_base_data_t;

typedef union KMP_ALIGN_CACHE kmp_sleep_team {
  double dt_align; /* use worst case alignment */
  char dt_pad[KMP_PAD(kmp_base_data_t, CACHE_LINE)];
  kmp_base_data_t dt;
} kmp_sleep_team_t;

typedef union KMP_ALIGN_CACHE kmp_ordered_team {
  double dt_align; /* use worst case alignment */
  char dt_pad[KMP_PAD(kmp_base_data_t, CACHE_LINE)];
  kmp_base_data_t dt;
} kmp_ordered_team_t;

typedef int (*launch_t)(int gtid);

/* Minimum number of ARGV entries to malloc if necessary */
#define KMP_MIN_MALLOC_ARGV_ENTRIES 100

// Set up how many argv pointers will fit in cache lines containing
// t_inline_argv. Historically, we have supported at least 96 bytes. Using a
// larger value for more space between the primary write/worker read section and
// read/write by all section seems to buy more performance on EPCC PARALLEL.
#if KMP_ARCH_X86 || KMP_ARCH_X86_64
#define KMP_INLINE_ARGV_BYTES                                                  \
  (4 * CACHE_LINE -                                                            \
   ((3 * KMP_PTR_SKIP + 2 * sizeof(int) + 2 * sizeof(kmp_int8) +               \
     sizeof(kmp_int16) + sizeof(kmp_uint32)) %                                 \
    CACHE_LINE))
#else
#define KMP_INLINE_ARGV_BYTES                                                  \
  (2 * CACHE_LINE - ((3 * KMP_PTR_SKIP + 2 * sizeof(int)) % CACHE_LINE))
#endif
#define KMP_INLINE_ARGV_ENTRIES (int)(KMP_INLINE_ARGV_BYTES / KMP_PTR_SKIP)

typedef struct KMP_ALIGN_CACHE kmp_base_team {
  // Synchronization Data
  // ---------------------------------------------------------------------------
  KMP_ALIGN_CACHE kmp_ordered_team_t t_ordered;
  kmp_balign_team_t t_bar[bs_last_barrier];
  std::atomic<int> t_construct; // count of single directive encountered by team
  char pad[sizeof(kmp_lock_t)]; // padding to maintain performance on big iron

  // [0] - parallel / [1] - worksharing task reduction data shared by taskgroups
  std::atomic<void *> t_tg_reduce_data[2]; // to support task modifier
  std::atomic<int> t_tg_fini_counter[2]; // sync end of task reductions

  // Primary thread only
  // ---------------------------------------------------------------------------
  KMP_ALIGN_CACHE int t_master_tid; // tid of primary thread in parent team
  int t_master_this_cons; // "this_construct" single counter of primary thread
  // in parent team
  ident_t *t_ident; // if volatile, have to change too much other crud to
  // volatile too
  kmp_team_p *t_parent; // parent team
  kmp_team_p *t_next_pool; // next free team in the team pool
  kmp_disp_t *t_dispatch; // thread's dispatch data
  kmp_task_team_t *t_task_team[2]; // Task team struct; switch between 2
  kmp_proc_bind_t t_proc_bind; // bind type for par region
  int t_primary_task_state; // primary thread's task state saved
#if USE_ITT_BUILD
  kmp_uint64 t_region_time; // region begin timestamp
#endif /* USE_ITT_BUILD */

  // Primary thread write, workers read
  // --------------------------------------------------------------------------
  KMP_ALIGN_CACHE void **t_argv;
  int t_argc;
  int t_nproc; // number of threads in team
  microtask_t t_pkfn;
  launch_t t_invoke; // procedure to launch the microtask

#if OMPT_SUPPORT
  ompt_team_info_t ompt_team_info;
  ompt_lw_taskteam_t *ompt_serialized_team_info;
#endif

#if KMP_ARCH_X86 || KMP_ARCH_X86_64
  kmp_int8 t_fp_control_saved;
  kmp_int8 t_pad2b;
  kmp_int16 t_x87_fpu_control_word; // FP control regs
  kmp_uint32 t_mxcsr;
#endif /* KMP_ARCH_X86 || KMP_ARCH_X86_64 */

  void *t_inline_argv[KMP_INLINE_ARGV_ENTRIES];

  KMP_ALIGN_CACHE kmp_info_t **t_threads;
  kmp_taskdata_t
      *t_implicit_task_taskdata; // Taskdata for the thread's implicit task
  int t_level; // nested parallel level

  KMP_ALIGN_CACHE int t_max_argc;
  int t_max_nproc; // max threads this team can handle (dynamically expandable)
  int t_serialized; // levels deep of serialized teams
  dispatch_shared_info_t *t_disp_buffer; // buffers for dispatch system
  int t_id; // team's id, assigned by debugger.
  int t_active_level; // nested active parallel level
  kmp_r_sched_t t_sched; // run-time schedule for the team
#if KMP_AFFINITY_SUPPORTED
  int t_first_place; // first & last place in parent thread's partition.
  int t_last_place; // Restore these values to primary thread after par region.
#endif // KMP_AFFINITY_SUPPORTED
  int t_display_affinity;
  int t_size_changed; // team size was changed?: 0: no, 1: yes, -1: changed via
  // omp_set_num_threads() call
  omp_allocator_handle_t t_def_allocator; /* default allocator */

// Read/write by workers as well
#if (KMP_ARCH_X86 || KMP_ARCH_X86_64)
  // Using CACHE_LINE=64 reduces memory footprint, but causes a big perf
  // regression of epcc 'parallel' and 'barrier' on fxe256lin01. This extra
  // padding serves to fix the performance of epcc 'parallel' and 'barrier' when
  // CACHE_LINE=64. TODO: investigate more and get rid if this padding.
  char dummy_padding[1024];
#endif
  // Internal control stack for additional nested teams.
  KMP_ALIGN_CACHE kmp_internal_control_t *t_control_stack_top;
  // for SERIALIZED teams nested 2 or more levels deep
  // typed flag to store request state of cancellation
  std::atomic<kmp_int32> t_cancel_request;
  int t_master_active; // save on fork, restore on join
  void *t_copypriv_data; // team specific pointer to copyprivate data array
#if KMP_OS_WINDOWS
  std::atomic<kmp_uint32> t_copyin_counter;
#endif
#if USE_ITT_BUILD
  void *t_stack_id; // team specific stack stitching id (for ittnotify)
#endif /* USE_ITT_BUILD */
  distributedBarrier *b; // Distributed barrier data associated with team
  kmp_nested_nthreads_t *t_nested_nth;
} kmp_base_team_t;

// Assert that the list structure fits and aligns within
// the double task team pointer
KMP_BUILD_ASSERT(sizeof(kmp_task_team_t *[2]) == sizeof(kmp_task_team_list_t));
KMP_BUILD_ASSERT(alignof(kmp_task_team_t *[2]) ==
                 alignof(kmp_task_team_list_t));

union KMP_ALIGN_CACHE kmp_team {
  kmp_base_team_t t;
  double t_align; /* use worst case alignment */
  char t_pad[KMP_PAD(kmp_base_team_t, CACHE_LINE)];
};

typedef union KMP_ALIGN_CACHE kmp_time_global {
  double dt_align; /* use worst case alignment */
  char dt_pad[KMP_PAD(kmp_base_data_t, CACHE_LINE)];
  kmp_base_data_t dt;
} kmp_time_global_t;

typedef struct kmp_base_global {
  /* cache-aligned */
  kmp_time_global_t g_time;

  /* non cache-aligned */
  volatile int g_abort;
  volatile int g_done;

  int g_dynamic;
  enum dynamic_mode g_dynamic_mode;
} kmp_base_global_t;

typedef union KMP_ALIGN_CACHE kmp_global {
  kmp_base_global_t g;
  double g_align; /* use worst case alignment */
  char g_pad[KMP_PAD(kmp_base_global_t, CACHE_LINE)];
} kmp_global_t;

typedef struct kmp_base_root {
  // TODO: GEH - combine r_active with r_in_parallel then r_active ==
  // (r_in_parallel>= 0)
  // TODO: GEH - then replace r_active with t_active_levels if we can to reduce
  // the synch overhead or keeping r_active
  volatile int r_active; /* TRUE if some region in a nest has > 1 thread */
  // keeps a count of active parallel regions per root
  std::atomic<int> r_in_parallel;
  // GEH: This is misnamed, should be r_active_levels
  kmp_team_t *r_root_team;
  kmp_team_t *r_hot_team;
  kmp_info_t *r_uber_thread;
  kmp_lock_t r_begin_lock;
  volatile int r_begin;
  int r_blocktime; /* blocktime for this root and descendants */
#if KMP_AFFINITY_SUPPORTED
  int r_affinity_assigned;
#endif // KMP_AFFINITY_SUPPORTED
} kmp_base_root_t;

typedef union KMP_ALIGN_CACHE kmp_root {
  kmp_base_root_t r;
  double r_align; /* use worst case alignment */
  char r_pad[KMP_PAD(kmp_base_root_t, CACHE_LINE)];
} kmp_root_t;

struct fortran_inx_info {
  kmp_int32 data;
};

// This list type exists to hold old __kmp_threads arrays so that
// old references to them may complete while reallocation takes place when
// expanding the array. The items in this list are kept alive until library
// shutdown.
typedef struct kmp_old_threads_list_t {
  kmp_info_t **threads;
  struct kmp_old_threads_list_t *next;
} kmp_old_threads_list_t;

/* ------------------------------------------------------------------------ */

extern int __kmp_settings;
extern int __kmp_duplicate_library_ok;
#if USE_ITT_BUILD
extern int __kmp_forkjoin_frames;
extern int __kmp_forkjoin_frames_mode;
#endif
extern PACKED_REDUCTION_METHOD_T __kmp_force_reduction_method;
extern int __kmp_determ_red;

#ifdef KMP_DEBUG
extern int kmp_a_debug;
extern int kmp_b_debug;
extern int kmp_c_debug;
extern int kmp_d_debug;
extern int kmp_e_debug;
extern int kmp_f_debug;
#endif /* KMP_DEBUG */

/* For debug information logging using rotating buffer */
#define KMP_DEBUG_BUF_LINES_INIT 512
#define KMP_DEBUG_BUF_LINES_MIN 1

#define KMP_DEBUG_BUF_CHARS_INIT 128
#define KMP_DEBUG_BUF_CHARS_MIN 2

extern int
    __kmp_debug_buf; /* TRUE means use buffer, FALSE means print to stderr */
extern int __kmp_debug_buf_lines; /* How many lines of debug stored in buffer */
extern int
    __kmp_debug_buf_chars; /* How many characters allowed per line in buffer */
extern int __kmp_debug_buf_atomic; /* TRUE means use atomic update of buffer
                                      entry pointer */

extern char *__kmp_debug_buffer; /* Debug buffer itself */
extern std::atomic<int> __kmp_debug_count; /* Counter for number of lines
                                              printed in buffer so far */
extern int __kmp_debug_buf_warn_chars; /* Keep track of char increase
                                          recommended in warnings */
/* end rotating debug buffer */

#ifdef KMP_DEBUG
extern int __kmp_par_range; /* +1 => only go par for constructs in range */

#define KMP_PAR_RANGE_ROUTINE_LEN 1024
extern char __kmp_par_range_routine[KMP_PAR_RANGE_ROUTINE_LEN];
#define KMP_PAR_RANGE_FILENAME_LEN 1024
extern char __kmp_par_range_filename[KMP_PAR_RANGE_FILENAME_LEN];
extern int __kmp_par_range_lb;
extern int __kmp_par_range_ub;
#endif

/* For printing out dynamic storage map for threads and teams */
extern int
    __kmp_storage_map; /* True means print storage map for threads and teams */
extern int __kmp_storage_map_verbose; /* True means storage map includes
                                         placement info */
extern int __kmp_storage_map_verbose_specified;

#if KMP_ARCH_X86 || KMP_ARCH_X86_64
extern kmp_cpuinfo_t __kmp_cpuinfo;
static inline bool __kmp_is_hybrid_cpu() { return __kmp_cpuinfo.flags.hybrid; }
#elif KMP_OS_DARWIN && KMP_ARCH_AARCH64
static inline bool __kmp_is_hybrid_cpu() { return true; }
#else
static inline bool __kmp_is_hybrid_cpu() { return false; }
#endif

extern volatile int __kmp_init_serial;
extern volatile int __kmp_init_gtid;
extern volatile int __kmp_init_common;
extern volatile int __kmp_need_register_serial;
extern volatile int __kmp_init_middle;
extern volatile int __kmp_init_parallel;
#if KMP_USE_MONITOR
extern volatile int __kmp_init_monitor;
#endif
extern volatile int __kmp_init_user_locks;
extern volatile int __kmp_init_hidden_helper_threads;
extern int __kmp_init_counter;
extern int __kmp_root_counter;
extern int __kmp_version;

/* list of address of allocated caches for commons */
extern kmp_cached_addr_t *__kmp_threadpriv_cache_list;

/* Barrier algorithm types and options */
extern kmp_uint32 __kmp_barrier_gather_bb_dflt;
extern kmp_uint32 __kmp_barrier_release_bb_dflt;
extern kmp_bar_pat_e __kmp_barrier_gather_pat_dflt;
extern kmp_bar_pat_e __kmp_barrier_release_pat_dflt;
extern kmp_uint32 __kmp_barrier_gather_branch_bits[bs_last_barrier];
extern kmp_uint32 __kmp_barrier_release_branch_bits[bs_last_barrier];
extern kmp_bar_pat_e __kmp_barrier_gather_pattern[bs_last_barrier];
extern kmp_bar_pat_e __kmp_barrier_release_pattern[bs_last_barrier];
extern char const *__kmp_barrier_branch_bit_env_name[bs_last_barrier];
extern char const *__kmp_barrier_pattern_env_name[bs_last_barrier];
extern char const *__kmp_barrier_type_name[bs_last_barrier];
extern char const *__kmp_barrier_pattern_name[bp_last_bar];

/* Global Locks */
extern kmp_bootstrap_lock_t __kmp_initz_lock; /* control initialization */
extern kmp_bootstrap_lock_t __kmp_forkjoin_lock; /* control fork/join access */
extern kmp_bootstrap_lock_t __kmp_task_team_lock;
extern kmp_bootstrap_lock_t
    __kmp_exit_lock; /* exit() is not always thread-safe */
#if KMP_USE_MONITOR
extern kmp_bootstrap_lock_t
    __kmp_monitor_lock; /* control monitor thread creation */
#endif
extern kmp_bootstrap_lock_t
    __kmp_tp_cached_lock; /* used for the hack to allow threadprivate cache and
                             __kmp_threads expansion to co-exist */

extern kmp_lock_t __kmp_global_lock; /* control OS/global access  */
extern kmp_queuing_lock_t __kmp_dispatch_lock; /* control dispatch access  */
extern kmp_lock_t __kmp_debug_lock; /* control I/O access for KMP_DEBUG */

extern enum library_type __kmp_library;

extern enum sched_type __kmp_sched; /* default runtime scheduling */
extern enum sched_type __kmp_static; /* default static scheduling method */
extern enum sched_type __kmp_guided; /* default guided scheduling method */
extern enum sched_type __kmp_auto; /* default auto scheduling method */
extern int __kmp_chunk; /* default runtime chunk size */
extern int __kmp_force_monotonic; /* whether monotonic scheduling forced */

extern size_t __kmp_stksize; /* stack size per thread         */
#if KMP_USE_MONITOR
extern size_t __kmp_monitor_stksize; /* stack size for monitor thread */
#endif
extern size_t __kmp_stkoffset; /* stack offset per thread       */
extern int __kmp_stkpadding; /* Should we pad root thread(s) stack */

extern size_t
    __kmp_malloc_pool_incr; /* incremental size of pool for kmp_malloc() */
extern int __kmp_env_stksize; /* was KMP_STACKSIZE specified? */
extern int __kmp_env_blocktime; /* was KMP_BLOCKTIME specified? */
extern int __kmp_env_checks; /* was KMP_CHECKS specified?    */
extern int __kmp_env_consistency_check; // was KMP_CONSISTENCY_CHECK specified?
extern int __kmp_generate_warnings; /* should we issue warnings? */
extern int __kmp_reserve_warn; /* have we issued reserve_threads warning? */

#ifdef DEBUG_SUSPEND
extern int __kmp_suspend_count; /* count inside __kmp_suspend_template() */
#endif

extern kmp_int32 __kmp_use_yield;
extern kmp_int32 __kmp_use_yield_exp_set;
extern kmp_uint32 __kmp_yield_init;
extern kmp_uint32 __kmp_yield_next;
extern kmp_uint64 __kmp_pause_init;

/* ------------------------------------------------------------------------- */
extern int __kmp_allThreadsSpecified;

extern size_t __kmp_align_alloc;
/* following data protected by initialization routines */
extern int __kmp_xproc; /* number of processors in the system */
extern int __kmp_avail_proc; /* number of processors available to the process */
extern size_t __kmp_sys_min_stksize; /* system-defined minimum stack size */
extern int __kmp_sys_max_nth; /* system-imposed maximum number of threads */
// maximum total number of concurrently-existing threads on device
extern int __kmp_max_nth;
// maximum total number of concurrently-existing threads in a contention group
extern int __kmp_cg_max_nth;
extern int __kmp_task_max_nth; // max threads used in a task
extern int __kmp_teams_max_nth; // max threads used in a teams construct
extern int __kmp_threads_capacity; /* capacity of the arrays __kmp_threads and
                                      __kmp_root */
extern int __kmp_dflt_team_nth; /* default number of threads in a parallel
                                   region a la OMP_NUM_THREADS */
extern int __kmp_dflt_team_nth_ub; /* upper bound on "" determined at serial
                                      initialization */
extern int __kmp_tp_capacity; /* capacity of __kmp_threads if threadprivate is
                                 used (fixed) */
extern int __kmp_tp_cached; /* whether threadprivate cache has been created
                               (__kmpc_threadprivate_cached()) */
extern int __kmp_dflt_blocktime; /* number of microseconds to wait before
                                    blocking (env setting) */
extern char __kmp_blocktime_units; /* 'm' or 'u' to note units specified */
extern bool __kmp_wpolicy_passive; /* explicitly set passive wait policy */

// Convert raw blocktime from ms to us if needed.
static inline void __kmp_aux_convert_blocktime(int *bt) {
  if (__kmp_blocktime_units == 'm') {
    if (*bt > INT_MAX / 1000) {
      *bt = INT_MAX / 1000;
      KMP_INFORM(MaxValueUsing, "kmp_set_blocktime(ms)", bt);
    }
    *bt = *bt * 1000;
  }
}

#if KMP_USE_MONITOR
extern int
    __kmp_monitor_wakeups; /* number of times monitor wakes up per second */
extern int __kmp_bt_intervals; /* number of monitor timestamp intervals before
                                  blocking */
#endif
#ifdef KMP_ADJUST_BLOCKTIME
extern int __kmp_zero_bt; /* whether blocktime has been forced to zero */
#endif /* KMP_ADJUST_BLOCKTIME */
#ifdef KMP_DFLT_NTH_CORES
extern int __kmp_ncores; /* Total number of cores for threads placement */
#endif
/* Number of millisecs to delay on abort for Intel(R) VTune(TM) tools */
extern int __kmp_abort_delay;

extern int __kmp_need_register_atfork_specified;
extern int __kmp_need_register_atfork; /* At initialization, call pthread_atfork
                                          to install fork handler */
extern int __kmp_gtid_mode; /* Method of getting gtid, values:
                               0 - not set, will be set at runtime
                               1 - using stack search
                               2 - dynamic TLS (pthread_getspecific(Linux* OS/OS
                                   X*) or TlsGetValue(Windows* OS))
                               3 - static TLS (__declspec(thread) __kmp_gtid),
                                   Linux* OS .so only.  */
extern int
    __kmp_adjust_gtid_mode; /* If true, adjust method based on #threads */
#ifdef KMP_TDATA_GTID
extern KMP_THREAD_LOCAL int __kmp_gtid;
#endif
extern int __kmp_tls_gtid_min; /* #threads below which use sp search for gtid */
extern int __kmp_foreign_tp; // If true, separate TP var for each foreign thread
#if KMP_ARCH_X86 || KMP_ARCH_X86_64
extern int __kmp_inherit_fp_control; // copy fp creg(s) parent->workers at fork
extern kmp_int16 __kmp_init_x87_fpu_control_word; // init thread's FP ctrl reg
extern kmp_uint32 __kmp_init_mxcsr; /* init thread's mxscr */
#endif /* KMP_ARCH_X86 || KMP_ARCH_X86_64 */

// max_active_levels for nested parallelism enabled by default via
// OMP_MAX_ACTIVE_LEVELS, OMP_NESTED, OMP_NUM_THREADS, and OMP_PROC_BIND
extern int __kmp_dflt_max_active_levels;
// Indicates whether value of __kmp_dflt_max_active_levels was already
// explicitly set by OMP_MAX_ACTIVE_LEVELS or OMP_NESTED=false
extern bool __kmp_dflt_max_active_levels_set;
extern int __kmp_dispatch_num_buffers; /* max possible dynamic loops in
                                          concurrent execution per team */
#if KMP_NESTED_HOT_TEAMS
extern int __kmp_hot_teams_mode;
extern int __kmp_hot_teams_max_level;
#endif

#if KMP_OS_LINUX
extern enum clock_function_type __kmp_clock_function;
extern int __kmp_clock_function_param;
#endif /* KMP_OS_LINUX */

#if KMP_MIC_SUPPORTED
extern enum mic_type __kmp_mic_type;
#endif

#ifdef USE_LOAD_BALANCE
extern double __kmp_load_balance_interval; // load balance algorithm interval
#endif /* USE_LOAD_BALANCE */

#if KMP_USE_ADAPTIVE_LOCKS

// Parameters for the speculative lock backoff system.
struct kmp_adaptive_backoff_params_t {
  // Number of soft retries before it counts as a hard retry.
  kmp_uint32 max_soft_retries;
  // Badness is a bit mask : 0,1,3,7,15,... on each hard failure we move one to
  // the right
  kmp_uint32 max_badness;
};

extern kmp_adaptive_backoff_params_t __kmp_adaptive_backoff_params;

#if KMP_DEBUG_ADAPTIVE_LOCKS
extern const char *__kmp_speculative_statsfile;
#endif

#endif // KMP_USE_ADAPTIVE_LOCKS

extern int __kmp_display_env; /* TRUE or FALSE */
extern int __kmp_display_env_verbose; /* TRUE if OMP_DISPLAY_ENV=VERBOSE */
extern int __kmp_omp_cancellation; /* TRUE or FALSE */
extern int __kmp_nteams;
extern int __kmp_teams_thread_limit;

/* ------------------------------------------------------------------------- */

/* the following are protected by the fork/join lock */
/* write: lock  read: anytime */
extern kmp_info_t **__kmp_threads; /* Descriptors for the threads */
/* Holds old arrays of __kmp_threads until library shutdown */
extern kmp_old_threads_list_t *__kmp_old_threads_list;
/* read/write: lock */
extern volatile kmp_team_t *__kmp_team_pool;
extern volatile kmp_info_t *__kmp_thread_pool;
extern kmp_info_t *__kmp_thread_pool_insert_pt;

// total num threads reachable from some root thread including all root threads
extern volatile int __kmp_nth;
/* total number of threads reachable from some root thread including all root
   threads, and those in the thread pool */
extern volatile int __kmp_all_nth;
extern std::atomic<int> __kmp_thread_pool_active_nth;

extern kmp_root_t **__kmp_root; /* root of thread hierarchy */
/* end data protected by fork/join lock */
/* ------------------------------------------------------------------------- */

#define __kmp_get_gtid() __kmp_get_global_thread_id()
#define __kmp_entry_gtid() __kmp_get_global_thread_id_reg()
#define __kmp_get_tid() (__kmp_tid_from_gtid(__kmp_get_gtid()))
#define __kmp_get_team() (__kmp_threads[(__kmp_get_gtid())]->th.th_team)
#define __kmp_get_thread() (__kmp_thread_from_gtid(__kmp_get_gtid()))

// AT: Which way is correct?
// AT: 1. nproc = __kmp_threads[ ( gtid ) ] -> th.th_team -> t.t_nproc;
// AT: 2. nproc = __kmp_threads[ ( gtid ) ] -> th.th_team_nproc;
#define __kmp_get_team_num_threads(gtid)                                       \
  (__kmp_threads[(gtid)]->th.th_team->t.t_nproc)

static inline bool KMP_UBER_GTID(int gtid) {
  KMP_DEBUG_ASSERT(gtid >= KMP_GTID_MIN);
  KMP_DEBUG_ASSERT(gtid < __kmp_threads_capacity);
  return (gtid >= 0 && __kmp_root[gtid] && __kmp_threads[gtid] &&
          __kmp_threads[gtid] == __kmp_root[gtid]->r.r_uber_thread);
}

static inline int __kmp_tid_from_gtid(int gtid) {
  KMP_DEBUG_ASSERT(gtid >= 0);
  return __kmp_threads[gtid]->th.th_info.ds.ds_tid;
}

static inline int __kmp_gtid_from_tid(int tid, const kmp_team_t *team) {
  KMP_DEBUG_ASSERT(tid >= 0 && team);
  return team->t.t_threads[tid]->th.th_info.ds.ds_gtid;
}

static inline int __kmp_gtid_from_thread(const kmp_info_t *thr) {
  KMP_DEBUG_ASSERT(thr);
  return thr->th.th_info.ds.ds_gtid;
}

static inline kmp_info_t *__kmp_thread_from_gtid(int gtid) {
  KMP_DEBUG_ASSERT(gtid >= 0);
  return __kmp_threads[gtid];
}

static inline kmp_team_t *__kmp_team_from_gtid(int gtid) {
  KMP_DEBUG_ASSERT(gtid >= 0);
  return __kmp_threads[gtid]->th.th_team;
}

static inline void __kmp_assert_valid_gtid(kmp_int32 gtid) {
  if (UNLIKELY(gtid < 0 || gtid >= __kmp_threads_capacity))
    KMP_FATAL(ThreadIdentInvalid);
}

#if KMP_HAVE_MWAIT || KMP_HAVE_UMWAIT
extern int __kmp_user_level_mwait; // TRUE or FALSE; from KMP_USER_LEVEL_MWAIT
extern int __kmp_umwait_enabled; // Runtime check if user-level mwait enabled
extern int __kmp_mwait_enabled; // Runtime check if ring3 mwait is enabled
extern int __kmp_mwait_hints; // Hints to pass in to mwait
#endif

#if KMP_HAVE_UMWAIT
extern int __kmp_waitpkg_enabled; // Runtime check if waitpkg exists
extern int __kmp_tpause_state; // 0 (default), 1=C0.1, 2=C0.2; from KMP_TPAUSE
extern int __kmp_tpause_hint; // 1=C0.1 (default), 0=C0.2; from KMP_TPAUSE
extern int __kmp_tpause_enabled; // 0 (default), 1 (KMP_TPAUSE is non-zero)
#endif

/* ------------------------------------------------------------------------- */

extern kmp_global_t __kmp_global; /* global status */

extern kmp_info_t __kmp_monitor;
// For Debugging Support Library
extern std::atomic<kmp_int32> __kmp_team_counter;
// For Debugging Support Library
extern std::atomic<kmp_int32> __kmp_task_counter;

#if USE_DEBUGGER
#define _KMP_GEN_ID(counter)                                                   \
  (__kmp_debugging ? KMP_ATOMIC_INC(&counter) + 1 : ~0)
#else
#define _KMP_GEN_ID(counter) (~0)
#endif /* USE_DEBUGGER */

#define KMP_GEN_TASK_ID() _KMP_GEN_ID(__kmp_task_counter)
#define KMP_GEN_TEAM_ID() _KMP_GEN_ID(__kmp_team_counter)

/* ------------------------------------------------------------------------ */

extern void __kmp_print_storage_map_gtid(int gtid, void *p1, void *p2,
                                         size_t size, char const *format, ...);

extern void __kmp_serial_initialize(void);
extern void __kmp_middle_initialize(void);
extern void __kmp_parallel_initialize(void);

extern void __kmp_internal_begin(void);
extern void __kmp_internal_end_library(int gtid);
extern void __kmp_internal_end_thread(int gtid);
extern void __kmp_internal_end_atexit(void);
extern void __kmp_internal_end_dtor(void);
extern void __kmp_internal_end_dest(void *);

extern int __kmp_register_root(int initial_thread);
extern void __kmp_unregister_root(int gtid);
extern void __kmp_unregister_library(void); // called by __kmp_internal_end()

extern int __kmp_ignore_mppbeg(void);
extern int __kmp_ignore_mppend(void);

extern int __kmp_enter_single(int gtid, ident_t *id_ref, int push_ws);
extern void __kmp_exit_single(int gtid);

extern void __kmp_parallel_deo(int *gtid_ref, int *cid_ref, ident_t *loc_ref);
extern void __kmp_parallel_dxo(int *gtid_ref, int *cid_ref, ident_t *loc_ref);

#ifdef USE_LOAD_BALANCE
extern int __kmp_get_load_balance(int);
#endif

extern int __kmp_get_global_thread_id(void);
extern int __kmp_get_global_thread_id_reg(void);
extern void __kmp_exit_thread(int exit_status);
extern void __kmp_abort(char const *format, ...);
extern void __kmp_abort_thread(void);
KMP_NORETURN extern void __kmp_abort_process(void);
extern void __kmp_warn(char const *format, ...);

extern void __kmp_set_num_threads(int new_nth, int gtid);

extern bool __kmp_detect_shm();
extern bool __kmp_detect_tmp();

// Returns current thread (pointer to kmp_info_t). Current thread *must* be
// registered.
static inline kmp_info_t *__kmp_entry_thread() {
  int gtid = __kmp_entry_gtid();

  return __kmp_threads[gtid];
}

extern void __kmp_set_max_active_levels(int gtid, int new_max_active_levels);
extern int __kmp_get_max_active_levels(int gtid);
extern int __kmp_get_ancestor_thread_num(int gtid, int level);
extern int __kmp_get_team_size(int gtid, int level);
extern void __kmp_set_schedule(int gtid, kmp_sched_t new_sched, int chunk);
extern void __kmp_get_schedule(int gtid, kmp_sched_t *sched, int *chunk);

extern unsigned short __kmp_get_random(kmp_info_t *thread);
extern void __kmp_init_random(kmp_info_t *thread);

extern kmp_r_sched_t __kmp_get_schedule_global(void);
extern void __kmp_adjust_num_threads(int new_nproc);
extern void __kmp_check_stksize(size_t *val);

extern void *___kmp_allocate(size_t size KMP_SRC_LOC_DECL);
extern void *___kmp_page_allocate(size_t size KMP_SRC_LOC_DECL);
extern void ___kmp_free(void *ptr KMP_SRC_LOC_DECL);
#define __kmp_allocate(size) ___kmp_allocate((size)KMP_SRC_LOC_CURR)
#define __kmp_page_allocate(size) ___kmp_page_allocate((size)KMP_SRC_LOC_CURR)
#define __kmp_free(ptr) ___kmp_free((ptr)KMP_SRC_LOC_CURR)

#if USE_FAST_MEMORY
extern void *___kmp_fast_allocate(kmp_info_t *this_thr,
                                  size_t size KMP_SRC_LOC_DECL);
extern void ___kmp_fast_free(kmp_info_t *this_thr, void *ptr KMP_SRC_LOC_DECL);
extern void __kmp_free_fast_memory(kmp_info_t *this_thr);
extern void __kmp_initialize_fast_memory(kmp_info_t *this_thr);
#define __kmp_fast_allocate(this_thr, size)                                    \
  ___kmp_fast_allocate((this_thr), (size)KMP_SRC_LOC_CURR)
#define __kmp_fast_free(this_thr, ptr)                                         \
  ___kmp_fast_free((this_thr), (ptr)KMP_SRC_LOC_CURR)
#endif

extern void *___kmp_thread_malloc(kmp_info_t *th, size_t size KMP_SRC_LOC_DECL);
extern void *___kmp_thread_calloc(kmp_info_t *th, size_t nelem,
                                  size_t elsize KMP_SRC_LOC_DECL);
extern void *___kmp_thread_realloc(kmp_info_t *th, void *ptr,
                                   size_t size KMP_SRC_LOC_DECL);
extern void ___kmp_thread_free(kmp_info_t *th, void *ptr KMP_SRC_LOC_DECL);
#define __kmp_thread_malloc(th, size)                                          \
  ___kmp_thread_malloc((th), (size)KMP_SRC_LOC_CURR)
#define __kmp_thread_calloc(th, nelem, elsize)                                 \
  ___kmp_thread_calloc((th), (nelem), (elsize)KMP_SRC_LOC_CURR)
#define __kmp_thread_realloc(th, ptr, size)                                    \
  ___kmp_thread_realloc((th), (ptr), (size)KMP_SRC_LOC_CURR)
#define __kmp_thread_free(th, ptr)                                             \
  ___kmp_thread_free((th), (ptr)KMP_SRC_LOC_CURR)

extern void __kmp_push_num_threads(ident_t *loc, int gtid, int num_threads);
extern void __kmp_push_num_threads_list(ident_t *loc, int gtid,
                                        kmp_uint32 list_length,
                                        int *num_threads_list);
extern void __kmp_set_strict_num_threads(ident_t *loc, int gtid, int sev,
                                         const char *msg);

extern void __kmp_push_proc_bind(ident_t *loc, int gtid,
                                 kmp_proc_bind_t proc_bind);
extern void __kmp_push_num_teams(ident_t *loc, int gtid, int num_teams,
                                 int num_threads);
extern void __kmp_push_num_teams_51(ident_t *loc, int gtid, int num_teams_lb,
                                    int num_teams_ub, int num_threads);

extern void __kmp_yield();

extern void __kmpc_dispatch_init_4(ident_t *loc, kmp_int32 gtid,
                                   enum sched_type schedule, kmp_int32 lb,
                                   kmp_int32 ub, kmp_int32 st, kmp_int32 chunk);
extern void __kmpc_dispatch_init_4u(ident_t *loc, kmp_int32 gtid,
                                    enum sched_type schedule, kmp_uint32 lb,
                                    kmp_uint32 ub, kmp_int32 st,
                                    kmp_int32 chunk);
extern void __kmpc_dispatch_init_8(ident_t *loc, kmp_int32 gtid,
                                   enum sched_type schedule, kmp_int64 lb,
                                   kmp_int64 ub, kmp_int64 st, kmp_int64 chunk);
extern void __kmpc_dispatch_init_8u(ident_t *loc, kmp_int32 gtid,
                                    enum sched_type schedule, kmp_uint64 lb,
                                    kmp_uint64 ub, kmp_int64 st,
                                    kmp_int64 chunk);

extern int __kmpc_dispatch_next_4(ident_t *loc, kmp_int32 gtid,
                                  kmp_int32 *p_last, kmp_int32 *p_lb,
                                  kmp_int32 *p_ub, kmp_int32 *p_st);
extern int __kmpc_dispatch_next_4u(ident_t *loc, kmp_int32 gtid,
                                   kmp_int32 *p_last, kmp_uint32 *p_lb,
                                   kmp_uint32 *p_ub, kmp_int32 *p_st);
extern int __kmpc_dispatch_next_8(ident_t *loc, kmp_int32 gtid,
                                  kmp_int32 *p_last, kmp_int64 *p_lb,
                                  kmp_int64 *p_ub, kmp_int64 *p_st);
extern int __kmpc_dispatch_next_8u(ident_t *loc, kmp_int32 gtid,
                                   kmp_int32 *p_last, kmp_uint64 *p_lb,
                                   kmp_uint64 *p_ub, kmp_int64 *p_st);

extern void __kmpc_dispatch_fini_4(ident_t *loc, kmp_int32 gtid);
extern void __kmpc_dispatch_fini_8(ident_t *loc, kmp_int32 gtid);
extern void __kmpc_dispatch_fini_4u(ident_t *loc, kmp_int32 gtid);
extern void __kmpc_dispatch_fini_8u(ident_t *loc, kmp_int32 gtid);

extern void __kmpc_dispatch_deinit(ident_t *loc, kmp_int32 gtid);

#ifdef KMP_GOMP_COMPAT

extern void __kmp_aux_dispatch_init_4(ident_t *loc, kmp_int32 gtid,
                                      enum sched_type schedule, kmp_int32 lb,
                                      kmp_int32 ub, kmp_int32 st,
                                      kmp_int32 chunk, int push_ws);
extern void __kmp_aux_dispatch_init_4u(ident_t *loc, kmp_int32 gtid,
                                       enum sched_type schedule, kmp_uint32 lb,
                                       kmp_uint32 ub, kmp_int32 st,
                                       kmp_int32 chunk, int push_ws);
extern void __kmp_aux_dispatch_init_8(ident_t *loc, kmp_int32 gtid,
                                      enum sched_type schedule, kmp_int64 lb,
                                      kmp_int64 ub, kmp_int64 st,
                                      kmp_int64 chunk, int push_ws);
extern void __kmp_aux_dispatch_init_8u(ident_t *loc, kmp_int32 gtid,
                                       enum sched_type schedule, kmp_uint64 lb,
                                       kmp_uint64 ub, kmp_int64 st,
                                       kmp_int64 chunk, int push_ws);
extern void __kmp_aux_dispatch_fini_chunk_4(ident_t *loc, kmp_int32 gtid);
extern void __kmp_aux_dispatch_fini_chunk_8(ident_t *loc, kmp_int32 gtid);
extern void __kmp_aux_dispatch_fini_chunk_4u(ident_t *loc, kmp_int32 gtid);
extern void __kmp_aux_dispatch_fini_chunk_8u(ident_t *loc, kmp_int32 gtid);

#endif /* KMP_GOMP_COMPAT */

extern kmp_uint32 __kmp_eq_4(kmp_uint32 value, kmp_uint32 checker);
extern kmp_uint32 __kmp_neq_4(kmp_uint32 value, kmp_uint32 checker);
extern kmp_uint32 __kmp_lt_4(kmp_uint32 value, kmp_uint32 checker);
extern kmp_uint32 __kmp_ge_4(kmp_uint32 value, kmp_uint32 checker);
extern kmp_uint32 __kmp_le_4(kmp_uint32 value, kmp_uint32 checker);
extern kmp_uint32 __kmp_wait_4(kmp_uint32 volatile *spinner, kmp_uint32 checker,
                               kmp_uint32 (*pred)(kmp_uint32, kmp_uint32),
                               void *obj);
extern void __kmp_wait_4_ptr(void *spinner, kmp_uint32 checker,
                             kmp_uint32 (*pred)(void *, kmp_uint32), void *obj);

extern void __kmp_wait_64(kmp_info_t *this_thr, kmp_flag_64<> *flag,
                          int final_spin
#if USE_ITT_BUILD
                          ,
                          void *itt_sync_obj
#endif
);
extern void __kmp_release_64(kmp_flag_64<> *flag);

extern void __kmp_infinite_loop(void);

extern void __kmp_cleanup(void);

#if KMP_HANDLE_SIGNALS
extern int __kmp_handle_signals;
extern void __kmp_install_signals(int parallel_init);
extern void __kmp_remove_signals(void);
#endif

extern void __kmp_clear_system_time(void);
extern void __kmp_read_system_time(double *delta);

extern void __kmp_check_stack_overlap(kmp_info_t *thr);

extern void __kmp_expand_host_name(char *buffer, size_t size);
extern void __kmp_expand_file_name(char *result, size_t rlen, char *pattern);

#if KMP_ARCH_X86 || KMP_ARCH_X86_64 || (KMP_OS_WINDOWS && (KMP_ARCH_AARCH64 || KMP_ARCH_ARM))
extern void
__kmp_initialize_system_tick(void); /* Initialize timer tick value */
#endif

extern void
__kmp_runtime_initialize(void); /* machine specific initialization */
extern void __kmp_runtime_destroy(void);

#if KMP_AFFINITY_SUPPORTED
extern char *__kmp_affinity_print_mask(char *buf, int buf_len,
                                       kmp_affin_mask_t *mask);
extern kmp_str_buf_t *__kmp_affinity_str_buf_mask(kmp_str_buf_t *buf,
                                                  kmp_affin_mask_t *mask);
extern void __kmp_affinity_initialize(kmp_affinity_t &affinity);
extern void __kmp_affinity_uninitialize(void);
extern void __kmp_affinity_set_init_mask(
    int gtid, int isa_root); /* set affinity according to KMP_AFFINITY */
void __kmp_affinity_bind_init_mask(int gtid);
extern void __kmp_affinity_bind_place(int gtid);
extern void __kmp_affinity_determine_capable(const char *env_var);
extern int __kmp_aux_set_affinity(void **mask);
extern int __kmp_aux_get_affinity(void **mask);
extern int __kmp_aux_get_affinity_max_proc();
extern int __kmp_aux_set_affinity_mask_proc(int proc, void **mask);
extern int __kmp_aux_unset_affinity_mask_proc(int proc, void **mask);
extern int __kmp_aux_get_affinity_mask_proc(int proc, void **mask);
extern void __kmp_balanced_affinity(kmp_info_t *th, int team_size);
#if KMP_WEIGHTED_ITERATIONS_SUPPORTED
extern int __kmp_get_first_osid_with_ecore(void);
#endif
#if KMP_OS_LINUX || KMP_OS_FREEBSD || KMP_OS_NETBSD || KMP_OS_DRAGONFLY ||     \
    KMP_OS_AIX
extern int kmp_set_thread_affinity_mask_initial(void);
#endif
static inline void __kmp_assign_root_init_mask() {
  int gtid = __kmp_entry_gtid();
  kmp_root_t *r = __kmp_threads[gtid]->th.th_root;
  if (r->r.r_uber_thread == __kmp_threads[gtid] && !r->r.r_affinity_assigned) {
    __kmp_affinity_set_init_mask(gtid, /*isa_root=*/TRUE);
    __kmp_affinity_bind_init_mask(gtid);
    r->r.r_affinity_assigned = TRUE;
  }
}
static inline void __kmp_reset_root_init_mask(int gtid) {
  if (!KMP_AFFINITY_CAPABLE())
    return;
  kmp_info_t *th = __kmp_threads[gtid];
  kmp_root_t *r = th->th.th_root;
  if (r->r.r_uber_thread == th && r->r.r_affinity_assigned) {
    __kmp_set_system_affinity(__kmp_affin_origMask, FALSE);
    KMP_CPU_COPY(th->th.th_affin_mask, __kmp_affin_origMask);
    r->r.r_affinity_assigned = FALSE;
  }
}
#else /* KMP_AFFINITY_SUPPORTED */
#define __kmp_assign_root_init_mask() /* Nothing */
static inline void __kmp_reset_root_init_mask(int gtid) {}
#endif /* KMP_AFFINITY_SUPPORTED */
// No need for KMP_AFFINITY_SUPPORTED guard as only one field in the
// format string is for affinity, so platforms that do not support
// affinity can still use the other fields, e.g., %n for num_threads
extern size_t __kmp_aux_capture_affinity(int gtid, const char *format,
                                         kmp_str_buf_t *buffer);
extern void __kmp_aux_display_affinity(int gtid, const char *format);

extern void __kmp_cleanup_hierarchy();
extern void __kmp_get_hierarchy(kmp_uint32 nproc, kmp_bstate_t *thr_bar);

#if KMP_USE_FUTEX

extern int __kmp_futex_determine_capable(void);

#endif // KMP_USE_FUTEX

extern void __kmp_gtid_set_specific(int gtid);
extern int __kmp_gtid_get_specific(void);

extern double __kmp_read_cpu_time(void);

extern int __kmp_read_system_info(struct kmp_sys_info *info);

#if KMP_USE_MONITOR
extern void __kmp_create_monitor(kmp_info_t *th);
#endif

extern void *__kmp_launch_thread(kmp_info_t *thr);

extern void __kmp_create_worker(int gtid, kmp_info_t *th, size_t stack_size);

#if KMP_OS_WINDOWS
extern int __kmp_still_running(kmp_info_t *th);
extern int __kmp_is_thread_alive(kmp_info_t *th, DWORD *exit_val);
extern void __kmp_free_handle(kmp_thread_t tHandle);
#endif

#if KMP_USE_MONITOR
extern void __kmp_reap_monitor(kmp_info_t *th);
#endif
extern void __kmp_reap_worker(kmp_info_t *th);
extern void __kmp_terminate_thread(int gtid);

extern int __kmp_try_suspend_mx(kmp_info_t *th);
extern void __kmp_lock_suspend_mx(kmp_info_t *th);
extern void __kmp_unlock_suspend_mx(kmp_info_t *th);

extern void __kmp_elapsed(double *);
extern void __kmp_elapsed_tick(double *);

extern void __kmp_enable(int old_state);
extern void __kmp_disable(int *old_state);

extern void __kmp_thread_sleep(int millis);

extern void __kmp_common_initialize(void);
extern void __kmp_common_destroy(void);
extern void __kmp_common_destroy_gtid(int gtid);

#if KMP_OS_UNIX
extern void __kmp_register_atfork(void);
#endif
extern void __kmp_suspend_initialize(void);
extern void __kmp_suspend_initialize_thread(kmp_info_t *th);
extern void __kmp_suspend_uninitialize_thread(kmp_info_t *th);

extern kmp_info_t *__kmp_allocate_thread(kmp_root_t *root, kmp_team_t *team,
                                         int tid);
extern kmp_team_t *
__kmp_allocate_team(kmp_root_t *root, int new_nproc, int max_nproc,
#if OMPT_SUPPORT
                    ompt_data_t ompt_parallel_data,
#endif
                    kmp_proc_bind_t proc_bind, kmp_internal_control_t *new_icvs,
                    int argc USE_NESTED_HOT_ARG(kmp_info_t *thr));
extern void __kmp_free_thread(kmp_info_t *);
extern void __kmp_free_team(kmp_root_t *,
                            kmp_team_t *USE_NESTED_HOT_ARG(kmp_info_t *));
extern kmp_team_t *__kmp_reap_team(kmp_team_t *);

/* ------------------------------------------------------------------------ */

extern void __kmp_initialize_bget(kmp_info_t *th);
extern void __kmp_finalize_bget(kmp_info_t *th);

KMP_EXPORT void *kmpc_malloc(size_t size);
KMP_EXPORT void *kmpc_aligned_malloc(size_t size, size_t alignment);
KMP_EXPORT void *kmpc_calloc(size_t nelem, size_t elsize);
KMP_EXPORT void *kmpc_realloc(void *ptr, size_t size);
KMP_EXPORT void kmpc_free(void *ptr);

/* declarations for internal use */

extern int __kmp_barrier(enum barrier_type bt, int gtid, int is_split,
                         size_t reduce_size, void *reduce_data,
                         void (*reduce)(void *, void *));
extern void __kmp_end_split_barrier(enum barrier_type bt, int gtid);
extern int __kmp_barrier_gomp_cancel(int gtid);

/*!
 * Tell the fork call which compiler generated the fork call, and therefore how
 * to deal with the call.
 */
enum fork_context_e {
  fork_context_gnu, /**< Called from GNU generated code, so must not invoke the
                       microtask internally. */
  fork_context_intel, /**< Called from Intel generated code.  */
  fork_context_last
};
extern int __kmp_fork_call(ident_t *loc, int gtid,
                           enum fork_context_e fork_context, kmp_int32 argc,
                           microtask_t microtask, launch_t invoker,
                           kmp_va_list ap);

extern void __kmp_join_call(ident_t *loc, int gtid
#if OMPT_SUPPORT
                            ,
                            enum fork_context_e fork_context
#endif
                            ,
                            int exit_teams = 0);

extern void __kmp_serialized_parallel(ident_t *id, kmp_int32 gtid);
extern void __kmp_internal_fork(ident_t *id, int gtid, kmp_team_t *team);
extern void __kmp_internal_join(ident_t *id, int gtid, kmp_team_t *team);
extern int __kmp_invoke_task_func(int gtid);
extern void __kmp_run_before_invoked_task(int gtid, int tid,
                                          kmp_info_t *this_thr,
                                          kmp_team_t *team);
extern void __kmp_run_after_invoked_task(int gtid, int tid,
                                         kmp_info_t *this_thr,
                                         kmp_team_t *team);

// should never have been exported
KMP_EXPORT int __kmpc_invoke_task_func(int gtid);
extern int __kmp_invoke_teams_master(int gtid);
extern void __kmp_teams_master(int gtid);
extern int __kmp_aux_get_team_num();
extern int __kmp_aux_get_num_teams();
extern void __kmp_save_internal_controls(kmp_info_t *thread);
extern void __kmp_user_set_library(enum library_type arg);
extern void __kmp_aux_set_library(enum library_type arg);
extern void __kmp_aux_set_stacksize(size_t arg);
extern void __kmp_aux_set_blocktime(int arg, kmp_info_t *thread, int tid);
extern void __kmp_aux_set_defaults(char const *str, size_t len);

/* Functions called from __kmp_aux_env_initialize() in kmp_settings.cpp */
void kmpc_set_blocktime(int arg);
void ompc_set_nested(int flag);
void ompc_set_dynamic(int flag);
void ompc_set_num_threads(int arg);

extern void __kmp_push_current_task_to_thread(kmp_info_t *this_thr,
                                              kmp_team_t *team, int tid);
extern void __kmp_pop_current_task_from_thread(kmp_info_t *this_thr);
extern kmp_task_t *__kmp_task_alloc(ident_t *loc_ref, kmp_int32 gtid,
                                    kmp_tasking_flags_t *flags,
                                    size_t sizeof_kmp_task_t,
                                    size_t sizeof_shareds,
                                    kmp_routine_entry_t task_entry);
extern void __kmp_init_implicit_task(ident_t *loc_ref, kmp_info_t *this_thr,
                                     kmp_team_t *team, int tid,
                                     int set_curr_task);
extern void __kmp_finish_implicit_task(kmp_info_t *this_thr);
extern void __kmp_free_implicit_task(kmp_info_t *this_thr);

extern kmp_event_t *__kmpc_task_allow_completion_event(ident_t *loc_ref,
                                                       int gtid,
                                                       kmp_task_t *task);
extern void __kmp_fulfill_event(kmp_event_t *event);

extern void __kmp_free_task_team(kmp_info_t *thread,
                                 kmp_task_team_t *task_team);
extern void __kmp_reap_task_teams(void);
extern void __kmp_push_task_team_node(kmp_info_t *thread, kmp_team_t *team);
extern void __kmp_pop_task_team_node(kmp_info_t *thread, kmp_team_t *team);
extern void __kmp_wait_to_unref_task_teams(void);
extern void __kmp_task_team_setup(kmp_info_t *this_thr, kmp_team_t *team);
extern void __kmp_task_team_sync(kmp_info_t *this_thr, kmp_team_t *team);
extern void __kmp_task_team_wait(kmp_info_t *this_thr, kmp_team_t *team
#if USE_ITT_BUILD
                                 ,
                                 void *itt_sync_obj
#endif /* USE_ITT_BUILD */
                                 ,
                                 int wait = 1);
extern void __kmp_tasking_barrier(kmp_team_t *team, kmp_info_t *thread,
                                  int gtid);
#if KMP_DEBUG
#define KMP_DEBUG_ASSERT_TASKTEAM_INVARIANT(team, thr)                         \
  KMP_DEBUG_ASSERT(                                                            \
      __kmp_tasking_mode != tskm_task_teams || team->t.t_nproc == 1 ||         \
      thr->th.th_task_team == team->t.t_task_team[thr->th.th_task_state])
#else
#define KMP_DEBUG_ASSERT_TASKTEAM_INVARIANT(team, thr) /* Nothing */
#endif

extern int __kmp_is_address_mapped(void *addr);
extern kmp_uint64 __kmp_hardware_timestamp(void);

#if KMP_OS_UNIX
extern int __kmp_read_from_file(char const *path, char const *format, ...);
#endif

/* ------------------------------------------------------------------------ */
//
// Assembly routines that have no compiler intrinsic replacement
//

extern int __kmp_invoke_microtask(microtask_t pkfn, int gtid, int npr, int argc,
                                  void *argv[]
#if OMPT_SUPPORT
                                  ,
                                  void **exit_frame_ptr
#endif
);

/* ------------------------------------------------------------------------ */

KMP_EXPORT void __kmpc_begin(ident_t *, kmp_int32 flags);
KMP_EXPORT void __kmpc_end(ident_t *);

KMP_EXPORT void __kmpc_threadprivate_register_vec(ident_t *, void *data,
                                                  kmpc_ctor_vec ctor,
                                                  kmpc_cctor_vec cctor,
                                                  kmpc_dtor_vec dtor,
                                                  size_t vector_length);
KMP_EXPORT void __kmpc_threadprivate_register(ident_t *, void *data,
                                              kmpc_ctor ctor, kmpc_cctor cctor,
                                              kmpc_dtor dtor);
KMP_EXPORT void *__kmpc_threadprivate(ident_t *, kmp_int32 global_tid,
                                      void *data, size_t size);

KMP_EXPORT kmp_int32 __kmpc_global_thread_num(ident_t *);
KMP_EXPORT kmp_int32 __kmpc_global_num_threads(ident_t *);
KMP_EXPORT kmp_int32 __kmpc_bound_thread_num(ident_t *);
KMP_EXPORT kmp_int32 __kmpc_bound_num_threads(ident_t *);

KMP_EXPORT kmp_int32 __kmpc_ok_to_fork(ident_t *);
KMP_EXPORT void __kmpc_fork_call(ident_t *, kmp_int32 nargs,
                                 kmpc_micro microtask, ...);
KMP_EXPORT void __kmpc_fork_call_if(ident_t *loc, kmp_int32 nargs,
                                    kmpc_micro microtask, kmp_int32 cond,
                                    void *args);

KMP_EXPORT void __kmpc_serialized_parallel(ident_t *, kmp_int32 global_tid);
KMP_EXPORT void __kmpc_end_serialized_parallel(ident_t *, kmp_int32 global_tid);

KMP_EXPORT void __kmpc_flush(ident_t *);
KMP_EXPORT void __kmpc_barrier(ident_t *, kmp_int32 global_tid);
KMP_EXPORT kmp_int32 __kmpc_master(ident_t *, kmp_int32 global_tid);
KMP_EXPORT void __kmpc_end_master(ident_t *, kmp_int32 global_tid);
KMP_EXPORT kmp_int32 __kmpc_masked(ident_t *, kmp_int32 global_tid,
                                   kmp_int32 filter);
KMP_EXPORT void __kmpc_end_masked(ident_t *, kmp_int32 global_tid);
KMP_EXPORT void __kmpc_ordered(ident_t *, kmp_int32 global_tid);
KMP_EXPORT void __kmpc_end_ordered(ident_t *, kmp_int32 global_tid);
KMP_EXPORT void __kmpc_critical(ident_t *, kmp_int32 global_tid,
                                kmp_critical_name *);
KMP_EXPORT void __kmpc_end_critical(ident_t *, kmp_int32 global_tid,
                                    kmp_critical_name *);
KMP_EXPORT void __kmpc_critical_with_hint(ident_t *, kmp_int32 global_tid,
                                          kmp_critical_name *, uint32_t hint);

KMP_EXPORT kmp_int32 __kmpc_barrier_master(ident_t *, kmp_int32 global_tid);
KMP_EXPORT void __kmpc_end_barrier_master(ident_t *, kmp_int32 global_tid);

KMP_EXPORT kmp_int32 __kmpc_barrier_master_nowait(ident_t *,
                                                  kmp_int32 global_tid);

KMP_EXPORT kmp_int32 __kmpc_single(ident_t *, kmp_int32 global_tid);
KMP_EXPORT void __kmpc_end_single(ident_t *, kmp_int32 global_tid);

KMP_EXPORT kmp_int32 __kmpc_sections_init(ident_t *loc, kmp_int32 global_tid);
KMP_EXPORT kmp_int32 __kmpc_next_section(ident_t *loc, kmp_int32 global_tid,
                                         kmp_int32 numberOfSections);
KMP_EXPORT void __kmpc_end_sections(ident_t *loc, kmp_int32 global_tid);

KMP_EXPORT void KMPC_FOR_STATIC_INIT(ident_t *loc, kmp_int32 global_tid,
                                     kmp_int32 schedtype, kmp_int32 *plastiter,
                                     kmp_int *plower, kmp_int *pupper,
                                     kmp_int *pstride, kmp_int incr,
                                     kmp_int chunk);

KMP_EXPORT void __kmpc_for_static_fini(ident_t *loc, kmp_int32 global_tid);

KMP_EXPORT void __kmpc_copyprivate(ident_t *loc, kmp_int32 global_tid,
                                   size_t cpy_size, void *cpy_data,
                                   void (*cpy_func)(void *, void *),
                                   kmp_int32 didit);

KMP_EXPORT void *__kmpc_copyprivate_light(ident_t *loc, kmp_int32 gtid,
                                          void *cpy_data);

extern void KMPC_SET_NUM_THREADS(int arg);
extern void KMPC_SET_DYNAMIC(int flag);
extern void KMPC_SET_NESTED(int flag);

/* OMP 3.0 tasking interface routines */
KMP_EXPORT kmp_int32 __kmpc_omp_task(ident_t *loc_ref, kmp_int32 gtid,
                                     kmp_task_t *new_task);
KMP_EXPORT kmp_task_t *__kmpc_omp_task_alloc(ident_t *loc_ref, kmp_int32 gtid,
                                             kmp_int32 flags,
                                             size_t sizeof_kmp_task_t,
                                             size_t sizeof_shareds,
                                             kmp_routine_entry_t task_entry);
KMP_EXPORT kmp_task_t *__kmpc_omp_target_task_alloc(
    ident_t *loc_ref, kmp_int32 gtid, kmp_int32 flags, size_t sizeof_kmp_task_t,
    size_t sizeof_shareds, kmp_routine_entry_t task_entry, kmp_int64 device_id);
KMP_EXPORT void __kmpc_omp_task_begin_if0(ident_t *loc_ref, kmp_int32 gtid,
                                          kmp_task_t *task);
KMP_EXPORT void __kmpc_omp_task_complete_if0(ident_t *loc_ref, kmp_int32 gtid,
                                             kmp_task_t *task);
KMP_EXPORT kmp_int32 __kmpc_omp_task_parts(ident_t *loc_ref, kmp_int32 gtid,
                                           kmp_task_t *new_task);
KMP_EXPORT kmp_int32 __kmpc_omp_taskwait(ident_t *loc_ref, kmp_int32 gtid);
KMP_EXPORT kmp_int32 __kmpc_omp_taskyield(ident_t *loc_ref, kmp_int32 gtid,
                                          int end_part);

#if TASK_UNUSED
void __kmpc_omp_task_begin(ident_t *loc_ref, kmp_int32 gtid, kmp_task_t *task);
void __kmpc_omp_task_complete(ident_t *loc_ref, kmp_int32 gtid,
                              kmp_task_t *task);
#endif // TASK_UNUSED

/* ------------------------------------------------------------------------ */

KMP_EXPORT void __kmpc_taskgroup(ident_t *loc, int gtid);
KMP_EXPORT void __kmpc_end_taskgroup(ident_t *loc, int gtid);

KMP_EXPORT kmp_int32 __kmpc_omp_task_with_deps(
    ident_t *loc_ref, kmp_int32 gtid, kmp_task_t *new_task, kmp_int32 ndeps,
    kmp_depend_info_t *dep_list, kmp_int32 ndeps_noalias,
    kmp_depend_info_t *noalias_dep_list);

KMP_EXPORT kmp_base_depnode_t *__kmpc_task_get_depnode(kmp_task_t *task);

KMP_EXPORT kmp_depnode_list_t *__kmpc_task_get_successors(kmp_task_t *task);

KMP_EXPORT void __kmpc_omp_wait_deps(ident_t *loc_ref, kmp_int32 gtid,
                                     kmp_int32 ndeps,
                                     kmp_depend_info_t *dep_list,
                                     kmp_int32 ndeps_noalias,
                                     kmp_depend_info_t *noalias_dep_list);
/* __kmpc_omp_taskwait_deps_51 : Function for OpenMP 5.1 nowait clause.
 *                               Placeholder for taskwait with nowait clause.*/
KMP_EXPORT void __kmpc_omp_taskwait_deps_51(ident_t *loc_ref, kmp_int32 gtid,
                                            kmp_int32 ndeps,
                                            kmp_depend_info_t *dep_list,
                                            kmp_int32 ndeps_noalias,
                                            kmp_depend_info_t *noalias_dep_list,
                                            kmp_int32 has_no_wait);

extern kmp_int32 __kmp_omp_task(kmp_int32 gtid, kmp_task_t *new_task,
                                bool serialize_immediate);

KMP_EXPORT kmp_int32 __kmpc_cancel(ident_t *loc_ref, kmp_int32 gtid,
                                   kmp_int32 cncl_kind);
KMP_EXPORT kmp_int32 __kmpc_cancellationpoint(ident_t *loc_ref, kmp_int32 gtid,
                                              kmp_int32 cncl_kind);
KMP_EXPORT kmp_int32 __kmpc_cancel_barrier(ident_t *loc_ref, kmp_int32 gtid);
KMP_EXPORT int __kmp_get_cancellation_status(int cancel_kind);

KMP_EXPORT void __kmpc_proxy_task_completed(kmp_int32 gtid, kmp_task_t *ptask);
KMP_EXPORT void __kmpc_proxy_task_completed_ooo(kmp_task_t *ptask);
KMP_EXPORT void __kmpc_taskloop(ident_t *loc, kmp_int32 gtid, kmp_task_t *task,
                                kmp_int32 if_val, kmp_uint64 *lb,
                                kmp_uint64 *ub, kmp_int64 st, kmp_int32 nogroup,
                                kmp_int32 sched, kmp_uint64 grainsize,
                                void *task_dup);
KMP_EXPORT void __kmpc_taskloop_5(ident_t *loc, kmp_int32 gtid,
                                  kmp_task_t *task, kmp_int32 if_val,
                                  kmp_uint64 *lb, kmp_uint64 *ub, kmp_int64 st,
                                  kmp_int32 nogroup, kmp_int32 sched,
                                  kmp_uint64 grainsize, kmp_int32 modifier,
                                  void *task_dup);
KMP_EXPORT void *__kmpc_task_reduction_init(int gtid, int num_data, void *data);
KMP_EXPORT void *__kmpc_taskred_init(int gtid, int num_data, void *data);
KMP_EXPORT void *__kmpc_task_reduction_get_th_data(int gtid, void *tg, void *d);
KMP_EXPORT void *__kmpc_task_reduction_modifier_init(ident_t *loc, int gtid,
                                                     int is_ws, int num,
                                                     void *data);
KMP_EXPORT void *__kmpc_taskred_modifier_init(ident_t *loc, int gtid, int is_ws,
                                              int num, void *data);
KMP_EXPORT void __kmpc_task_reduction_modifier_fini(ident_t *loc, int gtid,
                                                    int is_ws);
KMP_EXPORT kmp_int32 __kmpc_omp_reg_task_with_affinity(
    ident_t *loc_ref, kmp_int32 gtid, kmp_task_t *new_task, kmp_int32 naffins,
    kmp_task_affinity_info_t *affin_list);
KMP_EXPORT void __kmp_set_num_teams(int num_teams);
KMP_EXPORT int __kmp_get_max_teams(void);
KMP_EXPORT void __kmp_set_teams_thread_limit(int limit);
KMP_EXPORT int __kmp_get_teams_thread_limit(void);

/* Interface target task integration */
KMP_EXPORT void **__kmpc_omp_get_target_async_handle_ptr(kmp_int32 gtid);
KMP_EXPORT bool __kmpc_omp_has_task_team(kmp_int32 gtid);

/* Lock interface routines (fast versions with gtid passed in) */
KMP_EXPORT void __kmpc_init_lock(ident_t *loc, kmp_int32 gtid,
                                 void **user_lock);
KMP_EXPORT void __kmpc_init_nest_lock(ident_t *loc, kmp_int32 gtid,
                                      void **user_lock);
KMP_EXPORT void __kmpc_destroy_lock(ident_t *loc, kmp_int32 gtid,
                                    void **user_lock);
KMP_EXPORT void __kmpc_destroy_nest_lock(ident_t *loc, kmp_int32 gtid,
                                         void **user_lock);
KMP_EXPORT void __kmpc_set_lock(ident_t *loc, kmp_int32 gtid, void **user_lock);
KMP_EXPORT void __kmpc_set_nest_lock(ident_t *loc, kmp_int32 gtid,
                                     void **user_lock);
KMP_EXPORT void __kmpc_unset_lock(ident_t *loc, kmp_int32 gtid,
                                  void **user_lock);
KMP_EXPORT void __kmpc_unset_nest_lock(ident_t *loc, kmp_int32 gtid,
                                       void **user_lock);
KMP_EXPORT int __kmpc_test_lock(ident_t *loc, kmp_int32 gtid, void **user_lock);
KMP_EXPORT int __kmpc_test_nest_lock(ident_t *loc, kmp_int32 gtid,
                                     void **user_lock);

KMP_EXPORT void __kmpc_init_lock_with_hint(ident_t *loc, kmp_int32 gtid,
                                           void **user_lock, uintptr_t hint);
KMP_EXPORT void __kmpc_init_nest_lock_with_hint(ident_t *loc, kmp_int32 gtid,
                                                void **user_lock,
                                                uintptr_t hint);

#if OMPX_TASKGRAPH
// Taskgraph's Record & Replay mechanism
// __kmp_tdg_is_recording: check whether a given TDG is recording
// status: the tdg's current status
static inline bool __kmp_tdg_is_recording(kmp_tdg_status_t status) {
  return status == KMP_TDG_RECORDING;
}

KMP_EXPORT kmp_int32 __kmpc_start_record_task(ident_t *loc, kmp_int32 gtid,
                                              kmp_int32 input_flags,
                                              kmp_int32 tdg_id);
KMP_EXPORT void __kmpc_end_record_task(ident_t *loc, kmp_int32 gtid,
                                       kmp_int32 input_flags, kmp_int32 tdg_id);
#endif
/* Interface to fast scalable reduce methods routines */

KMP_EXPORT kmp_int32 __kmpc_reduce_nowait(
    ident_t *loc, kmp_int32 global_tid, kmp_int32 num_vars, size_t reduce_size,
    void *reduce_data, void (*reduce_func)(void *lhs_data, void *rhs_data),
    kmp_critical_name *lck);
KMP_EXPORT void __kmpc_end_reduce_nowait(ident_t *loc, kmp_int32 global_tid,
                                         kmp_critical_name *lck);
KMP_EXPORT kmp_int32 __kmpc_reduce(
    ident_t *loc, kmp_int32 global_tid, kmp_int32 num_vars, size_t reduce_size,
    void *reduce_data, void (*reduce_func)(void *lhs_data, void *rhs_data),
    kmp_critical_name *lck);
KMP_EXPORT void __kmpc_end_reduce(ident_t *loc, kmp_int32 global_tid,
                                  kmp_critical_name *lck);

/* Internal fast reduction routines */

extern PACKED_REDUCTION_METHOD_T __kmp_determine_reduction_method(
    ident_t *loc, kmp_int32 global_tid, kmp_int32 num_vars, size_t reduce_size,
    void *reduce_data, void (*reduce_func)(void *lhs_data, void *rhs_data),
    kmp_critical_name *lck);

// this function is for testing set/get/determine reduce method
KMP_EXPORT kmp_int32 __kmp_get_reduce_method(void);

KMP_EXPORT kmp_uint64 __kmpc_get_taskid();
KMP_EXPORT kmp_uint64 __kmpc_get_parent_taskid();

// C++ port
// missing 'extern "C"' declarations

KMP_EXPORT kmp_int32 __kmpc_in_parallel(ident_t *loc);
KMP_EXPORT void __kmpc_pop_num_threads(ident_t *loc, kmp_int32 global_tid);
KMP_EXPORT void __kmpc_push_num_threads(ident_t *loc, kmp_int32 global_tid,
                                        kmp_int32 num_threads);
KMP_EXPORT void __kmpc_push_num_threads_strict(ident_t *loc,
                                               kmp_int32 global_tid,
                                               kmp_int32 num_threads,
                                               int severity,
                                               const char *message);

KMP_EXPORT void __kmpc_push_num_threads_list(ident_t *loc, kmp_int32 global_tid,
                                             kmp_uint32 list_length,
                                             kmp_int32 *num_threads_list);
KMP_EXPORT void __kmpc_push_num_threads_list_strict(
    ident_t *loc, kmp_int32 global_tid, kmp_uint32 list_length,
    kmp_int32 *num_threads_list, int severity, const char *message);

KMP_EXPORT void __kmpc_push_proc_bind(ident_t *loc, kmp_int32 global_tid,
                                      int proc_bind);
KMP_EXPORT void __kmpc_push_num_teams(ident_t *loc, kmp_int32 global_tid,
                                      kmp_int32 num_teams,
                                      kmp_int32 num_threads);
KMP_EXPORT void __kmpc_set_thread_limit(ident_t *loc, kmp_int32 global_tid,
                                        kmp_int32 thread_limit);
/* Function for OpenMP 5.1 num_teams clause */
KMP_EXPORT void __kmpc_push_num_teams_51(ident_t *loc, kmp_int32 global_tid,
                                         kmp_int32 num_teams_lb,
                                         kmp_int32 num_teams_ub,
                                         kmp_int32 num_threads);
KMP_EXPORT void __kmpc_fork_teams(ident_t *loc, kmp_int32 argc,
                                  kmpc_micro microtask, ...);
struct kmp_dim { // loop bounds info casted to kmp_int64
  kmp_int64 lo; // lower
  kmp_int64 up; // upper
  kmp_int64 st; // stride
};
KMP_EXPORT void __kmpc_doacross_init(ident_t *loc, kmp_int32 gtid,
                                     kmp_int32 num_dims,
                                     const struct kmp_dim *dims);
KMP_EXPORT void __kmpc_doacross_wait(ident_t *loc, kmp_int32 gtid,
                                     const kmp_int64 *vec);
KMP_EXPORT void __kmpc_doacross_post(ident_t *loc, kmp_int32 gtid,
                                     const kmp_int64 *vec);
KMP_EXPORT void __kmpc_doacross_fini(ident_t *loc, kmp_int32 gtid);

KMP_EXPORT void *__kmpc_threadprivate_cached(ident_t *loc, kmp_int32 global_tid,
                                             void *data, size_t size,
                                             void ***cache);

// The routines below are not exported.
// Consider making them 'static' in corresponding source files.
void kmp_threadprivate_insert_private_data(int gtid, void *pc_addr,
                                           void *data_addr, size_t pc_size);
struct private_common *kmp_threadprivate_insert(int gtid, void *pc_addr,
                                                void *data_addr,
                                                size_t pc_size);
void __kmp_threadprivate_resize_cache(int newCapacity);
void __kmp_cleanup_threadprivate_caches();

// ompc_, kmpc_ entries moved from omp.h.
#if KMP_OS_WINDOWS
#define KMPC_CONVENTION __cdecl
#else
#define KMPC_CONVENTION
#endif

#ifndef __OMP_H
typedef enum omp_sched_t {
  omp_sched_static = 1,
  omp_sched_dynamic = 2,
  omp_sched_guided = 3,
  omp_sched_auto = 4
} omp_sched_t;
typedef void *kmp_affinity_mask_t;
#endif

KMP_EXPORT void KMPC_CONVENTION ompc_set_max_active_levels(int);
KMP_EXPORT void KMPC_CONVENTION ompc_set_schedule(omp_sched_t, int);
KMP_EXPORT int KMPC_CONVENTION ompc_get_ancestor_thread_num(int);
KMP_EXPORT int KMPC_CONVENTION ompc_get_team_size(int);
KMP_EXPORT int KMPC_CONVENTION
kmpc_set_affinity_mask_proc(int, kmp_affinity_mask_t *);
KMP_EXPORT int KMPC_CONVENTION
kmpc_unset_affinity_mask_proc(int, kmp_affinity_mask_t *);
KMP_EXPORT int KMPC_CONVENTION
kmpc_get_affinity_mask_proc(int, kmp_affinity_mask_t *);

KMP_EXPORT void KMPC_CONVENTION kmpc_set_stacksize(int);
KMP_EXPORT void KMPC_CONVENTION kmpc_set_stacksize_s(size_t);
KMP_EXPORT void KMPC_CONVENTION kmpc_set_library(int);
KMP_EXPORT void KMPC_CONVENTION kmpc_set_defaults(char const *);
KMP_EXPORT void KMPC_CONVENTION kmpc_set_disp_num_buffers(int);
void KMP_EXPAND_NAME(ompc_set_affinity_format)(char const *format);
size_t KMP_EXPAND_NAME(ompc_get_affinity_format)(char *buffer, size_t size);
void KMP_EXPAND_NAME(ompc_display_affinity)(char const *format);
size_t KMP_EXPAND_NAME(ompc_capture_affinity)(char *buffer, size_t buf_size,
                                              char const *format);

enum kmp_target_offload_kind {
  tgt_disabled = 0,
  tgt_default = 1,
  tgt_mandatory = 2
};
typedef enum kmp_target_offload_kind kmp_target_offload_kind_t;
// Set via OMP_TARGET_OFFLOAD if specified, defaults to tgt_default otherwise
extern kmp_target_offload_kind_t __kmp_target_offload;
extern int __kmpc_get_target_offload();

// Constants used in libomptarget
#define KMP_DEVICE_DEFAULT -1 // This is libomptarget's default device.
#define KMP_DEVICE_ALL -11 // This is libomptarget's "all devices".

// OMP Pause Resource

// The following enum is used both to set the status in __kmp_pause_status, and
// as the internal equivalent of the externally-visible omp_pause_resource_t.
typedef enum kmp_pause_status_t {
  kmp_not_paused = 0, // status is not paused, or, requesting resume
  kmp_soft_paused = 1, // status is soft-paused, or, requesting soft pause
  kmp_hard_paused = 2, // status is hard-paused, or, requesting hard pause
  kmp_stop_tool_paused = 3 // requesting stop_tool pause
} kmp_pause_status_t;

// This stores the pause state of the runtime
extern kmp_pause_status_t __kmp_pause_status;
extern int __kmpc_pause_resource(kmp_pause_status_t level);
extern int __kmp_pause_resource(kmp_pause_status_t level);
// Soft resume sets __kmp_pause_status, and wakes up all threads.
extern void __kmp_resume_if_soft_paused();
// Hard resume simply resets the status to not paused. Library will appear to
// be uninitialized after hard pause. Let OMP constructs trigger required
// initializations.
static inline void __kmp_resume_if_hard_paused() {
  if (__kmp_pause_status == kmp_hard_paused) {
    __kmp_pause_status = kmp_not_paused;
  }
}

extern void __kmp_omp_display_env(int verbose);

// 1: it is initializing hidden helper team
extern volatile int __kmp_init_hidden_helper;
// 1: the hidden helper team is done
extern volatile int __kmp_hidden_helper_team_done;
// 1: enable hidden helper task
extern kmp_int32 __kmp_enable_hidden_helper;
// Main thread of hidden helper team
extern kmp_info_t *__kmp_hidden_helper_main_thread;
// Descriptors for the hidden helper threads
extern kmp_info_t **__kmp_hidden_helper_threads;
// Number of hidden helper threads
extern kmp_int32 __kmp_hidden_helper_threads_num;
// Number of hidden helper tasks that have not been executed yet
extern std::atomic<kmp_int32> __kmp_unexecuted_hidden_helper_tasks;

extern void __kmp_hidden_helper_initialize();
extern void __kmp_hidden_helper_threads_initz_routine();
extern void __kmp_do_initialize_hidden_helper_threads();
extern void __kmp_hidden_helper_threads_initz_wait();
extern void __kmp_hidden_helper_initz_release();
extern void __kmp_hidden_helper_threads_deinitz_wait();
extern void __kmp_hidden_helper_threads_deinitz_release();
extern void __kmp_hidden_helper_main_thread_wait();
extern void __kmp_hidden_helper_worker_thread_wait();
extern void __kmp_hidden_helper_worker_thread_signal();
extern void __kmp_hidden_helper_main_thread_release();

// Check whether a given thread is a hidden helper thread
#define KMP_HIDDEN_HELPER_THREAD(gtid)                                         \
  ((gtid) >= 1 && (gtid) <= __kmp_hidden_helper_threads_num)

#define KMP_HIDDEN_HELPER_WORKER_THREAD(gtid)                                  \
  ((gtid) > 1 && (gtid) <= __kmp_hidden_helper_threads_num)

#define KMP_HIDDEN_HELPER_MAIN_THREAD(gtid)                                    \
  ((gtid) == 1 && (gtid) <= __kmp_hidden_helper_threads_num)

#define KMP_HIDDEN_HELPER_TEAM(team)                                           \
  (team->t.t_threads[0] == __kmp_hidden_helper_main_thread)

// Map a gtid to a hidden helper thread. The first hidden helper thread, a.k.a
// main thread, is skipped.
#define KMP_GTID_TO_SHADOW_GTID(gtid)                                          \
  ((gtid) % (__kmp_hidden_helper_threads_num - 1) + 2)

// Return the adjusted gtid value by subtracting from gtid the number
// of hidden helper threads. This adjusted value is the gtid the thread would
// have received if there were no hidden helper threads.
static inline int __kmp_adjust_gtid_for_hidden_helpers(int gtid) {
  int adjusted_gtid = gtid;
  if (__kmp_hidden_helper_threads_num > 0 && gtid > 0 &&
      gtid - __kmp_hidden_helper_threads_num >= 0) {
    adjusted_gtid -= __kmp_hidden_helper_threads_num;
  }
  return adjusted_gtid;
}

// Support for error directive
typedef enum kmp_severity_t {
  severity_warning = 1,
  severity_fatal = 2
} kmp_severity_t;
extern void __kmpc_error(ident_t *loc, int severity, const char *message);

// Support for scope directive
KMP_EXPORT void __kmpc_scope(ident_t *loc, kmp_int32 gtid, void *reserved);
KMP_EXPORT void __kmpc_end_scope(ident_t *loc, kmp_int32 gtid, void *reserved);

#ifdef __cplusplus
}
#endif

template <bool C, bool S>
extern void __kmp_suspend_32(int th_gtid, kmp_flag_32<C, S> *flag);
template <bool C, bool S>
extern void __kmp_suspend_64(int th_gtid, kmp_flag_64<C, S> *flag);
template <bool C, bool S>
extern void __kmp_atomic_suspend_64(int th_gtid,
                                    kmp_atomic_flag_64<C, S> *flag);
extern void __kmp_suspend_oncore(int th_gtid, kmp_flag_oncore *flag);
#if KMP_HAVE_MWAIT || KMP_HAVE_UMWAIT
template <bool C, bool S>
extern void __kmp_mwait_32(int th_gtid, kmp_flag_32<C, S> *flag);
template <bool C, bool S>
extern void __kmp_mwait_64(int th_gtid, kmp_flag_64<C, S> *flag);
template <bool C, bool S>
extern void __kmp_atomic_mwait_64(int th_gtid, kmp_atomic_flag_64<C, S> *flag);
extern void __kmp_mwait_oncore(int th_gtid, kmp_flag_oncore *flag);
#endif
template <bool C, bool S>
extern void __kmp_resume_32(int target_gtid, kmp_flag_32<C, S> *flag);
template <bool C, bool S>
extern void __kmp_resume_64(int target_gtid, kmp_flag_64<C, S> *flag);
template <bool C, bool S>
extern void __kmp_atomic_resume_64(int target_gtid,
                                   kmp_atomic_flag_64<C, S> *flag);
extern void __kmp_resume_oncore(int target_gtid, kmp_flag_oncore *flag);

template <bool C, bool S>
int __kmp_execute_tasks_32(kmp_info_t *thread, kmp_int32 gtid,
                           kmp_flag_32<C, S> *flag, int final_spin,
                           int *thread_finished,
#if USE_ITT_BUILD
                           void *itt_sync_obj,
#endif /* USE_ITT_BUILD */
                           kmp_int32 is_constrained);
template <bool C, bool S>
int __kmp_execute_tasks_64(kmp_info_t *thread, kmp_int32 gtid,
                           kmp_flag_64<C, S> *flag, int final_spin,
                           int *thread_finished,
#if USE_ITT_BUILD
                           void *itt_sync_obj,
#endif /* USE_ITT_BUILD */
                           kmp_int32 is_constrained);
template <bool C, bool S>
int __kmp_atomic_execute_tasks_64(kmp_info_t *thread, kmp_int32 gtid,
                                  kmp_atomic_flag_64<C, S> *flag,
                                  int final_spin, int *thread_finished,
#if USE_ITT_BUILD
                                  void *itt_sync_obj,
#endif /* USE_ITT_BUILD */
                                  kmp_int32 is_constrained);
int __kmp_execute_tasks_oncore(kmp_info_t *thread, kmp_int32 gtid,
                               kmp_flag_oncore *flag, int final_spin,
                               int *thread_finished,
#if USE_ITT_BUILD
                               void *itt_sync_obj,
#endif /* USE_ITT_BUILD */
                               kmp_int32 is_constrained);

extern int __kmp_nesting_mode;
extern int __kmp_nesting_mode_nlevels;
extern int *__kmp_nesting_nth_level;
extern void __kmp_init_nesting_mode();
extern void __kmp_set_nesting_mode_threads();

/// This class safely opens and closes a C-style FILE* object using RAII
/// semantics. There are also methods which allow using stdout or stderr as
/// the underlying FILE* object. With the implicit conversion operator to
/// FILE*, an object with this type can be used in any function which takes
/// a FILE* object e.g., fprintf().
/// No close method is needed at use sites.
class kmp_safe_raii_file_t {
  FILE *f;

  void close() {
    if (f && f != stdout && f != stderr) {
      fclose(f);
      f = nullptr;
    }
  }

public:
  kmp_safe_raii_file_t() : f(nullptr) {}
  kmp_safe_raii_file_t(const char *filename, const char *mode,
                       const char *env_var = nullptr)
      : f(nullptr) {
    open(filename, mode, env_var);
  }
  kmp_safe_raii_file_t(const kmp_safe_raii_file_t &other) = delete;
  kmp_safe_raii_file_t &operator=(const kmp_safe_raii_file_t &other) = delete;
  ~kmp_safe_raii_file_t() { close(); }

  /// Open filename using mode. This is automatically closed in the destructor.
  /// The env_var parameter indicates the environment variable the filename
  /// came from if != nullptr.
  void open(const char *filename, const char *mode,
            const char *env_var = nullptr) {
    KMP_ASSERT(!f);
    f = fopen(filename, mode);
    if (!f) {
      int code = errno;
      if (env_var) {
        __kmp_fatal(KMP_MSG(CantOpenFileForReading, filename), KMP_ERR(code),
                    KMP_HNT(CheckEnvVar, env_var, filename), __kmp_msg_null);
      } else {
        __kmp_fatal(KMP_MSG(CantOpenFileForReading, filename), KMP_ERR(code),
                    __kmp_msg_null);
      }
    }
  }
  /// Instead of erroring out, return non-zero when
  /// unsuccessful fopen() for any reason
  int try_open(const char *filename, const char *mode) {
    KMP_ASSERT(!f);
    f = fopen(filename, mode);
    if (!f)
      return errno;
    return 0;
  }
  /// Set the FILE* object to stdout and output there
  /// No open call should happen before this call.
  void set_stdout() {
    KMP_ASSERT(!f);
    f = stdout;
  }
  /// Set the FILE* object to stderr and output there
  /// No open call should happen before this call.
  void set_stderr() {
    KMP_ASSERT(!f);
    f = stderr;
  }
  operator bool() { return bool(f); }
  operator FILE *() { return f; }
};

template <typename SourceType, typename TargetType,
          bool isSourceSmaller = (sizeof(SourceType) < sizeof(TargetType)),
          bool isSourceEqual = (sizeof(SourceType) == sizeof(TargetType)),
          bool isSourceSigned = std::is_signed<SourceType>::value,
          bool isTargetSigned = std::is_signed<TargetType>::value>
struct kmp_convert {};

// Both types are signed; Source smaller
template <typename SourceType, typename TargetType>
struct kmp_convert<SourceType, TargetType, true, false, true, true> {
  static TargetType to(SourceType src) { return (TargetType)src; }
};
// Source equal
template <typename SourceType, typename TargetType>
struct kmp_convert<SourceType, TargetType, false, true, true, true> {
  static TargetType to(SourceType src) { return src; }
};
// Source bigger
template <typename SourceType, typename TargetType>
struct kmp_convert<SourceType, TargetType, false, false, true, true> {
  static TargetType to(SourceType src) {
    KMP_ASSERT(src <= static_cast<SourceType>(
                          (std::numeric_limits<TargetType>::max)()));
    KMP_ASSERT(src >= static_cast<SourceType>(
                          (std::numeric_limits<TargetType>::min)()));
    return (TargetType)src;
  }
};

// Source signed, Target unsigned
// Source smaller
template <typename SourceType, typename TargetType>
struct kmp_convert<SourceType, TargetType, true, false, true, false> {
  static TargetType to(SourceType src) {
    KMP_ASSERT(src >= 0);
    return (TargetType)src;
  }
};
// Source equal
template <typename SourceType, typename TargetType>
struct kmp_convert<SourceType, TargetType, false, true, true, false> {
  static TargetType to(SourceType src) {
    KMP_ASSERT(src >= 0);
    return (TargetType)src;
  }
};
// Source bigger
template <typename SourceType, typename TargetType>
struct kmp_convert<SourceType, TargetType, false, false, true, false> {
  static TargetType to(SourceType src) {
    KMP_ASSERT(src >= 0);
    KMP_ASSERT(src <= static_cast<SourceType>(
                          (std::numeric_limits<TargetType>::max)()));
    return (TargetType)src;
  }
};

// Source unsigned, Target signed
// Source smaller
template <typename SourceType, typename TargetType>
struct kmp_convert<SourceType, TargetType, true, false, false, true> {
  static TargetType to(SourceType src) { return (TargetType)src; }
};
// Source equal
template <typename SourceType, typename TargetType>
struct kmp_convert<SourceType, TargetType, false, true, false, true> {
  static TargetType to(SourceType src) {
    KMP_ASSERT(src <= static_cast<SourceType>(
                          (std::numeric_limits<TargetType>::max)()));
    return (TargetType)src;
  }
};
// Source bigger
template <typename SourceType, typename TargetType>
struct kmp_convert<SourceType, TargetType, false, false, false, true> {
  static TargetType to(SourceType src) {
    KMP_ASSERT(src <= static_cast<SourceType>(
                          (std::numeric_limits<TargetType>::max)()));
    return (TargetType)src;
  }
};

// Source unsigned, Target unsigned
// Source smaller
template <typename SourceType, typename TargetType>
struct kmp_convert<SourceType, TargetType, true, false, false, false> {
  static TargetType to(SourceType src) { return (TargetType)src; }
};
// Source equal
template <typename SourceType, typename TargetType>
struct kmp_convert<SourceType, TargetType, false, true, false, false> {
  static TargetType to(SourceType src) { return src; }
};
// Source bigger
template <typename SourceType, typename TargetType>
struct kmp_convert<SourceType, TargetType, false, false, false, false> {
  static TargetType to(SourceType src) {
    KMP_ASSERT(src <= static_cast<SourceType>(
                          (std::numeric_limits<TargetType>::max)()));
    return (TargetType)src;
  }
};

template <typename T1, typename T2>
static inline void __kmp_type_convert(T1 src, T2 *dest) {
  *dest = kmp_convert<T1, T2>::to(src);
}

#endif /* KMP_H */
