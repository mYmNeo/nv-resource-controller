#include "hook.h"

#include "ctrl/ctrl2080/ctrl2080fb.h"
#include "gpu/mem_mgr/rm_page_size.h"
#include "nv_escape.h"
#include "nvos.h"
#include "nvstatus.h"
#include "nvtypes.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

extern int get_mem_limit(uint32_t *minor, size_t *limit);
extern int get_core_limit(uint32_t *minor, size_t *limit);
extern void *create_shm_addr(uint32_t minor, pid_t pid, size_t data_size);
extern int wait_duration(struct timespec *interval);

static device_prop_t gpu_device = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .major = NVIDIA_DEVICE_MAJOR,
    .minor = NVIDIA_CTL_MINOR,
    .total_mem = 0,
    .free_mem = 0,
    .alloc_mem = 0,
    .mem_limited = 0,
    .core_limited = 0,
};

device_prop_t *get_device_prop(void) { return &gpu_device; }

void *token_post(void *arg) {
  device_prop_t *dev = arg;
  struct timespec interval = {0, 0};
  int i = 0, j = 0;
  token_param_t params, *dev_params = NULL;
  int loop = 0;
  int32_t launch_times[LAUNCH_SAMPLES] = {0};
  int32_t sum_launch = 0;

  LOGGER(VERBOSE, "start token post");
  dev_params = &dev->attr->params;
  while (1) {
    loop++;
    if (unlikely(atomic_load(&dev->attr->changed))) {
      interval.tv_nsec = dev->attr->wait_time.tv_nsec;
      params.add_per_cycle = atomic_load(&dev_params->add_per_cycle);
      atomic_store(&dev->attr->changed, 0);
    }

    atomic_store(&dev_params->launch_times, 0);

    wait_duration(&interval);

    for (i = 0; i < params.add_per_cycle; i++) {
      sem_post(&dev->tokens);
    }

    launch_times[loop % 10] = atomic_load(&dev_params->launch_times);
    sum_launch = 0;
    for (i = 0, j = 0; i < LAUNCH_SAMPLES; i++) {
      if (launch_times[i] > 0) {
        sum_launch += launch_times[i];
        j++;
      }
    }
    sum_launch /= (j + 1);
    dev_params->avg_launchs[dev_params->launch_idx % 2] =
        sum_launch ? sum_launch : 1;
    atomic_fetch_add(&dev_params->launch_idx, 1);
  }
}

int pre_ioctl(uint32_t major, uint32_t minor, uint32_t cmd, void *arg,
              int *success) {
  int ret = 0;
  int arg_cmd = 0;
  size_t arg_size = 0;
  NVOS32_PARAMETERS *pApi = arg;
  size_t align_size = 0;

  *success = 0;
  if (unlikely(major != NVIDIA_DEVICE_MAJOR)) {
    ret = -EINVAL;
    goto finish;
  }

  arg_size = _IOC_SIZE(cmd);
  arg_cmd = _IOC_NR(cmd);

  if (unlikely(arg_cmd != NV_ESC_RM_VID_HEAP_CONTROL)) {
    goto finish;
  }

  if (unlikely(minor != NVIDIA_CTL_MINOR)) {
    ret = -EINVAL;
    goto finish;
  }

  if (unlikely(arg_size != sizeof(NVOS32_PARAMETERS))) {
    ret = -EINVAL;
    goto finish;
  }

#ifndef NDEBUG
  LOGGER(DETAIL, "function %d", pApi->function);
#endif

  switch (pApi->function) {
  case NVOS32_FUNCTION_ALLOC_SIZE:
    if (likely(pApi->data.AllocSize.alignment != RM_PAGE_SIZE_INVALID)) {
      align_size =
          (pApi->data.AllocSize.size + pApi->data.AllocSize.alignment - 1) &
          ~(pApi->data.AllocSize.alignment - 1);
#ifndef NDEBUG
      LOGGER(VERBOSE, "size:%lu align:%lu final:%lu", pApi->data.AllocSize.size,
             pApi->data.AllocSize.alignment, align_size);
#endif
      pthread_mutex_lock(&gpu_device.mu);
      if (gpu_device.free_mem < align_size) {
        pApi->status = NV_ERR_NO_MEMORY;
        pApi->total = gpu_device.total_mem;
        pApi->free = gpu_device.free_mem;
        *success = 1;
      }
      pthread_mutex_unlock(&gpu_device.mu);
    }
    break;
  case NVOS32_FUNCTION_INFO:
    pthread_mutex_lock(&gpu_device.mu);
    if (gpu_device.free_mem < align_size) {
      pApi->status = NV_ERR_NO_MEMORY;
      pApi->total = gpu_device.total_mem;
      pApi->free = gpu_device.free_mem;
    }
    pthread_mutex_unlock(&gpu_device.mu);
    break;
  default:
    break;
  }

finish:
  return ret;
}

