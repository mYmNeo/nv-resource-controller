#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ctrl/ctrl2080/ctrl2080fb.h"
#include "extern.h"
#include "hook.h"
#include "nv_escape.h"
#include "nvos.h"
#include "nvstatus.h"
#include "nvtypes.h"
// this file must be the last include file
// clang-format off
#include "generated/g_allclasses.h"
// clang-format on

static device_prop_t gpu_device = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .major = NVIDIA_DEVICE_MAJOR,
    .minor = NVIDIA_CTL_MINOR,
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

    launch_times[loop % LAUNCH_SAMPLES] =
        atomic_load(&dev_params->launch_times);
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

int pre_vid_heap_alloc(uint32_t cmd, void *arg, int *success) {
  NVOS32_PARAMETERS *pApi = arg;
  size_t align_size = 0;
  int ret = 0;

  *success = 0;
  switch (pApi->function) {
    case NVOS32_FUNCTION_ALLOC_SIZE:
      if (likely(pApi->data.AllocSize.alignment != 0)) {
        align_size =
            (pApi->data.AllocSize.size + pApi->data.AllocSize.alignment - 1) &
            ~(pApi->data.AllocSize.alignment - 1);

        pthread_mutex_lock(&gpu_device.mu);
        if (gpu_device.fb_info->free_mem < align_size) {
          pApi->status = NV_ERR_NO_MEMORY;
          pApi->total = gpu_device.fb_info->total_mem;
          pApi->free = gpu_device.fb_info->free_mem;
          *success = 1;
        }
        pthread_mutex_unlock(&gpu_device.mu);
      }
      break;
    case NVOS32_FUNCTION_INFO:
      pthread_mutex_lock(&gpu_device.mu);
      if (gpu_device.fb_info->free_mem < align_size) {
        pApi->status = NV_ERR_NO_MEMORY;
        pApi->total = gpu_device.fb_info->total_mem;
        pApi->free = gpu_device.fb_info->free_mem;
      }
      pthread_mutex_unlock(&gpu_device.mu);
      break;
    default:
      break;
  }

  return ret;
}

int pre_rm_alloc(uint32_t cmd, void *arg, int *success) {
  NVOS21_PARAMETERS *pApi = arg;
  NV_MEMORY_ALLOCATION_PARAMS *params = NULL;
  size_t align_size = 0;
  int ret = 0;

  *success = 0;

#ifndef NDEBUG
  LOGGER(VERBOSE, "pre rm alloc: %p, parent: %p, root: %p, class: 0x%x",
         pApi->hObjectNew, pApi->hObjectParent, pApi->hRoot, pApi->hClass);

#endif
  switch (pApi->hClass) {
    case NV01_MEMORY_LOCAL_USER:
      params = pApi->pAllocParms;
      if (likely(params->alignment != 0)) {
        align_size =
            (params->size + params->alignment - 1) & ~(params->alignment - 1);

        pthread_mutex_lock(&gpu_device.mu);
        if (gpu_device.fb_info->free_mem < align_size) {
          pApi->status = NV_ERR_NO_MEMORY;
          *success = 1;
        }
        pthread_mutex_unlock(&gpu_device.mu);
      }
      break;
    default:
      break;
  }

  return ret;
}

int pre_ioctl(uint32_t major, uint32_t minor, uint32_t cmd, void *arg,
              int *success) {
  int arg_cmd = 0;
  int ret = 0;

  arg_cmd = _IOC_NR(cmd);
  switch (arg_cmd) {
    case NV_ESC_RM_VID_HEAP_CONTROL:
      ret = pre_vid_heap_alloc(cmd, arg, success);
      break;
    case NV_ESC_RM_ALLOC:
      ret = pre_rm_alloc(cmd, arg, success);
      break;
    default:
      break;
  }

  return ret;
}

