#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "hook.h"

#define DEFAULT_WAIT_DURATION_MILLSEC 100
#define MODTIMES_PER_SEC (1000 / DEFAULT_WAIT_DURATION_MILLSEC)
#define MIN_SAMPLE_UTIL 3

typedef struct nvmlProcessUtilizationSample_st {
  unsigned int pid;
  unsigned long long timeStamp;
  unsigned int smUtil;
  unsigned int memUtil;
  unsigned int encUtil;
  unsigned int decUtil;
} nvmlProcessUtilizationSample_t;

typedef enum nvmlClockType_enum {
  NVML_CLOCK_GRAPHICS = 0,  //!< Graphics clock domain
  NVML_CLOCK_SM = 1,        //!< SM clock domain
  NVML_CLOCK_MEM = 2,       //!< Memory clock domain
  NVML_CLOCK_VIDEO = 3,     //!< Video encoder/decoder clock domain

  // Keep this last
  NVML_CLOCK_COUNT  //!< Count of clock types
} nvmlClockType_t;

typedef enum nvmlClockId_enum {
  NVML_CLOCK_ID_CURRENT = 0,             //!< Current actual clock value
  NVML_CLOCK_ID_APP_CLOCK_TARGET = 1,    //!< Target application clock
  NVML_CLOCK_ID_APP_CLOCK_DEFAULT = 2,   //!< Default application clock target
  NVML_CLOCK_ID_CUSTOMER_BOOST_MAX = 3,  //!< OEM-defined maximum clock rate

  // Keep this last
  NVML_CLOCK_ID_COUNT  //!< Count of Clock Ids.
} nvmlClockId_t;

typedef struct {
  int (*nvmlInit)(void);
  int (*nvmlDeviceGetHandleByIndex)(unsigned int idx, void *dev);
  int (*nvmlShutdown)(void);
  int (*nvmlDeviceGetProcessUtilization)(void *dev, void *utils,
                                         unsigned int *count,
                                         uint64_t lastSeen);
  int (*nvmlDeviceGetClock)(void *dev, int clockType, int clockId,
                            unsigned int *clockMHz);
} nvml_lib_t;

extern void *get_shm_addr(uint32_t minor, const char *cgroup_id,
                          size_t data_size);
extern int wait_duration(struct timespec *interval);
extern int get_cgroup_id(pid_t pid, char *short_id, size_t id_len);

int init_handle(nvml_lib_t *hdr) {
  void *_hdr = NULL;
  void *_sym = NULL;
  int ret = -1;

  _hdr = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
  if (unlikely(!_hdr)) {
    LOGGER(ERROR, "can't open ml lib");
    goto done;
  }

  if ((_sym = dlsym(_hdr, "nvmlInit")) || (_sym = dlsym(_hdr, "nvmlInit_v2"))) {
    hdr->nvmlInit = _sym;
  }

  if (unlikely(!hdr->nvmlInit)) {
    LOGGER(ERROR, "can't find nvmlInit");
    goto done;
  }

  if ((_sym = dlsym(_hdr, "nvmlDeviceGetHandleByIndex")) ||
      (_sym = dlsym(_hdr, "nvmlDeviceGetHandleByIndex_v2"))) {
    hdr->nvmlDeviceGetHandleByIndex = _sym;
  }

  if (unlikely(!hdr->nvmlDeviceGetHandleByIndex)) {
    LOGGER(ERROR, "can't find nvmlDeviceGetHandleByIndex");
    goto done;
  }

  _sym = dlsym(_hdr, "nvmlShutdown");
  if (unlikely(!_sym)) {
    LOGGER(ERROR, "can't find nvmlShutdown");
    goto done;
  }
  hdr->nvmlShutdown = _sym;

  _sym = dlsym(_hdr, "nvmlDeviceGetProcessUtilization");
  if (unlikely(!_sym)) {
    LOGGER(ERROR, "can't find nvmlDeviceGetProcessUtilization");
    goto done;
  }
  hdr->nvmlDeviceGetProcessUtilization = _sym;

  _sym = dlsym(_hdr, "nvmlDeviceGetClock");
  if (unlikely(!_sym)) {
    LOGGER(ERROR, "can't find nvmlDeviceGetClock");
    goto done;
  }
  hdr->nvmlDeviceGetClock = _sym;

  ret = 0;
done:
  return ret;
}

