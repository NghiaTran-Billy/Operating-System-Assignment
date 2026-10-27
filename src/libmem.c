/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * System Library
 * Memory Module Library libmem.c
 */

// #ifdef MM_PAGING

#include "string.h"
#include "mm64.h"
#include "syscall.h"
#include "libmem.h"
#include "dma.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t kmm_lock = PTHREAD_MUTEX_INITIALIZER;

/* Page size of the active addressing mode (power of two) */
#ifdef MM64
#define LIB_PGSZ PAGING64_PAGESZ
#else
#define LIB_PGSZ PAGING_PAGESZ
#endif

/* System call number of sys_memmap (see src/syscall.tbl) */
#define SYSCALL_MEMMAP_NR 17

/*enlist_vm_freerg_list - add new rg to freerg_list
 *@mm: memory region
 *@rg_elmt: new region
 *
 */
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt)
{
  struct vm_rg_struct *rg_node = mm->mmap->vm_freerg_list;

  if (rg_elmt->rg_start >= rg_elmt->rg_end)
    return -1;

  if (rg_node != NULL)
    rg_elmt->rg_next = rg_node;

  /* Enlist the new region */
  mm->mmap->vm_freerg_list = rg_elmt;

  return 0;
}

/*get_symrg_byid - get mem region by region ID
 *@mm: memory region
 *@rgid: region ID act as symbol index of variable
 *
 */
struct vm_rg_struct *get_symrg_byid(struct mm_struct *mm, int rgid)
{
  if (rgid < 0 || rgid > PAGING_MAX_SYMTBL_SZ)
    return NULL;

  return &mm->symrgtbl[rgid];
}

/*__alloc - allocate a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *@alloc_addr: address of allocated memory region
 *
 */
int __alloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /*Allocate at the toproof */
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct rgnode;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  int inc_sz=0;

  if (get_free_vmrg_area(caller, vmaid, size, &rgnode) == 0)
  {
    caller->mm->symrgtbl[rgid].rg_start = rgnode.rg_start;
    caller->mm->symrgtbl[rgid].rg_end = rgnode.rg_end;
 
    *alloc_addr = rgnode.rg_start;

    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

  /* TODO get_free_vmrg_area FAILED handle the region management (Fig.6)*/

  /*Attempt to increate limit to get space */
#ifdef MM64
  inc_sz = (uint32_t)(size/(int)PAGING64_PAGESZ);
  inc_sz = inc_sz + 1;
#else
  inc_sz = PAGING_PAGE_ALIGNSZ(size);
#endif
  int old_sbrk;
  inc_sz = inc_sz + 1;

  old_sbrk = cur_vma->sbrk;

  /* TODO INCREASE THE LIMIT
   * SYSCALL 1 sys_memmap
   */
  struct sc_regs regs;
  regs.a1 = SYSMEM_INC_OP;
  regs.a2 = vmaid;
#ifdef MM64
  regs.a3 = size;
#else
  regs.a3 = PAGING_PAGE_ALIGNSZ(size);
#endif  
  _syscall(caller->krnl, caller->pid, 17, &regs); /* SYSCALL 17 sys_memmap */
  /*Successful increase limit */
  caller->mm->symrgtbl[rgid].rg_start = old_sbrk;
  caller->mm->symrgtbl[rgid].rg_end = old_sbrk + size;

  *alloc_addr = old_sbrk;

  pthread_mutex_unlock(&mmvm_lock);
  return 0;

}

/*__free - remove a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __free(struct pcb_t *caller, int vmaid, int rgid)
{
  pthread_mutex_lock(&mmvm_lock);

  if (rgid < 0 || rgid > PAGING_MAX_SYMTBL_SZ)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* TODO: Manage the collect freed region to freerg_list */
  struct vm_rg_struct *rgnode = get_symrg_byid(caller->mm, rgid);

  if (rgnode->rg_start == 0 && rgnode->rg_end == 0)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  struct vm_rg_struct *freerg_node = malloc(sizeof(struct vm_rg_struct));
  freerg_node->rg_start = rgnode->rg_start;
  freerg_node->rg_end = rgnode->rg_end;
  freerg_node->rg_next = NULL;

  rgnode->rg_start = rgnode->rg_end = 0;
  rgnode->rg_next = NULL;

  /*enlist the obsoleted memory region */
  enlist_vm_freerg_list(caller->mm, freerg_node);

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*liballoc - PAGING-based allocate a region memory
 *@proc:  Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */
