#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <stack_transform.h>
#include <sys/prctl.h>
#include "migrate.h"

#ifdef _TIME_REWRITE
#include <time.h>
#endif

/* Pierre: due to the-fdata-sections flags, in combination with the way the 
 * library is compiled for each architecture, global variables here end up 
 * placed into sections with different names, making them difficult to link 
 * back together from the alignment tool  perspective without ugly hacks. 
 * So, the solution here is to force these global variables to be in a custom 
 * section. By construction it will have the same name on both architecture. 
 * However for soem reason this doesn't work if the global variable is static so 
 * I had to remove the static keyword for the concerned variables. They are:
 * - cpus_x86
 * - migrate_callback
 * - migrate_callback_data
 * - popcorn_vdso
 */
//int cpus_x86 __attribute__ ((section (".bss.cpus_x86"))) = 0;

/* Architecture-specific assembly for migrating between architectures. */
#ifdef __aarch64__
# include <arch/aarch64/migrate.h>
#elif defined(__powerpc64__)
# include <arch/powerpc64/migrate.h>
#else
# include <arch/x86_64/migrate.h>
#endif

#ifdef _ENV_SELECT_MIGRATE

/*
 * The user can specify at which point a thread should migrate by specifying
 * program counter address ranges via environment variables.
 */

/* Environment variables specifying at which function to migrate */
static const char *env_start_aarch64 = "AARCH64_MIGRATE_START";
static const char *env_end_aarch64 = "AARCH64_MIGRATE_END";
static const char *env_start_powerpc64 = "POWERPC64_MIGRATE_START";
static const char *env_end_powerpc64 = "POWERPC64_MIGRATE_END";
static const char *env_start_x86_64 = "X86_64_MIGRATE_START";
static const char *env_end_x86_64 = "X86_64_MIGRATE_END";

/* Per-arch functions (specified via address range) at which to migrate */
static void *start_aarch64 = NULL;
static void *end_aarch64 = NULL;
static void *start_powerpc64 = NULL;
static void *end_powerpc64 = NULL;
static void *start_x86_64 = NULL;
static void *end_x86_64 = NULL;

/* TLS keys indicating if the thread has previously migrated */
static pthread_key_t num_migrated_aarch64 = 0;
static pthread_key_t num_migrated_powerpc64 = 0;
static pthread_key_t num_migrated_x86_64 = 0;

/* Read environment variables to setup migration points. */
static void __attribute__((constructor))
__init_migrate_testing(void)
{
  const char *start;
  const char *end;

#ifdef __aarch64__
  start = getenv(env_start_aarch64);
  end = getenv(env_end_aarch64);
  if(start && end)
  {
    start_aarch64 = (void *)strtoll(start, NULL, 16);
    end_aarch64 = (void *)strtoll(end, NULL, 16);
    if(start_aarch64 && end_aarch64)
      pthread_key_create(&num_migrated_aarch64, NULL);
  }
#elif defined(__powerpc64__)
  start = getenv(env_start_powerpc64);
  end = getenv(env_end_powerpc64);
  if(start && end)
  {
    start_powerpc64 = (void *)strtoll(start, NULL, 16);
    end_powerpc64 = (void *)strtoll(end, NULL, 16);
    if(start_powerpc64 && end_powerpc64)
      pthread_key_create(&num_migrated_powerpc64, NULL);
  }
#else
  start = getenv(env_start_x86_64);
  end = getenv(env_end_x86_64);
  if(start && end)
  {
    start_x86_64 = (void *)strtoll(start, NULL, 16);
    end_x86_64 = (void *)strtoll(end, NULL, 16);
    if(start_x86_64 && end_x86_64)
      pthread_key_create(&num_migrated_x86_64, NULL);
  }
#endif
}

/*
 * Check environment variables to see if this call site is the function at
 * which we should migrate.
 */
static inline int do_migrate(void *addr)
{
  int retval = -1;
#ifdef __aarch64__
  if(start_aarch64 && !pthread_getspecific(num_migrated_aarch64)) {
    if(start_aarch64 <= addr && addr < end_aarch64) {
      pthread_setspecific(num_migrated_aarch64, (void *)1);
      retval = 0;
    }
  }
#elif defined(__powerpc64__)
  if(start_powerpc64 && !pthread_getspecific(num_migrated_powerpc64)) {
    if(start_powerpc64 <= addr && addr < end_powerpc64) {
      pthread_setspecific(num_migrated_powerpc64, (void *)1);
      retval = 1;
    }
  }
#else
  if(start_x86_64 && !pthread_getspecific(num_migrated_x86_64)) {
    if(start_x86_64 <= addr && addr < end_x86_64) {
      pthread_setspecific(num_migrated_x86_64, (void *)1);
      retval = 2;
    }
  }
#endif
  return retval;
}

#else /* _ENV_SELECT_MIGRATE */

static inline int do_migrate(void *fn)
{
  int ret = syscall(SYSCALL_MIGRATION_PROPOSED);
  return ret >= 0 ? ret : -1;
}

#endif /* _ENV_SELECT_MIGRATE */

