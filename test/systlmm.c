#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "../arch/x86/include/asm/tlmm.h"

#include "systlmm.h"

static pthread_mutex_t inited_mu = PTHREAD_MUTEX_INITIALIZER;
static char inited;
static int devfd;

static void init(void)
{
	int fd;

	pthread_mutex_lock(&inited_mu);
	if (inited)
		goto done;
	fd = open("/dev/tlmm", O_RDWR);
	if (fd < 0) {
		perror("open /dev/tlmm:");
		exit(EXIT_FAILURE);
	}
	devfd = fd;
	inited = 1;
done:
	pthread_mutex_unlock(&inited_mu);
}

long sys_reserve(void)
{
	unsigned long addr;
	int r;

	if (!inited)
		init();

	r = ioctl(devfd, TLMM_RESERVE, &addr);
	if (r)
		return r;
	return addr;
}

long sys_pmap(void *addr, int *pds, unsigned int npds, int prot, int decmap)
{
	struct tlmm_pmap p;

	if (!inited)
		init();

	p.addr = (unsigned long)addr;
	p.upd = pds;
	p.npd = npds;
	p.prot = prot;
	p.decmap = decmap;
	return ioctl(devfd, TLMM_PMAP, &p);
}

long sys_palloc(void)
{
	if (!inited)
		init();

	return ioctl(devfd, TLMM_PALLOC);
}

