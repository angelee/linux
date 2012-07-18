#include <unistd.h>
#include <sys/syscall.h>

#define SYS_reserve	299
#define SYS_pmap 	300
#define SYS_palloc 	301

#define TLMM_SIZE	(1UL << 39)
#define TLMM_PD_NULL 	(-1)

void tlmm_init(void);

long sys_reserve(void);
long sys_pmap(void *addr, int *pds, unsigned int npds, int prot, int decmap);
long sys_palloc(void);
