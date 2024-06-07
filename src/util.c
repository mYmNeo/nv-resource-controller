#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "hook.h"

entry_t *find_entry(entry_t *list, int size, const char *symbol) {
  int i = 0;

  for (i = 0; i < size; i++) {
    if (!strcmp(list[i].name, symbol)) {
      return &list[i];
    }
  }

  return NULL;
}

size_t iec_to_bytes(const char *iec_value) {
  char *endptr = NULL;
  double value = 0.0;

  value = strtod(iec_value, &endptr);
  switch (*endptr) {
    case 'K':
    case 'k':
      value *= 1024UL;
      break;
    case 'M':
    case 'm':
      value *= 1024UL * 1024UL;
      break;
    case 'G':
    case 'g':
      value *= 1024UL * 1024UL * 1024UL;
      break;
    case 'T':
    case 't':
      value *= 1024UL * 1024UL * 1024UL * 1024UL;
      break;
    default:
      break;
  }

  return (size_t)value;
}

char *get_env_from(const char *str) {
  char *pos = strchr(str, '=');
  if (!pos) {
    return NULL;
  }

  return pos + 1;
}

int get_device_number(int fd, uint32_t *major, uint32_t *minor) {
  int ret = 0;
  struct stat st;

  ret = fstat(fd, &st);
  if (unlikely(ret < 0)) {
    return ret;
  }

  *major = 0;
  *minor = 0;
  if (unlikely(st.st_mode & S_IFMT != S_IFCHR)) {
    return -1;
  }

  *major = major(st.st_rdev);
  *minor = minor(st.st_rdev);

  return 0;
}

int get_cgroup_id(pid_t pid, char *short_id, size_t id_len) {
  char path[PATH_MAX] = {0};
  FILE *fp = NULL;
  char *line = NULL;
  size_t len = 0, max_copy = 0;
  int ret = -1;
  int i = 0;

  sprintf(path, "/proc/%d/cgroup", pid);
  fp = fopen(path, "r");
  if (unlikely(!fp)) {
    goto done;
  }

  while (getline(&line, &len, fp) != -1) {
    /*
     * resolve cgroup id from line like:
     * 4:devices:/kubepods/besteffort/pod59760328-93c1-464e-a944-7f6801c299d6/5680af4f12fc9a331866222e5446b51a0bf358334418d089916996a672b27346
     */
    if (strstr(line, "devices")) {
      for (i = len - 1; i >= 0; i--) {
        if (line[i] == '/') {
          i++;
          break;
        }
      }

      if (i >= 0) {
        max_copy = MIN(len - i, id_len);
        strncpy(short_id, line + i, max_copy);
        short_id[max_copy - 1] = '\0';
        ret = 0;
        break;
      }
    }
  }

done:
  if (likely(fp)) {
    fclose(fp);
    fp = NULL;
  }

  return ret;
}

void *create_shm_addr(uint32_t minor, pid_t pid, size_t data_size) {
  char path[PATH_MAX] = {0};
  char cgroup_id[MAX_CGROUP_ID_LEN] = {0};
  int ret = 0;
  int fd = -1;
  int first = 1;
  void *addr = NULL;

  ret = get_cgroup_id(pid, cgroup_id, sizeof(cgroup_id));
  if (unlikely(ret < 0)) {
    LOGGER(ERROR, "get cgroup id failed");
    goto done;
  }

  sprintf(path, HOOK_SHM_PATH_PATTERN, minor, cgroup_id);
  fd = shm_open(path, O_EXCL | O_RDWR | O_CREAT, 0666);
  if (unlikely(fd < 0)) {
    if (errno != EEXIST) {
      goto done;
    }

    fd = shm_open(path, O_RDWR, 0666);
    if (unlikely(fd < 0)) {
      LOGGER(ERROR, "open shm %d", errno);
      goto done;
    }
    first = 0;
  }

  if (likely(first)) {
    ret = ftruncate(fd, data_size);
    if (unlikely(ret < 0)) {
      goto done;
    }
  }

  addr = mmap(NULL, data_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (unlikely(addr == MAP_FAILED)) {
    goto done;
  }

  if (likely(first)) {
    memset(addr, 0x0, data_size);
  }
done:
  return addr;
}

void *get_shm_addr(uint32_t minor, const char *cgroup_id, size_t data_size) {
  int fd = -1;
  char path[128] = {0};
  void *addr = NULL;

  sprintf(path, HOOK_SHM_PATH_PATTERN, minor, cgroup_id);
  fd = shm_open(path, O_RDWR, 0666);
  if (unlikely(fd < 0)) {
    goto done;
  }

  addr = mmap(NULL, data_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (unlikely(addr == MAP_FAILED)) {
    goto done;
  }

done:
  return addr;
}

int wait_duration(struct timespec *interval) {
  struct timespec req_time = {0};
  struct timespec remain = {0};
  int ret = 0;
  int err = 0;

  req_time.tv_nsec = interval->tv_nsec;
  ret = nanosleep(&req_time, &remain);
  if (ret) {
    err = errno;
    do {
      if (err != EINTR) {
        break;
      }
      req_time.tv_nsec = remain.tv_nsec;
      ret = nanosleep(&req_time, &remain);
    } while (ret);
  }

  return ret;
}
