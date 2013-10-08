/*
 * This example reserves a TLMM, allocates TLMM_NPAGES, and creates
 * NTHREADS.  Each thread maps all TLMM_NPAGES into the TLMM as
 * PROT_READ, loops, selecting a random page, and incrementing an int in
 * the random page.  If the page is mapped PROT_READ, the kernel raises
 * a SIGSEGV, which the thread handles by rempping the page
 * PROT_READ|PROT_WRITE.
 *
 * Quirks/shortcomings of the TLMM implementation:
 *   - Threads cannot share or copy TLMM regions across fork/clone.
 *   - Threads can map only the top 500 pages of a TLMM region.
 *   - Threads cannot free a PD.  The kernel will garbage collect them
 *     when a process exits.
 */

#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/mman.h>

#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>

#include "systlmm.h"


#define NTHREADS  16
#define VERBOSE    0

#define TLMM_SIZE  (1UL << 39)
#define TLMM_NPAGES  6897 
#define PD_NULL   -1

static struct {
  void *base;
  void *top;
  int pd[TLMM_NPAGES];

  /* For testing/debug */
  unsigned int count[TLMM_NPAGES];
} tlmm;


static __thread unsigned int tid;
static pthread_barrier_t barrier;

#define __exits__ __attribute__((__noreturn__))

static void __exits__ die(const char* errstr, ...) {
  va_list ap;

  va_start(ap, errstr);
  vfprintf(stderr, errstr, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(EXIT_FAILURE);
}

// printing with errno
static void __exits__ edie(const char* errstr, ...) {
  va_list ap;

  va_start(ap, errstr);
  vfprintf(stderr, errstr, ap);
  va_end(ap);
  fprintf(stderr, ": %s\n", strerror(errno));
  exit(EXIT_FAILURE);
}

static void *test_thread(void *x) {
  tid = (long)x;
  int i;
  unsigned int r;
  unsigned int *a;

  // Threads start with all pages mapped with PROT_READ
  if (sys_pmap(tlmm.base, tlmm.pd, TLMM_NPAGES, PROT_READ, 0) < 0)
    edie("sys_pmap");

  pthread_barrier_wait(&barrier);

  for (i = 0; i < TLMM_NPAGES / 2; i++) {
    r = rand() % TLMM_NPAGES; // randomly choose a page

    /* Atomic increment the shared counter for that page */
    __asm__ __volatile__ ("lock; incl %0"
              : "=m" (tlmm.count[r])
              : "m" (tlmm.count[r]));

    a = tlmm.base + (r * PAGE_SIZE);
    // write to the thread's designated slot in that page
    a[tid]++;
  }

  return NULL;
}

static void segvhandler(int signum, siginfo_t *si, void *uc) {
  void *addr;
  ucontext_t *u = (ucontext_t *) uc;
  mcontext_t *tf = &u->uc_mcontext;

  if (signum != SIGSEGV)
    die("oops, spurious signal %u", signum);

  addr = si->si_addr;

#define tfreg(r) tf->gregs[REG_##r]
  if (tlmm.base <= addr && addr < tlmm.top) {
    // Fault on a TLMM page
    unsigned int err;
    int pd;

    err = tfreg(ERR);
    if (!(err & 0x2))
      die("TLMM fault not caused by write");

    // NB TLMM kernel code requires page-alignement
    addr = (void *)((unsigned long)addr & ~(PAGE_SIZE - 1));

    // Upgrade the page to PROT_READ|PROT_WRITE
    pd = tlmm.pd[(addr - tlmm.base)/PAGE_SIZE];
    if (sys_pmap(addr, &pd, 1, PROT_READ|PROT_WRITE, 0) < 0)
      edie("segvhandler: sys_pmap failed");

    if (VERBOSE)
      printf("%2u: upgrade %016lx\n", tid, addr);

  } else {
    fprintf(stderr, "segfault on address %016lx\n", addr);
    fprintf(stderr, "rax %016lx  rbx %016lx  rcx %016lx\n",
      tfreg(RAX), tfreg(RBX), tfreg(RCX));
    fprintf(stderr, "rdx %016lx  rsi %016lx  rdi %016lx\n",
      tfreg(RDX), tfreg(RSI), tfreg(RDI));
    fprintf(stderr, "r8  %016lx  r9  %016lx  r10 %016lx\n",
      tfreg(R8), tfreg(R9), tfreg(R10));
    fprintf(stderr, "r11 %016lx  r12 %016lx  r13 %016lx\n",
      tfreg(R11), tfreg(R12), tfreg(R13));
    fprintf(stderr, "r14 %016lx  r15 %016lx  rbp %016lx\n",
      tfreg(R14), tfreg(R15), tfreg(RBP));
    fprintf(stderr, "rip %016lx  rsp %016lx\n",
      tfreg(RIP), tfreg(RSP));
    exit(EXIT_FAILURE);
  }
#undef tfreg

  return;
}

int
main(int ac, char **av) {
  int error = 0;
  pthread_t th[NTHREADS];

  // Reserve a region of the address space for Thread Local
  // Mememory Mappings (TLMM)
  long r = sys_reserve(); 
  if (r < 0)
    edie("sys_reserve");
  tlmm.base = (void *)(r + TLMM_SIZE) - (PAGE_SIZE * TLMM_NPAGES);
  tlmm.top = (void *)(r + TLMM_SIZE);

  // Allocate pages for each thread to map into TLMM.  Pages are
  // identified to the kernel with Page Descriptors (PD).  PDs are
  // global per-process.
  for (int i = 0; i < TLMM_NPAGES; i++) {
    int pd = sys_palloc();
    if (pd < 0)
      edie("sys_palloc");
    tlmm.pd[i] = pd;
  }

  // Install a signal handler to handle page faults
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = segvhandler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  if (sigaction(SIGSEGV, &sa, 0) < 0)
    edie("sigaction");

  if (pthread_barrier_init(&barrier, NULL, NTHREADS))
    edie("pthread_barrier_init");
  for (int i = 1; i < NTHREADS; i++)
    if (pthread_create(&th[i], 0, test_thread, (void *)(long)i))
      edie("pthread_create");
  // using the main thread as thread 0
  test_thread(0);

  for (int i = 1; i < NTHREADS; i++)
    pthread_join(th[i], NULL);

  // Sanity check results
  for (int i = 0; i < TLMM_NPAGES; i++) {
    unsigned int c0 = tlmm.count[i];
    unsigned int c1 = 0;
    unsigned int *a = (unsigned int *)
      (tlmm.base + (i * PAGE_SIZE));
    for (int k = 0; k < NTHREADS; k++) {
      c1 += a[k];
    }
    if (c0 != c1) {
      fprintf(stderr, "ERROR: page %u: %u != %u\n", i, c0, c1);
      error++;
    }
  }

  if(error) {
    printf("Test FAILED.\n");
  } else {
    printf("Testing wiht %d pages: test passed.\n", TLMM_NPAGES);
  }
  return 0;
}

