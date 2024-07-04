#ifndef EXTERN_H
#define EXTERN_H

#include "hook.h"

extern void *create_shm_addr(const char *shm_path_pattern, size_t data_size,
                             share_data_t *share_data);
extern int wait_duration(struct timespec *interval);
extern int get_cgroup_id(pid_t pid, char *short_id, size_t id_len);

extern int get_mem_limit(uint32_t *minor, size_t *limit);
extern int get_core_limit(uint32_t *minor, size_t *limit);

#endif
