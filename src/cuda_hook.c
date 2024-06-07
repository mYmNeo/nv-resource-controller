#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "hook.h"

extern entry_t *find_entry(entry_t *list, int size, const char *symbol);
extern device_prop_t *get_device_prop(void);

static int HOOK_NAME(cuGetProcAddress)(const char *symbol, void **pfn,
                                       int cudaVersion, uint64_t flags);
static int HOOK_NAME(cuGetProcAddress_v2)(const char *symbol, void **pfn,
                                          int cudaVersion, uint64_t flags,
                                          void *symbolStatus);

static int HOOK_NAME(cuLaunchKernel)(
    void *f, unsigned int gridDimX, unsigned int gridDimY,
    unsigned int gridDimZ, unsigned int blockDimX, unsigned int blockDimY,
    unsigned int blockDimZ, unsigned int sharedMemBytes, void *hStream,
    void **kernelParams, void **extra);
static int HOOK_NAME(cuLaunchKernel_ptsz)(
    void *f, unsigned int gridDimX, unsigned int gridDimY,
    unsigned int gridDimZ, unsigned int blockDimX, unsigned int blockDimY,
    unsigned int blockDimZ, unsigned int sharedMemBytes, void *hStream,
    void **kernelParams, void **extra);
static int HOOK_NAME(cuLaunchKernelEx)(const CUlaunchConfig *config, void *f,
                                       void **kernelParams, void **extra);
static int HOOK_NAME(cuLaunchKernelEx_ptsz)(const CUlaunchConfig *config,
                                            void *f, void **kernelParams,
                                            void **extra);

static entry_t cuda_hook_funcs_data[] = {
    HOOK_FUNC(cuGetProcAddress),    HOOK_FUNC(cuGetProcAddress_v2),

    HOOK_FUNC(cuLaunchKernel),      HOOK_FUNC(cuLaunchKernelEx),
    HOOK_FUNC(cuLaunchKernel_ptsz), HOOK_FUNC(cuLaunchKernelEx_ptsz),
};

const static int hook_size = sizeof(cuda_hook_funcs_data) / sizeof(entry_t);

int get_hook_size() { return hook_size; }
entry_t *get_hook_funcs_data() { return cuda_hook_funcs_data; }

static int HOOK_NAME(cuGetProcAddress)(const char *symbol, void **pfn,
                                       int cudaVersion, uint64_t flags) {
  entry_t *e = NULL;
  int ret = 0;

#ifndef NDEBUG
  LOGGER(DETAIL, "call %s symbol:%s", __FUNCTION__, symbol);
#endif
  ret = CUDA_ENTRY_CALL(cuda_hook_funcs_data, cuGetProcAddress, symbol, pfn,
                        cudaVersion, flags);
  if (ret != 0) {
    return ret;
  }

  e = find_entry(cuda_hook_funcs_data, hook_size, symbol);
  if (e) {
    if (likely(!e->real_pfn)) {
      e->real_pfn = *pfn;
    }

    if (likely(e->hook_pfn)) {
      LOGGER(VERBOSE, "replace %s", symbol);
      *pfn = e->hook_pfn;
    }
  }

  return ret;
}

static int HOOK_NAME(cuGetProcAddress_v2)(const char *symbol, void **pfn,
                                          int cudaVersion, uint64_t flags,
                                          void *symbolStatus) {
  entry_t *e = NULL;
  int ret = 0;

#ifndef NDEBUG
  LOGGER(DETAIL, "call %s symbol:%s", __FUNCTION__, symbol);
#endif
  ret = CUDA_ENTRY_CALL(cuda_hook_funcs_data, cuGetProcAddress_v2, symbol, pfn,
                        cudaVersion, flags, symbolStatus);
  if (ret != 0) {
    return ret;
  }

  e = find_entry(cuda_hook_funcs_data, hook_size, symbol);
  if (e) {
    if (likely(!e->real_pfn)) {
      e->real_pfn = *pfn;
    }

    if (likely(e->hook_pfn)) {
      LOGGER(VERBOSE, "replace %s", symbol);
      *pfn = e->hook_pfn;
    }
  }

  return ret;
}

static int rate_limit() {
  int ret = 0;
  device_prop_t *dev = get_device_prop();
  token_param_t *param = &dev->attr->params;

  if (likely(dev->core_limited)) {
    while ((ret = sem_wait(&dev->tokens)) == -1 && errno == EINTR) {
      continue;
    }

    atomic_fetch_add(&param->launch_times, 1);
    if (ret == -1) {
      LOGGER(ERROR, "sem_wait errno:%d, %s", errno, strerror(errno));
      ret = 999;
    }
  }

  return ret;
}

static int HOOK_NAME(cuLaunchKernel)(
    void *f, unsigned int gridDimX, unsigned int gridDimY,
    unsigned int gridDimZ, unsigned int blockDimX, unsigned int blockDimY,
    unsigned int blockDimZ, unsigned int sharedMemBytes, void *hStream,
    void **kernelParams, void **extra) {
  int ret = 0;

  ret = rate_limit();
  if (unlikely(ret)) {
    goto done;
  }
  ret = CUDA_ENTRY_CALL(cuda_hook_funcs_data, cuLaunchKernel, f, gridDimX,
                        gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ,
                        sharedMemBytes, hStream, kernelParams, extra);
done:
  return ret;
}

static int HOOK_NAME(cuLaunchKernel_ptsz)(
    void *f, unsigned int gridDimX, unsigned int gridDimY,
    unsigned int gridDimZ, unsigned int blockDimX, unsigned int blockDimY,
    unsigned int blockDimZ, unsigned int sharedMemBytes, void *hStream,
    void **kernelParams, void **extra) {
  int ret = 0;

  ret = rate_limit();
  if (unlikely(ret)) {
    goto done;
  }

  ret = CUDA_ENTRY_CALL(cuda_hook_funcs_data, cuLaunchKernel_ptsz, f, gridDimX,
                        gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ,
                        sharedMemBytes, hStream, kernelParams, extra);
done:
  return ret;
}

static int HOOK_NAME(cuLaunchKernelEx)(const CUlaunchConfig *config, void *f,
                                       void **kernelParams, void **extra) {
  int ret = 0;

  ret = rate_limit();
  if (unlikely(ret)) {
    goto done;
  }

  ret = CUDA_ENTRY_CALL(cuda_hook_funcs_data, cuLaunchKernelEx, config, f,
                        kernelParams, extra);
done:
  return ret;
}

static int HOOK_NAME(cuLaunchKernelEx_ptsz)(const CUlaunchConfig *config,
                                            void *f, void **kernelParams,
                                            void **extra) {
  int ret = 0;

  ret = rate_limit();
  if (unlikely(ret)) {
    goto done;
  }

  ret = CUDA_ENTRY_CALL(cuda_hook_funcs_data, cuLaunchKernelEx_ptsz, config, f,
                        kernelParams, extra);
done:
  return ret;
}
