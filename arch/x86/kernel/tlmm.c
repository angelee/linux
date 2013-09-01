/*
 * Copyright (C) 2010 Silas Boyd-Wickizer and Angelina Lee
 *
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Send feedback to Silas Boyd-Wickizer <sbw@mit.edu>
 */

/*
 * XXX One approach to implementing pd_free is to reference count PD pages.
 * One convenient way to implement ref. counting it to use the ref. counts
 * included in the struct page (see include/linux/mm_types.h).  There is a
 * struct page for each physical page.  Given a physical address PA, you can
 * convert it to the corresponding struct page as follows:
 *
 *   void *kernel_virtual_address = __va(PA);
 *   struct page *page = virt_to_page(kernel_virtual_address);
 *   atomic_inc(page->_mapcount);
 *
 * See include/linux/mm.h for the semantics of page->_count and
 * page->_mapcount.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sysctl.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/slab.h>

#include <asm/tlmm.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>
#include <asm/pgalloc.h>
#include <asm/io.h>

/* Toggles Cilk-M pmap semantics or the mmap-like pmap semantics */
#define CILK_COMPAT    	   1
/* Maximum number of Page Descriptors that we copy onto the stack */
#define MAXSTACKPDS	   32
/* Initial tlmm table size */
#define INIT_TLMM_TABLE_SIZE    1024

/*
 * The Linux page table code is hard to work with.  It often assumes
 * a mm_struct and it is difficult to control how it allocates memory.
 * Therefore we implement some simple x86-64 page table code.
 */
#define NPTBITS		9		     /* log2(NPTENTRIES) */
#define NPTLVLS	    	3		     /* page table depth -1 */
#define NPTENTRIES	(1 << NPTBITS)

/* Page table/directory entry flags. */
#define PTE_P		0x001		     /* Present */
#define PTE_W		0x002		     /* Writeable */
#define PTE_U		0x004		     /* User */
#define PTE_A   	0x020                /* Accessed */
#define PTE_D   	0x040          	     /* Dirty */
#define PTE_NX		0x8000000000000000UL /* No execute */

#define PDXMASK		((1 << NPTBITS) - 1)
#define PDSHIFT(n)	(12 + NPTBITS * (n))
#define PDX(n, la)	((((uintptr_t) (la)) >> PDSHIFT(n)) & PDXMASK)
#define PTE_ADDR(pte) 	((unsigned long) (pte) & 0xFFFFFFFFFF000UL)

typedef unsigned long ptent_t;

/* Layout of a hardware page map */
struct tlmm_pgmap {
	ptent_t pm_ent[NPTENTRIES];
};

/*
 * Converts Page Derscriptors to the kernel virtual address of the
 * corresponding physical page.
 */
struct tlmm_table {
	int n;				/* the actual number of valid entries */
	int cp_index;		/* the lowest index to copy */
	void **page_map;	/* an array of page ptrs, indexed by its pd */
	void **next_page_map; /* same as page_map but twice as large */
	int size;			/* size of the tlmm table, page_map */
};

static int tlmm_next_dec(int pos)
{
	return pos - 1;
}

static int tlmm_next_inc(int pos)
{
	return pos + 1;
}

static inline void tlmm_page_free(void *va)
{
	free_page((unsigned long)va);
}

