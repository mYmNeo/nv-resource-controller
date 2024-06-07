#ifndef CUDA_ENTRY_H
#define CUDA_ENTRY_H

typedef int (*cuda_sym_t)();

#define CUDA_ENTRY_ENUM(x) ENTRY_##x

#define CUDA_FIND_ENTRY(table, sym) \
  ({ (table)[CUDA_ENTRY_ENUM(sym)].real_pfn; })

#define CUDA_ENTRY_CALL(table, sym, ...)             \
  ({                                                 \
    cuda_sym_t _entry = CUDA_FIND_ENTRY(table, sym); \
    BUG_ON(!_entry);                                 \
    _entry(__VA_ARGS__);                             \
  })

#define HOOK_NAME(NAME) hook_##NAME

#define HOOK_FUNC(NAME) {.name = #NAME, .hook_pfn = HOOK_NAME(NAME)}

/*
 * enum order should keep consistant with <cuda_hook_funcs_data> in hook.c
 */
typedef enum {
  CUDA_ENTRY_ENUM(cuGetProcAddress),
  CUDA_ENTRY_ENUM(cuGetProcAddress_v2),

  CUDA_ENTRY_ENUM(cuLaunchKernel),
  CUDA_ENTRY_ENUM(cuLaunchKernelEx),
  CUDA_ENTRY_ENUM(cuLaunchKernel_ptsz),
  CUDA_ENTRY_ENUM(cuLaunchKernelEx_ptsz),

  ENTRY_END,
} entry_enum_t;

typedef struct CUlaunchConfig_st {
  unsigned int gridDimX;
  unsigned int gridDimY;
  unsigned int gridDimZ;
  unsigned int blockDimX;
  unsigned int blockDimY;
  unsigned int blockDimZ;
} CUlaunchConfig;

#endif