int liballoc(struct pcb_t *proc, addr_t size, uint32_t reg_index)
{
  addr_t  addr;
  int val = __alloc(proc, 0, reg_index, size, &addr);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  /* By default using vmaid = 0 */
  return val;
}

/*libfree - PAGING-based free a region memory
 *@proc: Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */

int libfree(struct pcb_t *proc, uint32_t reg_index)
{
  int val = __free(proc, 0, reg_index);
  if (val == -1)
  {
    return -1;
  }
printf("%s:%d\n",__func__,__LINE__);
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif
  return 0;//val;
}

/*
 * pg_evict_victim - free one RAM frame by swapping a victim page out
 * @caller: process whose FIFO list supplies the victim
 * @mm: memory management struct of that process
 * @retfpn: returned RAM frame (its content is already saved in swap)
 * @via_syscall: 1 = copy through sys_memmap (user-space caller),
 *               0 = call the kernel routine directly (caller is already in
 *                   kernel space, e.g. inside sys_memmap, must not re-enter it)
 *
 * The victim is the oldest page of mm->fifo_pgn (FIFO replacement).
 * Return 0 on success, -1 when there is no victim or no free swap frame;
 * in that case nothing has been modified.
 */
static int pg_evict_victim(struct pcb_t *caller, struct mm_struct *mm,
                           addr_t *retfpn, int via_syscall)
{
  struct memphy_struct *mswp = caller->krnl->active_mswp;
  addr_t vicpgn, vicpte, vicfpn, swpfpn;

  /* Get free frame in MEMSWP first, so a failure leaves the FIFO untouched */
  if (MEMPHY_get_freefp(mswp, &swpfpn) != 0)
    return -1;

  /* Find victim page; skip stale FIFO entries that are no longer in RAM */
  while (1)
  {
    if (find_victim_page(mm, &vicpgn) != 0)
    {
      MEMPHY_put_freefp(mswp, swpfpn);
      return -1;
    }

    vicpte = pte_get_entry(mm, vicpgn, caller);
    if (PAGING_PAGE_PRESENT(vicpte))
      break;
  }
  vicfpn = PAGING_FPN(vicpte);

  /* Copy victim frame to swap: SWP(vicfpn --> swpfpn) */
  if (via_syscall)
  {
    struct sc_regs regs;

    memset(&regs, 0, sizeof(regs));
    regs.a1 = SYSMEM_SWP_OP;
    regs.a2 = vicfpn;
    regs.a3 = swpfpn;
    _syscall(caller->krnl, caller->pid, SYSCALL_MEMMAP_NR, &regs); /* SYSCALL 17 sys_memmap */
  }
  else
  {
    __mm_swap_page(caller, vicfpn, swpfpn);
  }

  /* The victim now lives in swap */
  pte_set_swap(caller, vicpgn, caller->krnl->active_mswp_id, swpfpn);

  *retfpn = vicfpn;
  return 0;
}

/*libmem_evict_page - kernel-side helper: obtain a RAM frame by swapping out
 *                    one page of the caller (used when RAM has no free frame,
 *                    e.g. from alloc_pages_range)
 *@caller: process asking for a frame
 *@retfpn: returned frame number
 *
 * Does NOT take mmvm_lock and does NOT issue a system call: it is meant to be
 * called from kernel code that already runs inside sys_memmap.
 */
int libmem_evict_page(struct pcb_t *caller, addr_t *retfpn)
{
  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
      caller->krnl->active_mswp == NULL || retfpn == NULL)
    return -1;

  return pg_evict_victim(caller, caller->mm, retfpn, 0);
}

/*pg_getpage - get the page in ram
 *@mm: memory region
 *@pagenum: PGN
 *@framenum: return FPN
 *@caller: caller
 *
 */