int post_rm_vid_heap_control(uint32_t minor, size_t arg_size, void *arg) {
  int ret = 0;
  NVOS32_PARAMETERS *pApi = arg;
  device_mem_t *entry = NULL;

  if (unlikely(minor != NVIDIA_CTL_MINOR)) {
    ret = -EINVAL;
    goto finish;
  }

  if (unlikely(arg_size != sizeof(NVOS32_PARAMETERS))) {
    ret = -EINVAL;
    goto finish;
  }

  if (unlikely(pApi->function != NVOS32_FUNCTION_ALLOC_SIZE)) {
    goto finish;
  }

  if (unlikely(pApi->status != NV_OK)) {
    goto finish;
  }

  if (unlikely(pApi->data.AllocSize.alignment == RM_PAGE_SIZE_INVALID)) {
    goto finish;
  }

  pthread_mutex_lock(&gpu_device.mu);
  entry = malloc(sizeof(device_mem_t));
  if (unlikely(!entry)) {
    ret = -ENOMEM;
    goto finish;
  }

  entry->object = pApi->data.AllocSize.hMemory;
  entry->size = pApi->data.AllocSize.size;
  list_add(&entry->node, &gpu_device.mem_list);

  gpu_device.free_mem -= entry->size;
  gpu_device.alloc_mem += entry->size;

  pApi->total = gpu_device.total_mem;
  pApi->free = gpu_device.free_mem;

  pthread_mutex_unlock(&gpu_device.mu);
finish:
  return ret;
}

int post_rm_control_fb_get_info(void *params, size_t param_size) {
  int ret = 0;
  NV2080_CTRL_FB_GET_INFO_PARAMS *pParams = params;
  NV2080_CTRL_FB_INFO *info = NULL;
  int i = 0;

  pthread_mutex_lock(&gpu_device.mu);
  for (i = 0; i < pParams->fbInfoListSize; i++) {
    info = ((NV2080_CTRL_FB_INFO *)(pParams->fbInfoList) + i);
    switch (info->index) {
    case NV2080_CTRL_FB_INFO_INDEX_TOTAL_RAM_SIZE:
    case NV2080_CTRL_FB_INFO_INDEX_HEAP_SIZE:
      info->data = gpu_device.total_mem >> 10;
      break;
    case NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE:
      info->data = gpu_device.free_mem >> 10;
      break;
    default:
      break;
    }
  }
  pthread_mutex_unlock(&gpu_device.mu);

  return ret;
}

int post_rm_control_fb_get_info_v2(void *params, size_t param_size) {
  int ret = 0;
  NV2080_CTRL_FB_GET_INFO_V2_PARAMS *pParams = params;
  NV2080_CTRL_FB_INFO *info = NULL;
  int i = 0;

  pthread_mutex_lock(&gpu_device.mu);
  for (i = 0; i < pParams->fbInfoListSize; i++) {
    info = &pParams->fbInfoList[i];

    switch (info->index) {
    case NV2080_CTRL_FB_INFO_INDEX_TOTAL_RAM_SIZE:
    case NV2080_CTRL_FB_INFO_INDEX_HEAP_SIZE:
      info->data = gpu_device.total_mem >> 10;
      break;
    case NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE:
      info->data = gpu_device.free_mem >> 10;
      break;
    default:
      break;
    }
  }
  pthread_mutex_unlock(&gpu_device.mu);
  return ret;
}

int post_rm_control(uint32_t minor, size_t arg_size, void *arg) {
  int ret = 0;
  NVOS54_PARAMETERS *pApi = arg;

  if (unlikely(minor != NVIDIA_CTL_MINOR)) {
    ret = -EINVAL;
    goto finish;
  }

  if (unlikely(arg_size != sizeof(NVOS54_PARAMETERS))) {
    ret = -EINVAL;
    goto finish;
  }

  if (unlikely(pApi->status != NV_OK)) {
    goto finish;
  }

  switch (pApi->cmd) {
    /* 0x20801301 */
  case NV2080_CTRL_CMD_FB_GET_INFO:
    ret = post_rm_control_fb_get_info(pApi->params, pApi->paramsSize);
    break;
    /* 0x20801303 */
  case NV2080_CTRL_CMD_FB_GET_INFO_V2:
    ret = post_rm_control_fb_get_info_v2(pApi->params, pApi->paramsSize);
    break;
  default:
#ifndef NDEBUG
    LOGGER(VERBOSE, "rm cmd:0x%x", pApi->cmd);
#endif
    break;
  }

finish:
  return ret;
}

