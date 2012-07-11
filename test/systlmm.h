#include <unistd.h>
#include <sys/syscall.h>

#define SYS_reserve	299
#define SYS_pmap 	300
#define SYS_palloc 	301
#define SYS_pumap 	302

#define TLMM_SIZE	(1UL << 39)
#define TLMM_PD_NULL 	(-1)

static inline long sys_reserve(unsigned long addr)
{
	return syscall(SYS_reserve, addr);
}

static inline long sys_pmap(void *addr, int *pds, unsigned int npds, int prot)
{
	return syscall(SYS_pmap, addr, pds, npds, prot);
}

static inline long sys_pumap(unsigned long addr)
{
	return syscall(SYS_pumap, addr);
}

static inline int sys_palloc(void)
{
	return syscall(SYS_palloc);
}

