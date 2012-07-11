#ifndef _X86_64_TLMM
#define _X86_64_TLMM

#define TLMM_SIZE		(1UL << 39)
#define TLMM_ALIGN(addr) 	ALIGN(addr, TLMM_SIZE)

void exit_tlmm_task(struct task_struct *tsk);
void exit_tlmm_mmap(struct mm_struct *mm);
void tlmm_sync_pud(struct task_struct *tsk, unsigned long address, pud_t *pud);

#endif