void init_attr(token_attr_t *attr, int limit) {
  token_param_t *params = &attr->params;
  int new_cycle = 0;

  if (likely(!attr->inited)) {
    params->core_limit = limit;
    params->mod_times = 0;
    params->add_per_cycle = 1;
    params->avg_launchs[0] = 0;
    params->avg_launchs[1] = 0;
    params->launch_idx = 0;

    attr->wait_time.tv_sec = 0;
    attr->wait_time.tv_nsec = DEFAULT_WAIT_DURATION_MILLSEC * 1000UL * 1000UL;

    attr->inited = 1;
    sem_post(&attr->ready);
  }

  if (attr->params.core_limit != limit) {
    params->core_limit = limit;
    params->mod_times = 0;
    new_cycle = (float)params->add_per_cycle * (float)attr->params.core_limit /
                (float)limit;
    params->add_per_cycle = new_cycle > 1 ? new_cycle : 1;
  }

  LOGGER(VERBOSE, "core_limit:%d, per_cycle:%d", params->core_limit,
         params->add_per_cycle);
  attr->changed = 1;
}

void delta_change(token_attr_t *attr, int util, int limit) {
  token_param_t *params = &attr->params;
  float util_per_kernel = 0;
  int old_cycle = 0, delta_cycle = 0, new_cycle = 0, cycle_signed = 0;
  int err = 0;
  int changed = 0;
  int sample_ticks = MODTIMES_PER_SEC * 5;
  int idx = 0, last_avg_launchs = 0, cur_avg_launchs = 0;
  float avg_delta_ratio = 0, cycle_delta_ratio = 0;

  if (atomic_load(&attr->changed)) {
    return;
  }

  idx = atomic_load(&params->launch_idx);
  cur_avg_launchs = params->avg_launchs[idx % 2];
  last_avg_launchs = params->avg_launchs[(idx + 1) % 2];

  util_per_kernel = (float)util / (float)cur_avg_launchs;
  old_cycle = atomic_load(&params->add_per_cycle);
  err = limit - util;

  params->mod_times++;
  if (params->mod_times <= sample_ticks) {
    if (old_cycle <= MIN_SAMPLE_UTIL) {
      atomic_store(&params->add_per_cycle, old_cycle + 1);
      changed = 1;
    }
    goto done;
  }

  if (params->mod_times % sample_ticks == 0) {
    cycle_signed = err > 0 ? 1 : -1;
    /* add 25% percent of delta */
    delta_cycle = (abs(err) / util_per_kernel) / 4;
    delta_cycle = delta_cycle < 0 ? 1 : delta_cycle;

    new_cycle = old_cycle + delta_cycle * cycle_signed;

    avg_delta_ratio =
        abs(cur_avg_launchs - last_avg_launchs) / (float)(last_avg_launchs + 1);
    cycle_delta_ratio = abs(new_cycle - old_cycle) / (float)old_cycle;
    /* overload */
    if (cur_avg_launchs != last_avg_launchs &&
        cycle_delta_ratio > avg_delta_ratio) {
      new_cycle =
          old_cycle + (float)last_avg_launchs * avg_delta_ratio * cycle_signed;
    }
    new_cycle = new_cycle < 1 ? 1 : new_cycle;

    atomic_store(&params->add_per_cycle, new_cycle);
    changed = 1;

    LOGGER(VERBOSE,
           "mod_time:%d, err:%d, util:%d, delta_cycle:%d, add_per_cycle:%d, "
           "avg_launchs:%d:%d",
           params->mod_times, err, util, delta_cycle, new_cycle,
           cur_avg_launchs, last_avg_launchs);
    goto done;
  }

done:
  if (changed) {
    atomic_store(&attr->changed, 1);
  }

  return;
}

