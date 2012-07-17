#ifndef _X86_64_TLMM
#define _X86_64_TLMM

#include <linux/ioctl.h>

struct tlmm_pmap {
	unsigned long addr;
	int *upd;
	int npd;
	unsigned long prot;
};

#define _TLMM_IOCTL		0xE0
#define TLMM_RESERVE		_IOW(_TLMM_IOCTL, 0, unsigned long)
#define TLMM_PMAP		_IOR(_TLMM_IOCTL, 1, struct tlmm_pmap)
#define TLMM_PALLOC		_IO(_TLMM_IOCTL, 2)

#ifdef __KERNEL__
#define TLMM_SIZE		(1UL << 39)
#define TLMM_ALIGN(addr) 	ALIGN(addr, TLMM_SIZE)

void exit_tlmm_task(struct task_struct *tsk);
void exit_tlmm_mmap(struct mm_struct *mm);
void tlmm_sync_pud(struct task_struct *tsk, unsigned long address, pud_t *pud);

long tlmm_reserve(void);
long tlmm_palloc(void);
long tlmm_pmap(unsigned long addr, int __user *upd, int npd,
	       unsigned long prot);

#endif /* __KERNEL__ */

#endif
