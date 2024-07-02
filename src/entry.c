#include <dlfcn.h>
#include <link.h>
#include <string.h>

#include "hook.h"

static int retrieve_symbols(struct dl_phdr_info *info, size_t size,
                            void *data) {
  /* ElfW is a macro that creates proper typenames for the used system
   * architecture (e.g. on a 32 bit system, ElfW(Dyn*) becomes "Elf32_Dyn*")
   */
  ElfW(Dyn *) dyn = NULL;
  ElfW(Sym *) sym = NULL;
  ElfW(Word *) hash = NULL;
  ElfW(Word) sym_index = 0;

  char *strtab = 0;
  char *sym_name = 0;
  void *sym_addr = NULL;
  ElfW(Word) sym_cnt = 0;
  size_t header_index = 0;
  const char *libname = basename(info->dlpi_name);
  dlfcn_t *dlfcn_data = data;

  if (unlikely(!strlen(libname) || !strcmp(libname, "linux-vdso.so.1"))) {
    return 0;
  }

#ifndef NDEBUG
  LOGGER(VERBOSE, "retrieve from %s base %p", info->dlpi_name,
         (void *)info->dlpi_addr);
#endif

  /* Iterate over all headers of the current shared lib
   * (first call is for the executable itself) */
  for (header_index = 0; header_index < info->dlpi_phnum; header_index++) {
    if (likely(info->dlpi_phdr[header_index].p_type != PT_DYNAMIC)) {
      continue;
    }
    /* Further processing is only needed if the dynamic section is reached
     */

    /* Get a pointer to the first entry of the dynamic section.
     * It's address is the shared lib's address + the virtual address */
    dyn =
        (ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[header_index].p_vaddr);

    /* Iterate over all entries of the dynamic section until the
     * end of the symbol table is reached. This is indicated by
     * an entry with d_tag == DT_NULL.
     *
     * Only the following entries need to be processed to find the
     * symbol names:
     *  - DT_HASH   -> second word of the hash is the number of symbols
     *  - DT_STRTAB -> pointer to the beginning of a string table that
     *                 contains the symbol names
     *  - DT_SYMTAB -> pointer to the beginning of the symbols table
     */
    while (likely(dyn->d_tag != DT_NULL)) {
      if (dyn->d_tag == DT_HASH) {
        /* Get a pointer to the hash */
        hash = (ElfW(Word *))dyn->d_un.d_ptr;

        /* The 2nd word is the number of symbols */
        sym_cnt = hash[1];

      } else if (dyn->d_tag == DT_STRTAB) {
        /* Get the pointer to the string table */
        strtab = (char *)dyn->d_un.d_ptr;
      } else if (dyn->d_tag == DT_SYMTAB) {
        /* Get the pointer to the first entry of the symbol table */
        sym = (ElfW(Sym *))dyn->d_un.d_ptr;

        /* Iterate over the symbol table */
        for (sym_index = 0; sym_index < sym_cnt; sym_index++) {
          /* get the name of the i-th symbol.
           * This is located at the address of st_name
           * relative to the beginning of the string table. */
          sym_name = &strtab[sym[sym_index].st_name];
          sym_addr = (void *)(info->dlpi_addr + sym[sym_index].st_value);

          /*
           * if sym_addr is equal to the address of the shared lib
           * dlpi_addr, that means it is a needed symbol
           */
          if (sym_addr == (void *)info->dlpi_addr) {
            continue;
          }

          if (likely(!strcmp(sym_name, "dlsym"))) {
            if (!dlfcn_data->dlsym) {
              dlfcn_data->dlsym = sym_addr;
            }
            continue;
          }

          if (likely(!strcmp(sym_name, "dlopen"))) {
            if (!dlfcn_data->dlopen) {
              dlfcn_data->dlopen = sym_addr;
            }
            continue;
          }

          if (likely(!strcmp(sym_name, "dlclose"))) {
            if (!dlfcn_data->dlclose) {
              dlfcn_data->dlclose = sym_addr;
            }
            continue;
          }

          if (likely(!strcmp(sym_name, "dladdr"))) {
            if (!dlfcn_data->dladdr) {
              dlfcn_data->dladdr = sym_addr;
            }
            continue;
          }

          if (likely(!strcmp(sym_name, "ioctl"))) {
            if (!dlfcn_data->ioctl) {
              dlfcn_data->ioctl = sym_addr;
            }
            continue;
          }
        }
      }

      /* move pointer to the next entry */
      dyn++;
    }
  }

  /* Returning something != 0 stops further iterations,
   * since only the first entry, which is the executable itself, is needed
   * 1 is returned after processing the first entry.
   *
   * If the symbols of all loaded dynamic libs shall be found,
   * the return value has to be changed to 0.
   */
  return 0;
}

extern void init_device_prop(void);
extern dlfcn_t *get_dlfcn();

__attribute__((constructor)) void init(void) {
  dl_iterate_phdr(retrieve_symbols, get_dlfcn());
  init_device_prop();
}