int post_device_rm_alloc(NVOS21_PARAMETERS *pApi) {
  int ret = 0;
  int32_t device_id = -1;
  rm_mem_t *entry = NULL;

  device_id = *(int *)pApi->pAllocParms;
#ifndef NDEBUG
  LOGGER(VERBOSE, "allocate struct of device id: %d", device_id);
#endif

  pthread_mutex_lock(&gpu_device.mu);

  entry = malloc(sizeof(rm_mem_t));
  if (unlikely(!entry)) {
    ret = -ENOMEM;
    goto finish;
  }

  entry->object = pApi->hObjectNew;
  entry->device_id = device_id;
  list_add(&entry->node, &gpu_device.rm_mem_list);

finish:
  pthread_mutex_unlock(&gpu_device.mu);

  return ret;
}

int post_ctrl_rm_alloc(NVOS21_PARAMETERS *pApi) {
  int ret = 0;
  int32_t device_id = -1;
  rm_mem_t *entry = NULL;
  struct list_head *iter = NULL;

#ifndef NDEBUG
  LOGGER(VERBOSE, "allocate ctrl param, parent: %p", pApi->hObjectParent);
#endif
  pthread_mutex_lock(&gpu_device.mu);

  list_for_each(iter, &gpu_device.rm_mem_list) {
    entry = container_of(iter, rm_mem_t, node);
    if (entry->object == pApi->hObjectParent) {
      device_id = entry->device_id;
      break;
    }
  }

  if (device_id == -1) {
    goto finish;
  }

#ifndef NDEBUG
  LOGGER(VERBOSE, "allocate ctrl param on device %d", device_id);
#endif

  entry = malloc(sizeof(rm_mem_t));
  if (unlikely(!entry)) {
    ret = -ENOMEM;
    goto finish;
  }

  entry->object = pApi->hObjectNew;
  entry->device_id = device_id;
  list_add(&entry->node, &gpu_device.rm_mem_list);

finish:
  pthread_mutex_unlock(&gpu_device.mu);

  return ret;
}

int post_memory_rm_alloc(NVOS21_PARAMETERS *pApi) {
  int ret = 0;
  device_mem_t *entry = NULL;
  NV_MEMORY_ALLOCATION_PARAMS *params = NULL;

  if (unlikely(pApi->status != NV_OK)) {
    goto finish;
  }

  params = pApi->pAllocParms;

  if (unlikely(params->alignment == 0)) {
    goto finish;
  }

  pthread_mutex_lock(&gpu_device.mu);
  entry = malloc(sizeof(device_mem_t));
  if (unlikely(!entry)) {
    ret = -ENOMEM;
    goto finish;
  }

  entry->object = params->owner;
  entry->size = params->size;
  list_add(&entry->node, &gpu_device.heap_mem_list);

  gpu_device.fb_info->free_mem -= entry->size;
  gpu_device.alloc_mem += entry->size;
  pthread_mutex_unlock(&gpu_device.mu);

#ifndef NDEBUG
  LOGGER(VERBOSE, "alloc from rm: %p, size: %lu, use: %lu", params->owner,
         params->size, gpu_device.alloc_mem);
#endif

finish:
  return ret;
}

int post_rm_alloc(uint32_t minor, size_t arg_size, void *arg) {
  int ret = 0;
  NVOS21_PARAMETERS *pApi = arg;

#ifndef NDEBUG
  LOGGER(VERBOSE, "rm alloc: %p, parent: %p, root: %p, class: 0x%x",
         pApi->hObjectNew, pApi->hObjectParent, pApi->hRoot, pApi->hClass);
#endif

  switch (pApi->hClass) {
    case NV01_DEVICE_0:
      ret = post_device_rm_alloc(pApi);
      break;
    case NV20_SUBDEVICE_0:
      ret = post_ctrl_rm_alloc(pApi);
      break;
    case NV01_MEMORY_LOCAL_USER:
      ret = post_memory_rm_alloc(pApi);
      break;
    default:
      break;
  }

  return ret;
}