int pg_getpage(struct mm_struct *mm, addr_t pgn, addr_t *fpn, struct pcb_t *caller)
{
  addr_t pte = pte_get_entry(mm, pgn, caller);

  if (!PAGING_PAGE_PRESENT(pte))
  { /* Page is not online, make it actively living */
    struct memphy_struct *mram = caller->krnl->mram;
    struct memphy_struct *mswp = caller->krnl->active_mswp;
    addr_t tgtfpn;                 /* RAM frame that will hold the page */
    addr_t tgtswp = PAGING_SWP(pte); /* swap frame that holds it now */

    /* A page that is neither present nor swapped was never mapped */
    if (!(pte & PAGING_PTE_SWAPPED_MASK))
      return -1;

    /* Initialize the target frame storing our variable:
     * a free frame if RAM has one, otherwise swap a victim out */
    if (MEMPHY_get_freefp(mram, &tgtfpn) != 0)
    {
      if (pg_evict_victim(caller, mm, &tgtfpn, 1) != 0)
        return -1;
    }

    /* Copy target frame from swap to RAM: SWP(tgtswp --> tgtfpn) */
    __swap_cp_page(mswp, tgtswp, mram, tgtfpn);

    /* Update its online status of the target page */
    pte_set_fpn(mm, pgn, tgtfpn, caller);

    /* The swap copy is obsolete now, give the swap frame back */
    MEMPHY_put_freefp(mswp, tgtswp);

    /* Track the page for later replacement (FIFO) */
    enlist_pgn_node(&mm->fifo_pgn, pgn);

    *fpn = tgtfpn;
    return 0;
  }

  *fpn = PAGING_FPN(pte);

  return 0;
}

/*pg_getval - read value at given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_getval(struct mm_struct *mm, addr_t addr, BYTE *data, struct pcb_t *caller)
{
  addr_t pgn = addr / LIB_PGSZ;
  addr_t off = addr % LIB_PGSZ;
  addr_t fpn, phyaddr;
  struct sc_regs regs;

  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  phyaddr = fpn * LIB_PGSZ + off;
  if (phyaddr >= (addr_t)caller->krnl->mram->maxsz)
    return -1;

  /* MEMPHY READ with SYSMEM_IO_READ: SYSCALL 17 sys_memmap */
  memset(&regs, 0, sizeof(regs));
  regs.a1 = SYSMEM_IO_READ;
  regs.a2 = phyaddr;
  if (_syscall(caller->krnl, caller->pid, SYSCALL_MEMMAP_NR, &regs) != 0)
    return -1;

  *data = (BYTE)regs.a3;

  return 0;
}

/*pg_setval - write value to given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_setval(struct mm_struct *mm, addr_t addr, BYTE value, struct pcb_t *caller)
{
  addr_t pgn = addr / LIB_PGSZ;
  addr_t off = addr % LIB_PGSZ;
  addr_t fpn, phyaddr;
  struct sc_regs regs;

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  phyaddr = fpn * LIB_PGSZ + off;
  if (phyaddr >= (addr_t)caller->krnl->mram->maxsz)
    return -1;

  /* MEMPHY WRITE with SYSMEM_IO_WRITE: SYSCALL 17 sys_memmap */
  memset(&regs, 0, sizeof(regs));
  regs.a1 = SYSMEM_IO_WRITE;
  regs.a2 = phyaddr;
  regs.a3 = (unsigned char)value;
  if (_syscall(caller->krnl, caller->pid, SYSCALL_MEMMAP_NR, &regs) != 0)
    return -1;

  return 0;
}

