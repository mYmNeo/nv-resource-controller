#ifndef HOOK_H
#define HOOK_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cuda_entry.h"
#include "list.h"

#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define EXPORT_API __attribute__((__visibility__("default")))

#define RETURN_ADDR(x)                                                         \
  ({                                                                           \
    void *_ret_addr =                                                          \
        __builtin_extract_return_addr(__builtin_return_address(x));            \
    _ret_addr;                                                                 \
  })

#define offsetof(type, member) ((size_t) & ((type *)0)->member)
#define container_of(ptr, type, member)                                        \
  ({                                                                           \
    const typeof(((type *)0)->member) *__mptr =                                \
        (const typeof(((type *)0)->member) *)(ptr);                            \
    (type *)((char *)__mptr - offsetof(type, member));                         \
  })

typedef enum {
  INFO = 0,
  ERROR = 1,
  WARN = 2,
  FATAL = 3,
  VERBOSE = 4,
  DETAIL = 5,
} log_level_enum_t;

#define LOGGER(level, format, ...)                                             \
  ({                                                                           \
    char *_print_level_str = getenv("LOGGER_LEVEL");                           \
    int _print_level = 3;                                                      \
    if (_print_level_str) {                                                    \
      _print_level = (int)strtoul(_print_level_str, NULL, 10);                 \
      _print_level = _print_level < 0 ? 3 : _print_level;                      \
    }                                                                          \
    if (level <= _print_level) {                                               \
      fprintf(stderr, "%s:%d " format "\n", __FILE__, __LINE__,                \
              ##__VA_ARGS__);                                                  \
    }                                                                          \
    if (level == FATAL) {                                                      \
      exit(-1);                                                                \
    }                                                                          \
  })

/*
 * calling noreturn functions, __builtin_unreachable() and __builtin_trap()
 * confuse the stack allocation in gcc, leading to overly large stack
 * frames, see https://gcc.gnu.org/bugzilla/show_bug.cgi?id=82365
 *
 * Adding an empty inline assembly before it works around the problem
 */
#define barrier_before_unreachable() asm volatile("")

#define BUG()                                                                  \
  do {                                                                         \
    fprintf(stderr, "BUG: failure at %s:%d/%s()!\n", __FILE__, __LINE__,       \
            __func__);                                                         \
    barrier_before_unreachable();                                              \
    exit(-1);                                                                  \
  } while (0)

#define BUG_ON(condition)                                                      \
  do {                                                                         \
    if (unlikely(condition))                                                   \
      BUG();                                                                   \
  } while (0)

#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2 * !!(condition)]))

#define UNUSED __attribute__((unused))

/* original functions data item */
typedef struct {
  cuda_sym_t real_pfn;
  cuda_sym_t hook_pfn;
  char *name;
  uint64_t flags;
  int cudaVersion;
} entry_t;

typedef struct {
  pthread_once_t once;
  void *(*dlopen)(const char *, int);
  void *(*dlsym)(void *, const char *);
  int (*dlclose)(void *);
  int (*ioctl)(int fd, uint64_t cmd, void *args);
} dlfcn_t;

typedef struct {
  uint32_t object;
  size_t size;
  struct list_head node;
} device_mem_t;

typedef struct {
  int add_per_cycle;
  int core_limit;
  int mod_times;
  int avg_launchs[2];
  int launch_idx;

  /* thread data */
  uint32_t launch_times;
} token_param_t;

typedef struct {
  int changed;
  int inited;
  struct timespec wait_time;
  token_param_t params;
  sem_t ready;
} token_attr_t;

typedef struct {
  pthread_mutex_t mu;
  pthread_t tid;
  uint32_t major;
  uint32_t minor;
  size_t total_mem;
  size_t free_mem;
  size_t alloc_mem;
  sem_t tokens;
  token_attr_t *attr;
  int mem_limited;
  int core_limited;
  pthread_once_t once;
  struct list_head mem_list;
} device_prop_t;

#define NVIDIA_DEVICE_MAJOR 195
#define NVIDIA_CTL_MINOR 0xFF
/* cuda_hook.<minor>.<cgroup_id> */
#define HOOK_SHM_PATH_PATTERN "/cuda_hook.%x.%s"
#define MAX_CGROUP_ID_LEN 16

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) < (b) ? (b) : (a))

#define LAUNCH_SAMPLES 10

#endif