int post_vid_heap_alloc(NVOS32_PARAMETERS *pApi) {
  int ret = 0;
  device_mem_t *entry = NULL;

  if (unlikely(pApi->status != NV_OK)) {
    goto finish;
  }

  if (unlikely(pApi->data.AllocSize.alignment == 0)) {
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
  list_add(&entry->node, &gpu_device.heap_mem_list);

  gpu_device.fb_info->free_mem -= entry->size;
  gpu_device.alloc_mem += entry->size;

  pApi->total = gpu_device.fb_info->total_mem;
  pApi->free = gpu_device.fb_info->free_mem;

  pthread_mutex_unlock(&gpu_device.mu);

#ifndef NDEBUG
  LOGGER(VERBOSE, "alloc from heap: %p, size: %lu, use: %lu",
         pApi->data.AllocSize.hMemory, pApi->data.AllocSize.size,
         gpu_device.alloc_mem);
#endif

finish:
  return ret;
}

int post_rm_vid_heap_control(uint32_t minor, size_t arg_size, void *arg) {
  int ret = 0;
  NVOS32_PARAMETERS *pApi = arg;

  if (unlikely(minor != NVIDIA_CTL_MINOR)) {
    ret = -EINVAL;
    goto finish;
  }

  switch (pApi->function) {
    case NVOS32_FUNCTION_ALLOC_SIZE:
#ifndef NDEBUG
      LOGGER(VERBOSE, "vid heap alloc parent: %p, root: %p",
             pApi->hObjectParent, pApi->hRoot);
#endif
      ret = post_vid_heap_alloc(pApi);
      break;
    default:
      break;
  }

finish:
  return ret;
}

int __get_fb_info(int device_id, size_t *total_mem, size_t *free_mem) {
  int ret = 0;
  char path[PATH_MAX] = {0};
  share_data_t fb_share_data;
  struct stat buf;
  fb_info_t *fb_info = NULL;

  *total_mem = 0;
  *free_mem = -1;

  sprintf(path, HOOK_SHM_FB_MEM_PATH_PATTERN, device_id);
  fb_info = create_shm_addr(path, sizeof(fb_info_t), &fb_share_data);
  if (unlikely(!fb_info)) {
    goto finish;
  }

  if (fb_info->pid == 0) {
    goto finish;
  }

  *total_mem = fb_info->total_mem >> 10;

  /* if fb_info->pid existed, use its free_mem value */
  sprintf(path, "/proc/%d", fb_info->pid);
  if (stat(path, &buf) == 0) {
    *free_mem = fb_info->free_mem >> 10;
  }

finish:
  if (fb_share_data.addr) {
    munmap(fb_share_data.addr, sizeof(fb_info_t));
  }

  if (fb_share_data.fd) {
    close(fb_share_data.fd);
  }

  return ret;
}

int post_rm_control_fb_get_info(uint32_t handle, void *params,
                                size_t param_size) {
  int ret = 0;
  NV2080_CTRL_FB_GET_INFO_PARAMS *pParams = params;
  NV2080_CTRL_FB_INFO *info = NULL;
  int i = 0;
  struct list_head *iter = NULL;
  rm_mem_t *entry = NULL;
  int device_id = -1;
  size_t total_mem = 0, free_mem = 0;

  pthread_mutex_lock(&gpu_device.mu);
  list_for_each(iter, &gpu_device.rm_mem_list) {
    entry = container_of(iter, rm_mem_t, node);
    if (entry->object == handle) {
      device_id = entry->device_id;
      break;
    }
  }
  pthread_mutex_unlock(&gpu_device.mu);

  if (device_id == -1) {
    goto finish;
  }

  __get_fb_info(device_id, &total_mem, &free_mem);

  for (i = 0; i < pParams->fbInfoListSize; i++) {
    info = ((NV2080_CTRL_FB_INFO *)(pParams->fbInfoList) + i);
    switch (info->index) {
      case NV2080_CTRL_FB_INFO_INDEX_TOTAL_RAM_SIZE:
      case NV2080_CTRL_FB_INFO_INDEX_HEAP_SIZE:
        info->data = total_mem > 0 ? total_mem : info->data;
        break;
      case NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE:
        info->data = free_mem != -1 ? free_mem
                                    : (total_mem > 0 ? total_mem : info->data);
        break;
      default:
        break;
    }
  }

finish:
  return ret;
}

int post_rm_control_fb_get_info_v2(uint32_t handle, void *params,
                                   size_t param_size) {
  int ret = 0;
  NV2080_CTRL_FB_GET_INFO_V2_PARAMS *pParams = params;
  NV2080_CTRL_FB_INFO *info = NULL;
  int i = 0;
  struct list_head *iter = NULL;
  rm_mem_t *entry = NULL;
  int device_id = -1;
  size_t total_mem = 0, free_mem = 0;

  pthread_mutex_lock(&gpu_device.mu);
  list_for_each(iter, &gpu_device.rm_mem_list) {
    entry = container_of(iter, rm_mem_t, node);
    if (entry->object == handle) {
      device_id = entry->device_id;
      break;
    }
  }
  pthread_mutex_unlock(&gpu_device.mu);

  if (device_id == -1) {
    goto finish;
  }

  __get_fb_info(device_id, &total_mem, &free_mem);

  for (i = 0; i < pParams->fbInfoListSize; i++) {
    info = &pParams->fbInfoList[i];

    switch (info->index) {
      case NV2080_CTRL_FB_INFO_INDEX_TOTAL_RAM_SIZE:
      case NV2080_CTRL_FB_INFO_INDEX_HEAP_SIZE:
        info->data = total_mem > 0 ? total_mem : info->data;
        break;
      case NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE:
        info->data = free_mem != -1 ? free_mem
                                    : (total_mem > 0 ? total_mem : info->data);
        break;
      default:
        break;
    }
  }

finish:
  return ret;
}

int post_rm_control(uint32_t minor, size_t arg_size, void *arg) {
  int ret = 0;
  NVOS54_PARAMETERS *pApi = arg;

  if (unlikely(minor != NVIDIA_CTL_MINOR)) {
    ret = -EINVAL;
    goto finish;
  }

  if (unlikely(pApi->status != NV_OK)) {
    goto finish;
  }

  switch (pApi->cmd) {
      /* 0x20801301 */
    case NV2080_CTRL_CMD_FB_GET_INFO:
      ret = post_rm_control_fb_get_info(pApi->hObject, pApi->params,
                                        pApi->paramsSize);
      break;
      /* 0x20801303 */
    case NV2080_CTRL_CMD_FB_GET_INFO_V2:
      ret = post_rm_control_fb_get_info_v2(pApi->hObject, pApi->params,
                                           pApi->paramsSize);
      break;
    default:
      break;
  }

finish:
  return ret;
}

void free_device_page(uint32_t page) {
  struct list_head *iter = NULL;
  rm_mem_t *entry = NULL;

  list_for_each(iter, &gpu_device.rm_mem_list) {
    entry = container_of(iter, rm_mem_t, node);
    if (entry->object == page) {
      break;
    }
  }

  if (likely(entry)) {
#ifndef NDEBUG
    LOGGER(VERBOSE, "free device page: %d", entry->device_id);
#endif

    list_del(&entry->node);

    free(entry);
    entry = NULL;
  }
}

void free_heap_page(uint32_t page) {
  struct list_head *iter = NULL;
  device_mem_t *entry = NULL;

  list_for_each(iter, &gpu_device.heap_mem_list) {
    entry = container_of(iter, device_mem_t, node);
    if (entry->object == page) {
      break;
    }
  }

  if (likely(entry)) {
    gpu_device.fb_info->free_mem += entry->size;
    gpu_device.alloc_mem -= entry->size;

#ifndef NDEBUG
    LOGGER(VERBOSE, "free heap page: 0x%x, size: %lu, use: %lu", entry->object,
           entry->size, gpu_device.alloc_mem);
#endif

    list_del(&entry->node);

    free(entry);
    entry = NULL;
  }
}

int post_rm_free(uint32_t minor, size_t arg_size, void *arg) {
  int ret = 0;
  NVOS00_PARAMETERS *pApi = arg;

  if (unlikely(minor != NVIDIA_CTL_MINOR)) {
    ret = -EINVAL;
    goto finish;
  }

  if (unlikely(pApi->status != NV_OK)) {
    goto finish;
  }

#ifndef NDEBUG
  LOGGER(VERBOSE, "rm free %p, parent: %p, root: %p", pApi->hObjectOld,
         pApi->hObjectParent, pApi->hRoot);
#endif

  if (unlikely(pApi->hObjectOld == pApi->hRoot)) {
    goto finish;
  }

  pthread_mutex_lock(&gpu_device.mu);

  if (pApi->hRoot == pApi->hObjectParent) {
    free_device_page(pApi->hObjectOld);
  } else {
    free_heap_page(pApi->hObjectOld);
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
    case NV_ESC_RM_ALLOC:
      ret = post_rm_alloc(minor, arg_size, arg);
      break;
    default:
      break;
  }

finish:
  return ret;
}

void _init_device_prop() {
  int ret = 0;
  token_attr_t *attr = NULL;
  fb_info_t *fb_info = NULL;
  size_t core_limit;
  size_t total_mem;
  pid_t pid = 0;
  char cgroup_id[PATH_MAX] = {0};
  char path[PATH_MAX] = {0};
  struct stat buf;
  int need_init = 0;
  share_data_t fb_share_data, attr_share_data;

  ret = get_mem_limit(&gpu_device.minor, &total_mem);
  if (unlikely(ret)) {
    LOGGER(VERBOSE, "get mem limit failed");
    return;
  }
  gpu_device.mem_limited = 1;

  /* for memory limit */
  sprintf(path, HOOK_SHM_FB_MEM_PATH_PATTERN, gpu_device.minor);
  fb_info = create_shm_addr(path, sizeof(fb_info_t), &fb_share_data);
  if (unlikely(!fb_info)) {
    LOGGER(ERROR, "create fb shm addr failed");
    exit(-1);
    return;
  }
  gpu_device.fb_info = fb_info;
  pid = getpid();

  need_init = fb_info->pid == 0 ? 1 : 0;
  if (!need_init) {
    /* if fb_info->pid not existed, initialize fb info */
    sprintf(path, "/proc/%d", fb_info->pid);
    ret = stat(path, &buf);
    if (ret < 0) {
      need_init = errno == ENOENT ? 1 : 0;
    }
  }

  if (need_init) {
    LOGGER(VERBOSE, "init fb info for %lu, device_id: %d, total: %x", pid,
           gpu_device.minor, total_mem);

    gpu_device.fb_info->total_mem = total_mem;
    gpu_device.fb_info->free_mem = total_mem;
    gpu_device.fb_info->pid = pid;
  }

  INIT_LIST_HEAD(&gpu_device.rm_mem_list);
  INIT_LIST_HEAD(&gpu_device.heap_mem_list);

  ret = get_core_limit(NULL, &core_limit);
  if (likely(!ret)) {
    gpu_device.core_limited = 1;
  }

  if (unlikely(!gpu_device.core_limited)) {
    return;
  }

  /* for time limit */
  ret = get_cgroup_id(pid, cgroup_id, sizeof(cgroup_id));
  if (unlikely(ret < 0)) {
    LOGGER(ERROR, "get cgroup id failed");
    exit(-1);
    return;
  }

  sprintf(path, HOOK_SHM_PATH_PATTERN, gpu_device.minor, cgroup_id);
  attr = create_shm_addr(path, sizeof(token_attr_t), &attr_share_data);
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
