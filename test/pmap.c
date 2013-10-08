#include <sys/mman.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>

#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>

#include "systlmm.h"

#define ITERS        100
#define NUM_PAGES    5000 
#define NUM_SEGS     20

static unsigned long tlmm_top;
static unsigned long tlmm_bot;
static unsigned long map_top;

static int test_seg[NUM_SEGS][NUM_PAGES];
static int pd[NUM_PAGES];


static void __attribute__((noreturn))
die(const char *errstr, ...) {
  va_list ap;

  va_start(ap, errstr);
  vfprintf(stderr, errstr, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(EXIT_FAILURE);
}

static void do_test(void *x) {
  for (unsigned int i = 0; i < ITERS; i++) {
    unsigned int r = rand() % NUM_SEGS;

    // now remap the pages using array with randomly permuted pds
    if (sys_pmap((void *)map_top, &test_seg[r][0], NUM_PAGES,
          PROT_READ|PROT_WRITE, 1) < 0)
      die("do_test: sys_pmap error: %s", strerror(errno));

    for (unsigned int k = 0; k < NUM_PAGES; k++) {
      int val;
      uint64_t addr;

      addr = map_top - (k * 4096);
      // get value stored at 
      val = *((uint64_t *)addr);

      if (val != pd[test_seg[r][k]])
        die("test: val %d test_seg[r][k] %d", val, pd[test_seg[r][k]]);
    }
  }
}

static void test_init(void) {
  for (unsigned int i = 0; i < NUM_PAGES; i++) { // allocate NUM_PAGES pages
    pd[i] = sys_palloc();
    if (pd[i] < 0)
      die("test_init: sys_palloc error: %s", strerror(errno));
    // The kernel doesn't guarantee this, but we depend
    // on it in do_test
    // Not necessary; just store pd[i] instead of i in the page
    // assert(pd[i] == (int)i);
  }

  // map as a stack; sys_pmap takes the address of the first page as an 
  // argument, which resides at the top (high addres) and grows downward.  
  // The address to sys_pmap uses the lower address of the first page
  // (map_top = tlm_top - PAGE_SIZE).  The pages mapped in from higher to 
  // lower address are specified in the pd array, with indices 0 - NUM_PAGE-1.
  // Each page with page descriptor 'i' contains the value 'i' at the
  // beginning of the page (lower address)
  if (sys_pmap((void *)map_top, pd, NUM_PAGES, PROT_READ|PROT_WRITE, 1)
      < 0)
    die("test_init: sys_pmap error: %s", strerror(errno));

  for (unsigned int i = 0; i < NUM_PAGES; i++) {
    uint64_t *ptr;
    unsigned long addr;
    addr = map_top - (i * 4096);
    ptr = (uint64_t *)addr;
    *ptr = pd[i]; // store pd[i] in the page as a marker
  }

  for (unsigned int i = 0; i < NUM_SEGS; i++) {
    for (unsigned int k = 0; k < NUM_PAGES; k++) {
      unsigned int r;
      r = rand() % NUM_PAGES;
      // init test_seg with random numbers between 0 -- NUM_PAGES-1
      test_seg[i][k] = (int) r;
    }
  }
}

int
main(int ac, char **av) {
  tlmm_bot = sys_reserve(); // lower addr
  tlmm_top = tlmm_bot + TLMM_SIZE; // higher addr
  map_top = tlmm_top - 4096;

  test_init();
  do_test(NULL);

  // test complete if didn't fail assert and die
  printf("%s test with %d pages complete\n", av[0], NUM_PAGES);
  return 0;
}
