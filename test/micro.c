#include <sys/mman.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>

#include <errno.h>
#include <string.h>
#include <pthread.h>

#include "systlmm.h"

#define INCLUDE_TLB 0

static unsigned long tlmm_top;
static unsigned long tlmm_bot;

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

static inline uint64_t
read_tsc(void)
{
	uint32_t a, d;
	__asm __volatile("rdtsc" : "=a" (a), "=d" (d));
	return ((uint64_t) a) | (((uint64_t) d) << 32);
}

static void
bench0(void)
{
	enum { iters = 10000 };

	int i;
	int pd;
	unsigned long addr;
	uint64_t s, e, tmap, tumap;
	float f;
	int pdnull;

	pd = sys_palloc();
	if (pd < 0)
		die("bench0: sys_palloc error: %s\n", strerror(errno));

	pdnull = TLMM_PD_NULL;

	addr = tlmm_top - 4096;

	tmap = tumap = 0;
	for (i = 0; i < iters; i++) {
		s = read_tsc();
		if (sys_pmap((void *)addr, &pd, 1, PROT_READ|PROT_WRITE, 1) < 0)
			die("bench0: sys_pmap error: %s\n", strerror(errno));

		if (INCLUDE_TLB)
			*((unsigned long *)addr) = 1;

		e = read_tsc();
		tmap += (e - s);

		s = read_tsc();
		if (sys_pmap((void *)addr, &pdnull, 1, 0, 1) < 0)
			die("bench0: sys_pmap (null) error: %s",
			    strerror(errno));
		e = read_tsc();
		tumap += (e - s);
	}

	f = (float)tmap/(float)iters;
	printf("sys map %f cycles\n", f);
	f = (float)tumap/(float)iters;
	printf("sys umap %f cycles\n", f);
}

static void bench1_helper(int *pd, int *null, unsigned int n)
{
	enum { iters = 10000 };

	uint64_t s, e, tmap, tumap;
	unsigned long addr;
	unsigned int i;
	float f;

	addr = tlmm_top - (4096 * n);

	tmap = tumap = 0;
	for (i = 0; i < iters; i++) {
		s = read_tsc();
		if (sys_pmap((void *)addr, pd, n, PROT_READ|PROT_WRITE, 1) < 0)
			die("bench1: sys_pmap error: %s", strerror(errno));

		e = read_tsc();
		tmap += (e - s);

		s = read_tsc();
		if (sys_pmap((void *)addr, null, n, PROT_READ|PROT_WRITE, 1)
		    < 0)
			die("bench1: sys_pmap (null) error: %s",
			    strerror(errno));
		e = read_tsc();
		tumap += (e - s);
	}

	printf("%3u", n);
	f = (float)tmap/(float)iters;
	printf("   map %7.2f", f);
	f = (float)tumap/(float)iters;
	printf("   umap %7.2f\n", f);
}

static void bench1(void)
{
	enum { max_pages = 100 };

	int pd[max_pages], null[max_pages];
	unsigned int i;

	for (i = 0; i < max_pages; i++) {
		pd[i] = sys_palloc();
		if (pd[i] < 0)
			die("bench1: sys_palloc error: %s", strerror(errno));
	}

	for (i = 0; i < max_pages; i++)
		null[i] = TLMM_PD_NULL;

	for (i = 1; i < max_pages; i++)
		bench1_helper(pd, null, i);
}

int
main(int ac, char **av)
{
	tlmm_bot = sys_reserve();
	tlmm_top = tlmm_bot + TLMM_SIZE;

	bench0();
	bench1();
	return 0;
}