/*__read - read value in region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __read(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  struct vm_rg_struct *currg;
  struct vm_area_struct *cur_vma;
  int ret;

  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
      data == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  pthread_mutex_lock(&mmvm_lock);

  /* Use the memory space of the process itself, not the kernel one */
  currg = get_symrg_byid(caller->mm, rgid);
  cur_vma = get_vma_by_num(caller->mm, vmaid);

  /* Invalid memory identify: unknown region/area, freed region, or offset
   * beyond the end of the region */
  if (currg == NULL || cur_vma == NULL || currg->rg_start + offset >= currg->rg_end)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  ret = pg_getval(caller->mm, currg->rg_start + offset, data, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/*libread - PAGING-based read a region memory */
int libread(
    struct pcb_t *proc, // Process executing the instruction
    uint32_t source,    // Index of source register
    addr_t offset,    // Source address = [source] + [offset]
    uint32_t* destination)
{
  BYTE data = 0;
printf("%s:%d\n",__func__,__LINE__);
  int val = __read(proc, 0, source, offset, &data);

  /* Hand the byte (as unsigned) back to the caller only if it was read */
  if (val == 0 && destination != NULL)
    *destination = (uint32_t)(unsigned char)data;
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}

/*__write - write a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __write(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  struct vm_rg_struct *currg;
  struct vm_area_struct *cur_vma;
  int ret;

  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
      rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  pthread_mutex_lock(&mmvm_lock);

  /* Use the memory space of the process itself, not the kernel one */
  currg = get_symrg_byid(caller->mm, rgid);
  cur_vma = get_vma_by_num(caller->mm, vmaid);

  /* Invalid memory identify */
  if (currg == NULL || cur_vma == NULL || currg->rg_start + offset >= currg->rg_end)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  ret = pg_setval(caller->mm, currg->rg_start + offset, value, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/*libwrite - PAGING-based write a region memory */
int libwrite(
    struct pcb_t *proc,   // Process executing the instruction
    BYTE data,            // Data to be wrttien into memory
    uint32_t destination, // Index of destination register
    addr_t offset)
{
  int val = __write(proc, 0, destination, offset, data);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}


/*libkmem_malloc- alloc region memory in kmem
 *@caller: caller
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 */

int libkmem_malloc(struct pcb_t * caller, uint32_t size, uint32_t reg_index)
{
  /* 
   * TODO: provide OS level management
   *       and forward the request to helper
   */
//addr_t  addr;
//int val = ...

  /* 
   * TODO: provide OS kmem allocation validation
   */

  return 0;
}


/*kmalloc - alloc region memory in kmem
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 *@alloc_addr: allocated address
 */
addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /* TODO: provide OS kernel memory allocation
   *       update krnl_pgd for OS kernel level management */

  //struct krnl_t *krnl = caller->krnl;
  //krnl->symrgtbl...
  //krnl->krnl_pgd ...

  return 0;

}



/*libmem_dma_memcpy - DMA memcpy
 *@caller: caller
 *@src: source region
 *@dst: destination region
 *@offset: memory offset
 *@size: copied data size
 */
int libmem_dma_memcpy(struct pcb_t *caller, uint32_t src, uint32_t dst, uint32_t offset, uint32_t size)
{
  /* TODO: provide DMA mempy */

  //struct krnl_t *krnl = caller->krnl;

  /*
   *  TODO: provide asynchronous dma memcpy
   *        handling the dma_done_callback
   */
  //do_dma(caller, (uint64_t) src, (uint64_t) dst, (uint64_t) size);
  //krnl->...
  //krnl->...

  return 0;
}


int libkmem_copy_from_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  /*
   * TODO: Map kernel address range
   */
  //__read_user_mem(...)
  //__write_kernel_mem(...);

  return 0;
}

int libkmem_copy_to_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  /*
   * TODO: Map kernel address range
   */
  //__read_kernel_mem(...)
  //__write_user_mem(...);

  return 1;
}


/*__read_kernel_mem - read value in kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy

  return 0;
}

/*__write_kernel_mem - write a kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy

  return 0;
}

/*__read_user_mem - read value in user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy

   return 0;
}


/*__write_user_mem - write a user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy

  return 0;
}


/*free_pcb_memphy - collect all memphy of pcb
 *@caller: process that is terminating
 *
 * Gives back to the free lists every frame the process really owns: RAM frames
 * of the present pages and swap frames of the swapped pages. Pages that were
 * never mapped are skipped. Must be called before the PCB is freed.
 */