int post_rm_free(uint32_t minor, size_t arg_size, void *arg) {
  int ret = 0;
  NVOS00_PARAMETERS *pApi = arg;
  struct list_head *iter = NULL;
  device_mem_t *entry = NULL;

  if (unlikely(minor != NVIDIA_CTL_MINOR)) {
    ret = -EINVAL;
    goto finish;
  }

  if (unlikely(arg_size != sizeof(NVOS00_PARAMETERS))) {
    ret = -EINVAL;
    goto finish;
  }

  if (unlikely(pApi->status != NV_OK)) {
    goto finish;
  }

  if (unlikely(pApi->hObjectOld == pApi->hRoot)) {
    goto finish;
  }

  pthread_mutex_lock(&gpu_device.mu);

  list_for_each(iter, &gpu_device.mem_list) {
    entry = container_of(iter, device_mem_t, node);
    if (entry->object == pApi->hObjectOld) {
      break;
    }
  }

  if (likely(entry)) {
    list_del(&entry->node);

    gpu_device.free_mem += entry->size;
    gpu_device.alloc_mem -= entry->size;

    free(entry);
    entry = NULL;
  }

  pthread_mutex_unlock(&gpu_device.mu);
finish:
  return ret;
}

int post_ioctl(uint32_t major, uint32_t minor, uint32_t cmd, void *arg) {
  int ret = 0;
  int arg_cmd = 0;
  size_t arg_size = 0;

  if (unlikely(major != NVIDIA_DEVICE_MAJOR)) {
    goto finish;
  }

  arg_size = _IOC_SIZE(cmd);
  arg_cmd = _IOC_NR(cmd);

  switch (arg_cmd) {
    /* 0x4a */
  case NV_ESC_RM_VID_HEAP_CONTROL:
    ret = post_rm_vid_heap_control(minor, arg_size, arg);
    break;
    /* 0x2a */
  case NV_ESC_RM_CONTROL:
    ret = post_rm_control(minor, arg_size, arg);
    break;
    /* 0x29 */
  case NV_ESC_RM_FREE:
    ret = post_rm_free(minor, arg_size, arg);
    break;
  default:
#ifndef NDEBUG
    LOGGER(VERBOSE, "ioctl cmd:0x%x, size:0x%x", arg_cmd, arg_size);
#endif
    break;
  }

finish:
  return ret;
}

void _init_device_prop() {
  int ret = 0;
  token_attr_t *attr = NULL;
  size_t core_limit;

  ret = get_mem_limit(&gpu_device.minor, &gpu_device.total_mem);
  if (unlikely(ret)) {
    LOGGER(VERBOSE, "get mem limit failed");
    return;
  }
  gpu_device.mem_limited = 1;

  /* for memory limit */
  gpu_device.free_mem = gpu_device.total_mem;
  INIT_LIST_HEAD(&gpu_device.mem_list);

  ret = get_core_limit(NULL, &core_limit);
  if (likely(!ret)) {
    gpu_device.core_limited = 1;
  }

  if (unlikely(!gpu_device.core_limited)) {
    return;
  }

  /* for time limit */
  attr = create_shm_addr(gpu_device.minor, getpid(), sizeof(token_attr_t));
  if (unlikely(!attr)) {
    LOGGER(ERROR, "create shm addr failed");
    exit(-1);
    return;
  }

  ret = sem_init(&attr->ready, 1, 0);
  if (unlikely(ret < 0)) {
    LOGGER(ERROR, "attr not ready");
    exit(-1);
    return;
  }

  if (likely(!attr->inited)) {
    while ((ret = sem_wait(&attr->ready)) == -1 && errno == EINTR) {
      continue;
    }

    if (ret == -1) {
      LOGGER(ERROR, "sem_wait errno:%d, %s", errno, strerror(errno));
      exit(-1);
      return;
    }
  }

  ret = sem_init(&gpu_device.tokens, 0, attr->params.core_limit);
  if (unlikely(ret < 0)) {
    LOGGER(ERROR, "token init failed");
    exit(-1);
    return;
  }

  gpu_device.attr = attr;
  pthread_create(&gpu_device.tid, NULL, token_post, &gpu_device);
}

void init_device_prop() { pthread_once(&gpu_device.once, _init_device_prop); }