static inline void *tlmm_page_alloc(void)
{
	return (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
}

static inline int tlmm_get_pd_page(struct mm_struct *mm, unsigned int pd,
				   void **p)
{
	if (!mm->tlmm_table || pd >= mm->tlmm_table->n)
		return -EINVAL;

	if (!mm->tlmm_table->page_map[pd])
		return -EINVAL;

	*p = mm->tlmm_table->page_map[pd];
	return 0;
}

static inline int tlmm_addr(const void *addr)
{
	const void *tlmm;

	tlmm = (void *)current->mm->tlmm;

	return (tlmm != NULL &&
		tlmm <= addr &&
		addr < tlmm + TLMM_SIZE);
}

static inline int tlmm_handle_pd_map(int pd, ptent_t *pm_entp, u64 ptflags)
{
	void *page;
	int r;

	if (pd == -1) {
		*pm_entp = 0;
	} else {
		r = tlmm_get_pd_page(current->mm, pd, &page);
		if (r < 0)
			return r;
		*pm_entp = __pa(page) | ptflags;
	}

	return 0;
}

static int page_map_traverse(struct tlmm_pgmap *pgmap, int pmlevel,
			     int *pd, int n, int *pos,
			     const void *first, const void *last,
			     u64 ptflags, int (*next_pos)(int))
{
	struct tlmm_pgmap *pgmap_next;
	const void *first_next;
	const void *last_next;
	uint32_t first_idx;
	uint32_t last_idx;
	ptent_t *pm_entp;
	ptent_t pm_ent;
	uint64_t idx;
	int r;

	first_idx = PDX(pmlevel, first);
	last_idx  = PDX(pmlevel, last);

	for (idx = first_idx; idx <= last_idx; idx++) {
		pm_entp = &pgmap->pm_ent[idx];
		pm_ent = *pm_entp;

		/*
		 * We've hit the bottom of the page map, which is known as a
		 * "page table".  Each page table is 512 64-bit entries.  Each
		 * entry maps a 4KByte page.
		 */
		if (pmlevel == 0) {
			r = tlmm_handle_pd_map(pd[*pos], pm_entp, ptflags);
			if (r < 0)
				return r;
			*pos = next_pos(*pos);
			continue;
		}

		/*
		 * The next level we're going to recurse on isn't present.
		 * So we add it by allocating a new page and pointing the
		 * pm_ent of the current level at the physical address of
		 * the new page.
		 */
		if (!(pm_ent & PTE_P)) {
			void *p;

			p = tlmm_page_alloc();
			if (p == NULL)
				return -ENOMEM;

			pm_ent = __pa(p) | PTE_P | PTE_U | PTE_W;
			*pm_entp = pm_ent;
		}

		/*
		 * Recurse down to the next level.
		 */
		pgmap_next = (struct tlmm_pgmap *) __va(PTE_ADDR(pm_ent));
		first_next = (idx == first_idx) ?
			first : 0;
		last_next  = (idx == last_idx)  ?
			last  : (const void *) (uintptr_t) ~0UL;
		r = page_map_traverse(pgmap_next, pmlevel - 1, pd, n, pos,
				      first_next, last_next, ptflags,
				      next_pos);
		if (r < 0)
			return r;
	}

	return 0;
}

static void tlmm_page_map_free(struct tlmm_pgmap *pgmap, int level,
			       const void *first, const void *last)
{
	const void *first_next;
	const void *last_next;
	struct tlmm_pgmap *pm;
	int first_idx;
	int last_idx;
	int i;

	first_idx = PDX(level, first);
	last_idx = PDX(level, last);

	for (i = first_idx; i <= last_idx; i++) {
		ptent_t ptent = pgmap->pm_ent[i];
		if (!(ptent & PTE_P))
			continue;
		if (level > 0) {
			pm = (struct tlmm_pgmap *) __va(PTE_ADDR(ptent));
			first_next = (i == first_idx) ? first : 0;
			last_next  = (i == last_idx)  ?
				last  : (const void *) (uintptr_t) ~0UL;
			tlmm_page_map_free(pm, level - 1, first_next,
					   last_next);
		}
	}

	tlmm_page_free(pgmap);
}

static long get_unmapped_reserve(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	unsigned long addr;

	addr = TLMM_ALIGN(TASK_SIZE - TLMM_SIZE);
	do {
		vma = find_vma(mm, addr);
		if (!vma || addr + TLMM_SIZE <= vma->vm_start)
			return addr;
		addr -= TLMM_SIZE;
	} while (addr > TASK_UNMAPPED_BASE);

	return -ENOMEM;
}

long tlmm_reserve(void)
{
	struct mm_struct *mm = current->mm;
	long error = -ENOMEM;

	down_write(&mm->mmap_sem);
	if (!mm->tlmm) {
		error = get_unmapped_reserve(mm);
		if (error > 0)
			mm->tlmm = error;
	}
	up_write(&mm->mmap_sem);

	return error;
}

static inline int expand_tlmm_table(struct tlmm_table *table)
{
	void **new_array;
    /* size of the new array for next_page_map */
	int new_size = table->size << 2;

	if (new_size <= 0)  /* overflow */
		return -ENOMEM;

	new_array = kmalloc(sizeof(void *)*new_size, GFP_KERNEL);
	if (!new_array)
		return -ENOMEM;

	table->cp_index = table->size - 1;
	table->size = (table->size << 1);
	kfree(table->page_map);
	table->page_map = table->next_page_map;
	table->next_page_map = new_array;

	return 0;
}

static inline int tlmm_alloc_pd(struct mm_struct *mm)
{
	void *page;
	int n;

    /* first time init tlmm_table */
	if (mm->tlmm_table == NULL) {
		mm->tlmm_table = kmalloc(sizeof(*mm->tlmm_table), __GFP_ZERO);
		if (mm->tlmm_table == NULL)
			return -ENOMEM;
		mm->tlmm_table->page_map =
			kmalloc(sizeof(void *)*INIT_TLMM_TABLE_SIZE,
				__GFP_ZERO);
		if (mm->tlmm_table->page_map == NULL) {
			kfree(mm->tlmm_table);
			return -ENOMEM;
		}
		mm->tlmm_table->size = INIT_TLMM_TABLE_SIZE;

		mm->tlmm_table->next_page_map =
			kmalloc(sizeof(void *)*(INIT_TLMM_TABLE_SIZE << 1),
				__GFP_ZERO);
		if (mm->tlmm_table->next_page_map == NULL) {
			kfree(mm->tlmm_table->page_map);
			kfree(mm->tlmm_table);
			return -ENOMEM;
		}
		/* no need to copy additional pds until first expand */
		mm->tlmm_table->cp_index = -1;
	}

	n = mm->tlmm_table->n;
	if (n >= mm->tlmm_table->size) {
		if (expand_tlmm_table(mm->tlmm_table))
			return -ENOMEM;
	}

	page = tlmm_page_alloc();
	if (page == NULL)
		return -ENOMEM;

	mm->tlmm_table->page_map[n] = page;
	mm->tlmm_table->next_page_map[n] = page;
    /* always true after first expand */
	if (mm->tlmm_table->cp_index >= 0) {
		mm->tlmm_table->next_page_map[mm->tlmm_table->cp_index] =
			mm->tlmm_table->page_map[mm->tlmm_table->cp_index];
		mm->tlmm_table->cp_index--;
	}
	mm->tlmm_table->n = n + 1;

	return n;
}

static void tlmm_free_pd(struct mm_struct *mm, unsigned int pd)
{
	void *p;

	p = mm->tlmm_table->page_map[pd];
	tlmm_page_free(p);
	mm->tlmm_table->page_map[pd] = NULL;
	mm->tlmm_table->next_page_map[pd] = NULL;
}

long tlmm_palloc(void)
{
	long pd;

	down_write(&current->mm->mmap_sem);
	pd = tlmm_alloc_pd(current->mm);
	up_write(&current->mm->mmap_sem);

	return pd;
}

static int do_pmap(int *pd, int n, const void *addr, unsigned int vm_flags,
		   int decmap)
{
	const void *last;
	const void *start;
	u64 ptflags;
	int pos;
	int r;

	if (decmap) {
		pos = n - 1;
		last = addr;
		start = addr - ((n - 1) * PAGE_SIZE);
	} else {
		pos = 0;
		start = addr;
		last = addr + ((n - 1) * PAGE_SIZE);
	}

	if (!tlmm_addr(addr) || !tlmm_addr(last))
		return -EINVAL;

	if (current->tlmm_pgmap == NULL) {
		struct tlmm_pgmap *pgmap;

		pgmap = tlmm_page_alloc();
		if (pgmap == NULL)
			return -ENOMEM;

		memcpy(pgmap, current->mm->pgd, PAGE_SIZE);
		current->tlmm_pgmap = pgmap;
	}

	ptflags = PTE_P | PTE_U | PTE_NX;
	if (vm_flags & VM_WRITE)
		ptflags |= PTE_W;
	if (vm_flags & VM_EXEC)
		ptflags &= ~PTE_NX;

	r = page_map_traverse(current->tlmm_pgmap, NPTLVLS, pd, n,
			      &pos, (void *)start, (void *)last, ptflags,
			      decmap ? tlmm_next_dec : tlmm_next_inc);
	if (r < 0)
		return r;

	/*
	 * NB this flushes the TLB *and* loads current->tlmm_pgmap if the code
	 * above just allocated it.  Hardware might optimize the flush, or it
	 * might not.  I haven't seen a noticeable difference using invlpg to
	 * flush individual entries.
	 */
	load_cr3((pgd_t *)current->tlmm_pgmap);
	return 0;
}

long tlmm_pmap(unsigned long addr, int __user *upd, int npd,
	       unsigned long prot, int decmap)
{
	long ret = -ENOMEM;
	int stack_pd[MAXSTACKPDS];
	int *kmalloc_pd = NULL;
	int *pd;
	unsigned int vm_flags;

	if (!npd || addr & ~PAGE_MASK)
		return -EINVAL;

	/* Try to avoid calling kmalloc */
	if (npd > MAXSTACKPDS) {
		kmalloc_pd = kmalloc(sizeof(*kmalloc_pd) * npd, GFP_KERNEL);
		if (!kmalloc_pd)
			return -ENOMEM;
		pd = kmalloc_pd;
	} else {
		pd = stack_pd;
	}

	if (copy_from_user(pd, upd, sizeof(int) * npd)) {
		ret = -EFAULT;
		goto done;
	}

	vm_flags = calc_vm_prot_bits(prot) & (VM_WRITE | VM_READ);

	ret = do_pmap(pd, npd, (void *) addr, vm_flags, decmap);

done:
	if (kmalloc_pd)
		kfree(kmalloc_pd);

	return ret;
}

void tlmm_sync_pud(struct task_struct *tsk, unsigned long address, pud_t *pud)
{
	unsigned long pa;
	ptent_t *pm_ent;

	if (!tlmm_addr((void *)address)) {
		/*
		 * NB this code assumes the entries in the top-level of the
		 * shared page map (aka mm->pgd) only changes from a NULL entry
		 * (not present) to a non-NULL entry (present), but never from
		 * non-NULL entry to another non-NULL entry.
		 */
		pm_ent = &tsk->tlmm_pgmap->pm_ent[PDX(NPTLVLS, address)];
		if (!(*pm_ent & PTE_P)) {
			pa = __pa(pud) & PAGE_MASK;
			*pm_ent = pa | PTE_P | PTE_U | PTE_W | PTE_A | PTE_D;
		}
	}
}

void exit_tlmm_task(struct task_struct *tsk)
{
	struct mm_struct *mm;
	void *addr;

	mm = tsk->mm;

	if (!mm || !mm->tlmm || !tsk->tlmm_pgmap)
		return;

	load_cr3(mm->pgd);
	addr = (void *) mm->tlmm;
	tlmm_page_map_free(tsk->tlmm_pgmap, NPTLVLS, addr,
			   addr + TLMM_SIZE - PAGE_SIZE);

	tsk->tlmm_pgmap = NULL;
}

void exit_tlmm_mmap(struct mm_struct *mm)
{
	int i;

	if (!mm->tlmm_table)
		return;

	for (i = 0; i < mm->tlmm_table->n; i++)
		if (mm->tlmm_table->page_map[i])
			tlmm_free_pd(mm, i);

	kfree(mm->tlmm_table);
	mm->tlmm_table = NULL;
}