int free_pcb_memph(struct pcb_t *caller)
{
  struct mm_struct *mm;
  struct vm_area_struct *vma;
  struct pgn_t *pg, *next;
  addr_t pgn, pgn_start, pgn_end, end, pte;

  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL)
    return -1;

  mm = caller->mm;

  pthread_mutex_lock(&mmvm_lock);

  /* Only the pages inside the vm areas can have been mapped */
  for (vma = mm->mmap; vma != NULL; vma = vma->vm_next)
  {
    end = (vma->vm_end > vma->sbrk) ? vma->vm_end : vma->sbrk;
    pgn_start = vma->vm_start / LIB_PGSZ;
    pgn_end = (end + LIB_PGSZ - 1) / LIB_PGSZ;

    for (pgn = pgn_start; pgn < pgn_end; pgn++)
    {
      pte = pte_get_entry(mm, pgn, caller);

      if (PAGING_PAGE_PRESENT(pte))
        MEMPHY_put_freefp(caller->krnl->mram, PAGING_FPN(pte));
      else if (pte & PAGING_PTE_SWAPPED_MASK)
        MEMPHY_put_freefp(caller->krnl->active_mswp, PAGING_SWP(pte));
      else
        continue; /* nothing behind this page */

      /* Forget the mapping so the frame cannot be released twice */
      pte_set_entry(mm, pgn, 0, caller);
    }
  }

  /* Drop the FIFO replacement list */
  for (pg = mm->fifo_pgn; pg != NULL; pg = next)
  {
    next = pg->pg_next;
    free(pg);
  }
  mm->fifo_pgn = NULL;

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}


/*find_victim_page - find victim page
 *@caller: caller
 *@pgn: return page number
 *
 * FIFO policy: the victim is the oldest page, i.e. the tail of the list
 * (enlist_pgn_node pushes new pages at the head).
 */
int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
  struct pgn_t *pg;
  struct pgn_t *prev = NULL;

  if (mm == NULL || retpgn == NULL)
    return -1;

  pg = mm->fifo_pgn;
  if (!pg)
  {
    return -1;
  }

  while (pg->pg_next)
  {
    prev = pg;
    pg = pg->pg_next;
  }
  *retpgn = pg->pgn;

  /* Unlink the tail; a one-element list leaves the FIFO empty */
  if (prev != NULL)
    prev->pg_next = NULL;
  else
    mm->fifo_pgn = NULL;

  free(pg);

  return 0;
}

/*get_free_vmrg_area - get a free vm region
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@size: allocated size
 *
 */
int get_free_vmrg_area(struct pcb_t *caller, int vmaid, int size, struct vm_rg_struct *newrg)
{
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  struct vm_rg_struct *rgit = cur_vma->vm_freerg_list;

  if (rgit == NULL)
    return -1;

  /* Probe unintialized newrg */
  newrg->rg_start = newrg->rg_end = -1;

  /* Traverse on list of free vm region to find a fit space */
  while (rgit != NULL)
  {
    if (rgit->rg_start + size <= rgit->rg_end)
    { /* Current region has enough space */
      newrg->rg_start = rgit->rg_start;
      newrg->rg_end = rgit->rg_start + size;

      /* Update left space in chosen region */
      if (rgit->rg_start + size < rgit->rg_end)
      {
        rgit->rg_start = rgit->rg_start + size;
      }
      else
      { /*Use up all space, remove current node */
        /*Clone next rg node */
        struct vm_rg_struct *nextrg = rgit->rg_next;

        /*Cloning */
        if (nextrg != NULL)
        {
          rgit->rg_start = nextrg->rg_start;
          rgit->rg_end = nextrg->rg_end;

          rgit->rg_next = nextrg->rg_next;

          free(nextrg);
        }
        else
        {                                /*End of free list */
          rgit->rg_start = rgit->rg_end; // dummy, size 0 region
          rgit->rg_next = NULL;
        }
      }
      break;
    }
    else
    {
      rgit = rgit->rg_next; // Traverse next rg
    }
  }

  if (newrg->rg_start == -1) // new region not found
    return -1;

  return 0;
}

// #endif
