#include "hook.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *CUDA_MEM_LIMIT = "CUDA_MEM_LIMIT";
static const char *CUDA_CORE_LIMIT = "CUDA_CORE_LIMIT";

extern size_t iec_to_bytes(const char *iec_value);
extern char *get_env_from(const char *str);

static inline int get_limit(const char *name, uint32_t *minor, char *data) {
  char *str = NULL;
  int ret = -1;
  int n = 0;
  uint32_t _minor = 0;

  str = getenv(name);
  if (unlikely(!str)) {
    LOGGER(WARN, "env %s not found", name);
    goto done;
  }

  n = sscanf(str, "%u=%15s", &_minor, data);
  if (unlikely(n != 2 || !strlen(data))) {
    LOGGER(WARN, "empty minor or size");
    goto done;
  }

  ret = 0;
  if (likely(minor)) {
    *minor = _minor;
  }
done:
  return ret;
}

int get_mem_limit(uint32_t *minor, size_t *limit) {
  int ret = -1;
  char tmp[16] = {0};

  ret = get_limit(CUDA_MEM_LIMIT, minor, tmp);
  if (unlikely(ret)) {
    return ret;
  }

  *limit = iec_to_bytes(tmp);
  return 0;
}

int get_core_limit(uint32_t *minor, size_t *limit) {
  int ret = -1;
  char tmp[16] = {0};

  ret = get_limit(CUDA_CORE_LIMIT, minor, tmp);
  if (unlikely(ret)) {
    return ret;
  }

  *limit = atoi(tmp);
  return 0;
}
