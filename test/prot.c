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

#define NTHREADS	16
#define VERBOSE		1

#define TLMM_SIZE	(1UL << 39)
#define TLMM_NPAGES	100
#define PD_NULL 	-1

static struct {
	void *base;
	void *top;
	int pd[TLMM_NPAGES];

	/* For testing/debug */
	unsigned int count[TLMM_NPAGES];
} tlmm;

static __thread unsigned int tid;

static pthread_barrier_t barrier;

static inline long sys_reserve(void *addr)
{
	return syscall(299, addr);
}

static inline long sys_pmap(void *addr, int *pd, int count, int prot)
{
	return syscall(300, addr, pd, count, prot);
}

static inline long sys_palloc(void)
{
	return syscall(301);
}

static inline long sys_punmap(void *addr)
{
	return syscall(302, addr);
}

#define __exits__ __attribute__((__noreturn__))

static void __exits__ die(const char* errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(EXIT_FAILURE);
}

static void __exits__ edie(const char* errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	fprintf(stderr, ": %s\n", strerror(errno));
	exit(EXIT_FAILURE);
}

static void *test_thread(void *x)
{
	tid = (long)x;
	int i;
	unsigned int r;
	unsigned int *a;

	/*
	 * Threads start with all pages mapped with PROT_READ
	 */
	if (sys_pmap(tlmm.base, tlmm.pd, TLMM_NPAGES, PROT_READ) < 0)
		edie("sys_pmap");

	pthread_barrier_wait(&barrier);

	for (i = 0; i < TLMM_NPAGES / 2; i++) {
		r = rand() % TLMM_NPAGES;

		/* Atomic increment the shared counter */
		__asm__ __volatile__ ("lock; incl %0"
				      : "=m" (tlmm.count[r])
				      : "m" (tlmm.count[r]));

		a = tlmm.base + (r * PAGE_SIZE);
		a[tid]++;
	}

	return NULL;
}

static void segvhandler(int signum, siginfo_t *si, void *uc)
{
	void *addr;
	ucontext_t *u = (ucontext_t *) uc;
	mcontext_t *tf = &u->uc_mcontext;

	if (signum != SIGSEGV)
		die("oops, spurious signal %u", signum);

	addr = si->si_addr;

#define tfreg(r) tf->gregs[REG_##r]
	if (tlmm.base <= addr && addr < tlmm.top) {
		/*
		 * Fault on a TLMM page
		 */
		unsigned int err;
		int pd;

		err = tfreg(ERR);
		if (!(err & 0x2))
			die("TLMM fault not caused by write");

		/*
		 * NB TLMM kernel code requires page-alignement
		 */
		addr = (void *)((unsigned long)addr & ~(PAGE_SIZE - 1));

		/*
		 * Upgrade the page to PROT_READ|PROT_WRITE
		 */
		pd = (addr - tlmm.base) / PAGE_SIZE;
		if (sys_pmap(addr, &pd, 1, PROT_READ|PROT_WRITE) < 0)
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
main(int ac, char **av)
{
	pthread_t th[NTHREADS];
	struct sigaction sa;
	long r;
	int i, k;

	/*
	 * Reserve a region of the address space for Thread Local
	 * Mememory Mappings (TLMM)
	 */
	r = sys_reserve(0);
	if (r < 0)
		edie("sys_reserve");
	tlmm.base = (void *)(r + TLMM_SIZE) - (PAGE_SIZE * TLMM_NPAGES);
	tlmm.top = (void *)(r + TLMM_SIZE);

	/*
	 * Allocate pages for each thread to map into TLMM.  Pages are
	 * identified to the kernel with Page Descriptors (PD).  PDs are
	 * global per-process.
	 */
	for (i = 0; i < TLMM_NPAGES; i++) {
		r = sys_palloc();
		if (r < 0)
			edie("sys_palloc");
		tlmm.pd[i] = r;
	}

	/*
	 * Install a signal handler to handle page faults
	 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = segvhandler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	if (sigaction(SIGSEGV, &sa, 0) < 0)
		edie("sigaction");

	if (pthread_barrier_init(&barrier, NULL, NTHREADS))
		edie("pthread_barrier_init");
	for (i = 1; i < NTHREADS; i++)
		if (pthread_create(&th[i], 0, test_thread, (void *)(long)i))
			edie("pthread_create");
	test_thread(0);

	for (i = 1; i < NTHREADS; i++)
		pthread_join(th[i], NULL);

	/*
	 * Sanity check results
	 */
	for (i = 0; i < TLMM_NPAGES; i++) {
		unsigned int c0 = tlmm.count[i];
		unsigned int c1 = 0;
		unsigned int *a = (unsigned int *)
			(tlmm.base + (i * PAGE_SIZE));
		for (k = 0; k < NTHREADS; k++)
			c1 += a[k];

		if (c0 != c1)
			fprintf(stderr, "error: page %u: %u != %u\n",
				i, c0, c1);
	}

	printf("test complete\n");
	return 0;
}