int get_gpu_util(nvml_lib_t *hdr, void *dev, const char *cgroup_id,
                 nvmlProcessUtilizationSample_t *samples, int sample_size,
                 struct timespec *last_time) {
  char target_cgroup_id[MAX_CGROUP_ID_LEN] = {0};
  nvmlProcessUtilizationSample_t *target_sample = NULL;
  uint64_t last_seen = 0;
  int i = 0;
  int ret = 0;

  last_seen = last_time->tv_sec * 1000UL * 1000UL + last_time->tv_nsec / 1000UL;
  ret = hdr->nvmlDeviceGetProcessUtilization(
      dev, samples, (unsigned int *)&sample_size, last_seen);
  if (unlikely(ret)) {
    /* not NOT_FOUND error */
    if (ret == 6) {
      return -1;
    }

    LOGGER(ERROR, "can't get samples %d", ret);
    return -1;
  }

  for (i = 0; i < sample_size; i++) {
    if (samples[i].timeStamp < last_seen) {
      continue;
    }

    ret = get_cgroup_id(samples[i].pid, target_cgroup_id,
                        sizeof(target_cgroup_id));
    if (!ret && !strcmp(target_cgroup_id, cgroup_id)) {
      target_sample = &samples[i];
      break;
    }
  }

  if (unlikely(!target_sample)) {
    LOGGER(ERROR, "can't find target pid");
    return -1;
  }

  return target_sample->smUtil;
}

void watch_dog(nvml_lib_t *hdr, uint32_t minor, const char *cgroup_id,
               int limit) {
  void *dev = NULL;
  int ret = 0;
  struct timespec wait_time = {
      .tv_sec = 0,
      .tv_nsec = DEFAULT_WAIT_DURATION_MILLSEC * 1000UL * 1000UL,
  };
  nvmlProcessUtilizationSample_t *samples = NULL;
  int sample_size = 200;
  int util = 0;
  struct timespec last_time = {0, 0};
  token_attr_t *attr = NULL;
  uint32_t cur_clock = 0, max_clock = 0;

  ret = hdr->nvmlDeviceGetHandleByIndex(minor, &dev);
  if (unlikely(ret)) {
    LOGGER(ERROR, "can't get dev handle");
    return;
  }

  attr = get_shm_addr(minor, cgroup_id, sizeof(token_attr_t));
  if (unlikely(!attr)) {
    LOGGER(ERROR, "can't find shm addr");
    return;
  }

  init_attr(attr, limit);
  samples = malloc(sizeof(nvmlProcessUtilizationSample_t) * sample_size);
  if (unlikely(!samples)) {
    LOGGER(ERROR, "can't alloc samples");
    return;
  }

  max_clock = 0;
  ret = hdr->nvmlDeviceGetClock(dev, NVML_CLOCK_GRAPHICS,
                                NVML_CLOCK_ID_CUSTOMER_BOOST_MAX, &max_clock);
  if (unlikely(ret)) {
    LOGGER(ERROR, "can't get current clock");
    return;
  }

  cur_clock = 0;
  ret = hdr->nvmlDeviceGetClock(dev, NVML_CLOCK_GRAPHICS, NVML_CLOCK_ID_CURRENT,
                                &cur_clock);
  if (unlikely(ret)) {
    LOGGER(ERROR, "can't get current clock");
  }

  LOGGER(VERBOSE, "clock: %u:%u", cur_clock, max_clock);

  while (1) {
    clock_gettime(CLOCK_MONOTONIC, &last_time);
    wait_duration(&wait_time);
    util = get_gpu_util(hdr, dev, cgroup_id, samples, sample_size, &last_time);
    if (unlikely(util < 0)) {
      continue;
    }

    delta_change(attr, util, limit);
  }

  if (samples) {
    free(samples);
    samples = NULL;
  }
}

int main(int argc, char *argv[]) {
  uint32_t minor = 0;
  char cgroup_id[MAX_CGROUP_ID_LEN] = {0};
  int core_limit = 0;
  nvml_lib_t handler;
  int ret = 0;

  /* $0 <minor> <cgroup id> <core limit> */
  if (argc != 4) {
    printf("usage: %s <minor> <cgroup id> <core limit>\n", argv[0]);
    exit(-1);
  }

  minor = strtol(argv[1], NULL, 10);
  strncpy(cgroup_id, argv[2], sizeof(cgroup_id));
  core_limit = strtol(argv[3], NULL, 10);

  LOGGER(INFO, "monitor minor:%d, cgroup_id:%s, core_limit:%d", minor,
         cgroup_id, core_limit);

  ret = init_handle(&handler);
  if (unlikely(ret < 0)) {
    exit(-1);
  }

  ret = handler.nvmlInit();
  if (unlikely(ret)) {
    LOGGER(ERROR, "can't init nvml");
    exit(-1);
  }

  watch_dog(&handler, minor, cgroup_id, core_limit);

  handler.nvmlShutdown();

  return 0;
}
