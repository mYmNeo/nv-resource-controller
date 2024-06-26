#include <dlfcn.h>
#include <link.h>

#include "hook.h"

static dlfcn_t __dlfcn_data = {
    .once = PTHREAD_ONCE_INIT,
    .dlopen = NULL,
    .dlsym = NULL,
    .dlclose = NULL,
    .dladdr = NULL,
    .ioctl = NULL,
};

extern void init(void);
extern entry_t *get_hook_funcs_data();
extern int get_hook_size();
extern entry_t *find_entry(entry_t *list, int size, const char *symbol);
extern int pre_ioctl(uint32_t major, uint32_t minor, uint32_t cmd, void *args,
                     int *success);
extern int post_ioctl(uint32_t major, uint32_t minor, uint32_t cmd, void *args);
extern int get_device_number(int fd, uint32_t *major, uint32_t *minor);
extern device_prop_t *get_device_prop(void);

dlfcn_t *get_dlfcn() { return &__dlfcn_data; }

static void *get_next_symbol(void *addr, const char *symbol) {
  void *handle = NULL;
  struct link_map *map = NULL, *self = NULL, *temp_map = NULL;
  ElfW(Addr) caller = (ElfW(Addr))addr;
  const char *self_path = NULL;
  Dl_info dl_info;
  int index = 0;

  BUG_ON(!__dlfcn_data.dlopen);
  BUG_ON(!__dlfcn_data.dlsym);
  BUG_ON(!__dlfcn_data.dlclose);
  BUG_ON(!__dlfcn_data.dladdr);

  if (__dlfcn_data.dladdr(get_next_symbol, &dl_info)) {
    LOGGER(DETAIL, "current shared object is located at: %s",
           dl_info.dli_fname);
    self_path = dl_info.dli_fname;
  } else {
    LOGGER(DETAIL, "This shared object is not found, use name: %s",
           LIBRARY_NAME);
    self_path = LIBRARY_NAME;
  }

  self = __dlfcn_data.dlopen(self_path, RTLD_NOLOAD | RTLD_LAZY);
  if (unlikely(!self)) {
    LOGGER(FATAL, "can't find %s", self_path);
    return NULL;
  }

#ifndef NDEBUG
  LOGGER(VERBOSE, "addr: %p, find next %s", addr, symbol);
#endif
  for (map = self; map; map = map->l_next) {
#ifndef NDEBUG
    LOGGER(VERBOSE, "find next %s(%p) from %s, l_addr: %p", symbol, map,
           map->l_name, (void *)map->l_addr);
#endif
    if (unlikely(caller < map->l_addr)) {
      continue;
    }

    // if caller > map->l_addr for the first time, we are at linkmap which
    // calls dlsym. so skip it.
    if (index < 1) {
      index++;
      continue;
    }

    // we should do temprary dlopen to initialize link_map properly
    temp_map = __dlfcn_data.dlopen(map->l_name, RTLD_NOLOAD | RTLD_LAZY);
    if (unlikely(!temp_map)) {
      LOGGER(FATAL, "can't find %s", map->l_name);
      return NULL;
    }

    handle = __dlfcn_data.dlsym(temp_map, symbol);
    __dlfcn_data.dlclose(temp_map);

    if (likely(handle)) {
#ifndef NDEBUG
      LOGGER(VERBOSE, "found next %s at %p", symbol, handle);
#endif
      goto done;
    }
  }

done:
  if (likely(self)) {
    __dlfcn_data.dlclose(self);
  }
  return handle;
}

EXPORT_API void *dlsym(void *handle, const char *symbol) {
  entry_t *e = NULL;
  void *entrypoint = NULL;

  if (unlikely(!__dlfcn_data.dlsym)) {
    pthread_once(&__dlfcn_data.once, init);
  }
  BUG_ON(!__dlfcn_data.dlsym);

  if (unlikely(handle == RTLD_NEXT)) {
    entrypoint = get_next_symbol(RETURN_ADDR(0), symbol);
    goto done;
  }

  entrypoint = __dlfcn_data.dlsym(handle, symbol);
  e = find_entry(get_hook_funcs_data(), get_hook_size(), symbol);
  if (likely(e && entrypoint)) {
    e->real_pfn = entrypoint;
    if (likely(e->hook_pfn)) {
#ifndef NDEBUG
      LOGGER(VERBOSE, "replace %s", symbol);
#endif
      entrypoint = e->hook_pfn;
    }
    goto done;
  }

done:
  return entrypoint;
}

EXPORT_API int ioctl(int fd, uint64_t cmd, void *args) {
  uint32_t major = 0, minor = 0;
  int ret = 0;
  int success = 0;
  device_prop_t *dev = get_device_prop();

  if (unlikely(!__dlfcn_data.ioctl)) {
    pthread_once(&__dlfcn_data.once, init);
  }
  BUG_ON(!__dlfcn_data.ioctl);

  if (likely(dev->mem_limited)) {
    ret = get_device_number(fd, &major, &minor);
    if (unlikely(ret)) {
      goto redirect;
    }

    if (unlikely(major != NVIDIA_DEVICE_MAJOR)) {
      goto redirect;
    }

    ret = pre_ioctl(major, minor, cmd, args, &success);
    if (unlikely(ret || success)) {
      goto finish;
    }
  }

redirect:
  ret = __dlfcn_data.ioctl(fd, cmd, args);
  if (likely(dev->mem_limited && !ret)) {
    ret = post_ioctl(major, minor, cmd, args);
  }

finish:
  return ret;
}
