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

#define ITERS 	     100
#define NUM_PAGES    100
#define NUM_SEGS     20

static unsigned long tlmm_top;
static unsigned long tlmm_bot;
static unsigned long map_top;

static int test_seg[NUM_SEGS][NUM_PAGES];
static int pd[NUM_PAGES];


static void __attribute__((noreturn))
die(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
}

static void do_test(void *x)
{
	unsigned long addr;
	unsigned int i;
	unsigned int k;
	unsigned int r;

	for (i = 0; i < ITERS; i++) {
		r = rand() % NUM_SEGS;

		if (sys_pmap((void *)map_top, &test_seg[r][0], NUM_PAGES,
			     PROT_READ|PROT_WRITE, 1) < 0)
			die("do_test: sys_pmap error: %s", strerror(errno));

		for (k = 0; k < NUM_PAGES; k++) {
			uint64_t val;

			addr = map_top - (k * 4096);
			val = *((uint64_t *)addr);

			if ((int)val != test_seg[r][k])
				die("test: val %u test_seg[r][k] %u",
				    (int)val, test_seg[r][k]);
		}
	}
}

static void test_init(void)
{
	unsigned int i;
	unsigned long addr;

	for (i = 0; i < NUM_PAGES; i++) {
		pd[i] = sys_palloc();
		if (pd[i] < 0)
			die("test_init: sys_palloc error: %s", strerror(errno));
		/*
		 * The kernel doesn't guarantee this, but we depend
		 * on it in do_test
		 */
		assert(pd[i] == (int)i);
	}

	if (sys_pmap((void *)map_top, pd, NUM_PAGES, PROT_READ|PROT_WRITE, 1)
	    < 0)
		die("test_init: sys_pmap error: %s", strerror(errno));


	for (i = 0; i < NUM_PAGES; i++) {
		uint64_t *ptr;

		addr = map_top - (i * 4096);

		ptr = (uint64_t *)addr;
		*ptr = i;
	}

	for (i = 0; i < NUM_SEGS; i++) {
		unsigned int k;

		for (k = 0; k < NUM_PAGES; k++) {
			unsigned int r;

			r = rand() % NUM_PAGES;
			test_seg[i][k] = r;
		}
	}
}

int
main(int ac, char **av)
{
	tlmm_bot = sys_reserve();
	tlmm_top = tlmm_bot + TLMM_SIZE;
	map_top = tlmm_top - 4096;

	test_init();
	do_test(NULL);

	printf("%s test complete\n", av[0]);
	return 0;
}