/* Flag set by signal handler indicating thread should migrate. */
// TODO make this TLS
int __migrate_flag = -1;

/* Data needed post-migration. */
struct shim_data {
  void (*callback)(void *);
  void *callback_data;
  void *regset;
};

#define MAX_POPCORN_NODES 32
int archs[MAX_POPCORN_NODES] __attribute__ ((section (".data.archs"))) = { 0 };

static void __attribute__((constructor)) __init_nodes_info(void)
{
  int i;
  struct node_info {
    unsigned int status;
    int arch;
    int distance;
  } ni;

  for (i = 0; i < MAX_POPCORN_NODES; i++) {
    if (syscall(SYSCALL_GET_NODE_INFO, i, &ni) == 0
        && ni.status == 1) {
      archs[i] = ni.arch;
    } else {
      archs[i] = NUM_ARCHES;
    }
  }
}

#ifdef _DEBUG
/*
 * Flag indicating we should spin post-migration in order to wait until a
 * debugger can attach.
 */
static volatile int __hold = 1;
#endif

/* Check & invoke migration if requested. */
// Note: a pointer to data necessary to bootstrap execution after migration is
// saved by the pthread library.
static void inline __migrate_shim_internal(int nid, void (*callback)(void *),
                                           void *callback_data)
{
  struct shim_data data;
  struct shim_data *data_ptr = *pthread_migrate_args();

  if(data_ptr) // Post-migration
  {
#ifdef _DEBUG
    // Hold until we can attach post-migration
    while(__hold);
#endif

    if(data_ptr->callback) data_ptr->callback(data_ptr->callback_data);
    *pthread_migrate_args() = NULL;

    // Hack: the kernel can't set floating-point registers, so we have to
    // manually copy them over in userspace
    SET_FP_REGS;

    // Reset the migration flag
    __migrate_flag = -1;
  }
  else // Invoke migration
  {
    const int dst_arch = archs[nid];

    GET_LOCAL_REGSET;
    union {
       struct regset_aarch64 aarch;
       struct regset_powerpc64 powerpc;
       struct regset_x86_64 x86;
    } regs_dst;

    unsigned long sp = 0, bp = 0;
            
#ifdef _TIME_REWRITE
    struct timespec start, end;
    unsigned long start_ns, end_ns;

#endif
    data.callback = callback;
    data.callback_data = callback_data;
    data.regset = &regs_dst;
    *pthread_migrate_args() = &data;

    if(REWRITE_STACK)
    {
#ifdef _TIME_REWRITE
      clock_gettime(CLOCK_MONOTONIC, &end);
      start_ns = start.tv_sec * 1000000000 + start.tv_nsec;
      end_ns = end.tv_sec * 1000000000 + end.tv_nsec;
      printf("Stack transformation time: %ldns\n", end_ns - start_ns);
#endif

      if(dst_arch == X86_64) {
        regs_dst.x86.rip = __migrate_shim_internal;
        sp = (unsigned long)regs_dst.x86.rsp;
        bp = (unsigned long)regs_dst.x86.rbp;
      } else if (dst_arch == AARCH64) {
        regs_dst.aarch.pc = __migrate_shim_internal;
        sp = (unsigned long)regs_dst.aarch.sp;
        bp = (unsigned long)regs_dst.aarch.x[29];
      } else if (dst_arch == POWERPC64) {
        regs_dst.powerpc.pc = __migrate_shim_internal;
        sp = (unsigned long)regs_dst.powerpc.r[1];
        bp = (unsigned long)regs_dst.powerpc.r[31];
      } else {
        assert(0 && "Unsupported architecture!");
      }

      MIGRATE;
      assert(0 && "Couldn't migrate!");
    }
  }
}

/* Check if we should migrate, and invoke migration. */
void check_migrate(void (*callback)(void *), void *callback_data)
{
  int nid = do_migrate(__builtin_return_address(0));
  if (nid >= 0)
    __migrate_shim_internal(nid, callback, callback_data);
}

/* Externally-visible function to invoke migration. */
void migrate(int nid, void (*callback)(void *), void *callback_data)
{
  __migrate_shim_internal(nid, callback, callback_data);
}

/* Callback function & data for migration points inserted via compiler. */
void (*migrate_callback)(void *) __attribute__ ((section(".bss.migrate_callback"))) = NULL;
void *migrate_callback_data __attribute__ ((section(".bss.migrate_callback_data")))= NULL;

/* Register callback function for compiler-inserted migration points. */
void register_migrate_callback(void (*callback)(void*), void *callback_data)
{
  migrate_callback = callback;
  migrate_callback_data = callback_data;
}

/* Hook inserted by compiler at the beginning of a function. */
void __cyg_profile_func_enter(void *this_fn, void *call_site)
{
  int nid = do_migrate(this_fn);
  if (nid >= 0)
    __migrate_shim_internal(nid, migrate_callback, migrate_callback_data);
}

/* Hook inserted by compiler at the end of a function. */
void __cyg_profile_func_exit(void *this_fn, void *call_site)
{
  int nid = do_migrate(this_fn);
  if (nid >= 0)
    __migrate_shim_internal(nid, migrate_callback, migrate_callback_data);
}

