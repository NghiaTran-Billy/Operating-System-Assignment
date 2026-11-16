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

/* ------------------------------------------------------------------------
 * SV2 helpers: page size, PTE state tests, swap device lookup and the
 * system-call wrappers used by the paging code below.
 *
 * PTE convention used by this module (must be shared with mm64.c):
 *   - in RAM   : PRESENT=1, SWAPPED=0, FPN field holds the RAM frame
 *   - in SWAP  : SWAPPED=1 (PRESENT is ignored), SWPTYP/SWPOFF hold the
 *                swap device id and the frame inside that device
 *   - unmapped : PTE == 0, accessing it is an invalid memory access
 * ------------------------------------------------------------------------ */
#ifdef MM64
#define VM_PAGESZ PAGING64_PAGESZ
#else
#define VM_PAGESZ PAGING_PAGESZ
#endif
#define VM_PGN(a) ((addr_t)(a) / VM_PAGESZ)
#define VM_OFF(a) ((addr_t)(a) % VM_PAGESZ)

static unsigned long swap_in_cnt = 0;  /* statistics, protected by mmvm_lock */
static unsigned long swap_out_cnt = 0;

static int pte_in_ram(addr_t pte)
{
  return ((pte & PAGING_PTE_PRESENT_MASK) && !(pte & PAGING_PTE_SWAPPED_MASK));
}

static int pte_in_swap(addr_t pte)
{
  return ((pte & PAGING_PTE_SWAPPED_MASK) != 0);
}

/* Only the active swap device can be reached (krnl->mswp[] is not usable:
 * os.c stores the address of the array of structs, not an array of pointers) */
static struct memphy_struct *get_swap_dev(struct krnl_t *krnl, uint32_t swptyp)
{
  if (swptyp == krnl->active_mswp_id)
    return krnl->active_mswp;
  return NULL;
}

/* sys_memmap / SYSMEM_SWP_OP : dir 0 = RAM->SWAP, 1 = SWAP->RAM */
static int sys_swap_page(struct pcb_t *caller, addr_t ramfpn, addr_t swpfpn, int dir)
{
  struct sc_regs regs;

  memset(&regs, 0, sizeof(regs));
  regs.a1 = SYSMEM_SWP_OP;
  regs.a2 = ramfpn;
  regs.a3 = swpfpn;
  regs.a4 = dir;
  return _syscall(caller->krnl, caller->pid, 17, &regs); /* SYSCALL 17 sys_memmap */
}

/* sys_memmap / SYSMEM_IO_READ : read one byte of MEMRAM */
static int sys_io_read(struct pcb_t *caller, addr_t phyaddr, BYTE *data)
{
  struct sc_regs regs;
  int ret;

  memset(&regs, 0, sizeof(regs));
  regs.a1 = SYSMEM_IO_READ;
  regs.a2 = phyaddr;
  ret = _syscall(caller->krnl, caller->pid, 17, &regs);
  *data = (BYTE)(regs.a3 & 0xFF);
  return ret;
}

/* sys_memmap / SYSMEM_IO_WRITE : write one byte of MEMRAM */
static int sys_io_write(struct pcb_t *caller, addr_t phyaddr, BYTE value)
{
  struct sc_regs regs;

  memset(&regs, 0, sizeof(regs));
  regs.a1 = SYSMEM_IO_WRITE;
  regs.a2 = phyaddr;
  regs.a3 = (unsigned char)value;
  return _syscall(caller->krnl, caller->pid, 17, &regs);
}

/* Number of pages brought in / pushed out so far (for tests and the report) */
unsigned long libmem_swap_count(int dir)
{
  unsigned long cnt;

  pthread_mutex_lock(&mmvm_lock);
  cnt = (dir == 0) ? swap_out_cnt : swap_in_cnt;
  pthread_mutex_unlock(&mmvm_lock);
  return cnt;
}

/*pg_getpage - get the page in ram
 *@mm: memory region (the process mm, caller->mm, is the one really used)
 *@pgn: page number
 *@fpn: return the RAM frame number holding the page
 *@caller: caller
 *
 *Return 0 on success, -1 for an invalid page or when no frame can be found.
 *The caller must hold mmvm_lock.
 */
int pg_getpage(struct mm_struct *mm, addr_t pgn, addr_t *fpn, struct pcb_t *caller)
{
  struct krnl_t *krnl;
  struct mm_struct *pmm;
  struct memphy_struct *swpdev;
  addr_t pte, tgtfpn, tgtswp;
  uint32_t swptyp;

  (void)mm;
  if (caller == NULL || fpn == NULL)
    return -1;
  krnl = caller->krnl;
  pmm = caller->mm;
  if (krnl == NULL || pmm == NULL)
    return -1;

  pte = pte_get_entry(pmm, pgn, caller);

  if (pte_in_ram(pte))
  { /* Page is already online */
    *fpn = PAGING_FPN(pte);
    return 0;
  }

  if (!pte_in_swap(pte))
    return -1; /* Page was never mapped: invalid memory access */

  /* Page is in MEMSWP: make it actively living in MEMRAM */
  swptyp = PAGING_PTE_SWPTYP(pte);
  tgtswp = PAGING_PTE_SWP(pte);
  swpdev = get_swap_dev(krnl, swptyp);
  if (swpdev == NULL)
    return -1;

  /* Initialize the target frame storing our page */
  if (MEMPHY_get_freefp(krnl->mram, &tgtfpn) != 0)
  {
    /* MEMRAM is full: the oldest page of this process (FIFO) goes to swap */
    addr_t vicpgn, vicpte, vicfpn, vicswp;

    /* Get free frame in MEMSWP for the victim */
    if (MEMPHY_get_freefp(krnl->active_mswp, &vicswp) != 0)
      return -1; /* swap is full as well */

    /* Find victim page */
    if (find_victim_page(pmm, &vicpgn) != 0)
    {
      MEMPHY_put_freefp(krnl->active_mswp, vicswp);
      return -1; /* this process owns no page in RAM to give away */
    }

    vicpte = pte_get_entry(pmm, vicpgn, caller);
    vicfpn = PAGING_FPN(vicpte);

    /* Copy the victim frame to swap: SWP(vicfpn <--> swpfpn), SYSCALL 17 */
    if (sys_swap_page(caller, vicfpn, vicswp, 0) != 0)
    {
      enlist_pgn_node(&pmm->fifo_pgn, vicpgn); /* victim stays in RAM */
      MEMPHY_put_freefp(krnl->active_mswp, vicswp);
      return -1;
    }
    swap_out_cnt++;

    /* Update page table: the victim now lives in swap */
    pte_set_swap(caller, vicpgn, krnl->active_mswp_id, vicswp);

    /* The frame of the victim is the target frame */
    tgtfpn = vicfpn;
  }

  /* Copy the target page from swap into the target frame */
  if (sys_swap_page(caller, tgtfpn, tgtswp, 1) != 0)
  {
    MEMPHY_put_freefp(krnl->mram, tgtfpn); /* give the frame back */
    return -1;
  }
  swap_in_cnt++;

  /* The swap frame of the target page is free again */
  MEMPHY_put_freefp(swpdev, tgtswp);

  /* Update its online status of the target page (clear the old swap bits) */
  pte_set_entry(pmm, pgn, 0, caller);
  pte_set_fpn(pmm, pgn, tgtfpn, caller);

  /* Track the page for later page replacement */
  enlist_pgn_node(&pmm->fifo_pgn, pgn);

  *fpn = tgtfpn;

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
  addr_t pgn = VM_PGN(addr);
  addr_t off = VM_OFF(addr);
  addr_t fpn;
  addr_t phyaddr;

  if (data == NULL)
    return -1;

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  phyaddr = fpn * VM_PAGESZ + off;

  /* MEMPHY READ through SYSCALL 17 sys_memmap with SYSMEM_IO_READ */
  return sys_io_read(caller, phyaddr, data);
}

/*pg_setval - write value to given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_setval(struct mm_struct *mm, addr_t addr, BYTE value, struct pcb_t *caller)
{
  addr_t pgn = VM_PGN(addr);
  addr_t off = VM_OFF(addr);
  addr_t fpn;
  addr_t phyaddr;

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  phyaddr = fpn * VM_PAGESZ + off;

  /* MEMPHY WRITE through SYSCALL 17 sys_memmap with SYSMEM_IO_WRITE */
  return sys_io_write(caller, phyaddr, value);
}

/* Check that [rg_start + offset] lies inside the region [rg_start, rg_end) */
static int region_contains(struct vm_rg_struct *rg, addr_t offset)
{
  if (rg == NULL)
    return 0;
  if (rg->rg_start >= rg->rg_end)
    return 0; /* empty or already freed region */
  if (rg->vmaid == KRNL_RG_VMAID)
    return 0; /* kernel region: user instructions must not touch it */
  return (offset < rg->rg_end - rg->rg_start);
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

  pthread_mutex_lock(&mmvm_lock);

  currg = get_symrg_byid(caller->mm, rgid);
  cur_vma = get_vma_by_num(caller->mm, vmaid);

  /* Invalid memory identify */
  if (cur_vma == NULL || !region_contains(currg, offset))
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  ret = pg_getval(caller->mm, currg->rg_start + offset, data, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return (ret == 0) ? 0 : -1;
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

  if (val == 0 && destination != NULL)
    *destination = (unsigned char)data;
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

  pthread_mutex_lock(&mmvm_lock);

  currg = get_symrg_byid(caller->mm, rgid);
  cur_vma = get_vma_by_num(caller->mm, vmaid);

  /* Invalid memory identify */
  if (cur_vma == NULL || !region_contains(currg, offset))
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  ret = pg_setval(caller->mm, currg->rg_start + offset, value, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return (ret == 0) ? 0 : -1;
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


/* ------------------------------------------------------------------------
 * SV3: kernel memory and user <-> kernel copies
 *
 * Contract shared with libkmem_malloc / __kmalloc (kernel allocation):
 *  - A KERNEL region is a variable of the process stored in its symbol
 *    table caller->mm->symrgtbl[rgid] with vmaid == KRNL_RG_VMAID.
 *  - rg_start / rg_end of a kernel region are PHYSICAL byte addresses in
 *    MEMRAM (kmalloc gives physically contiguous memory, no page table).
 *  - A USER region is reached through the page table (virtual addresses,
 *    swap handled by pg_getpage).
 *  - User instructions (READ/WRITE) never touch a kernel region: only
 *    copy_from_user / copy_to_user and the __*_kernel_mem helpers do.
 * All accesses to RAM go through SYSCALL 17 (sys_memmap IO read/write).
 * ------------------------------------------------------------------------ */

/* sys_memmap / SYSMEM_IO_READ on a physical address of MEMRAM */
static int krnl_io_read(struct pcb_t *caller, addr_t phyaddr, BYTE *data)
{
  struct sc_regs regs;
  int ret;

  memset(&regs, 0, sizeof(regs));
  regs.a1 = SYSMEM_IO_READ;
  regs.a2 = phyaddr;
  ret = _syscall(caller->krnl, caller->pid, 17, &regs);
  *data = (BYTE)(regs.a3 & 0xFF);
  return ret;
}

/* sys_memmap / SYSMEM_IO_WRITE on a physical address of MEMRAM */
static int krnl_io_write(struct pcb_t *caller, addr_t phyaddr, BYTE value)
{
  struct sc_regs regs;

  memset(&regs, 0, sizeof(regs));
  regs.a1 = SYSMEM_IO_WRITE;
  regs.a2 = phyaddr;
  regs.a3 = (unsigned char)value;
  return _syscall(caller->krnl, caller->pid, 17, &regs);
}

/* Is [offset, offset + size) inside region [rg] of the wanted kind? */
static int rg_fits(struct vm_rg_struct *rg, int want_kernel, addr_t offset, addr_t size)
{
  addr_t len;

  if (rg == NULL || rg->rg_start >= rg->rg_end)
    return 0; /* not allocated, empty or freed */
  if ((rg->vmaid == KRNL_RG_VMAID) != (want_kernel != 0))
    return 0; /* wrong kind of region */

  len = rg->rg_end - rg->rg_start;
  return (size <= len && offset <= len - size);
}

/* The *_nl helpers expect mmvm_lock to be held by the caller */
static int kmem_read_nl(struct pcb_t *caller, int rgid, addr_t offset, BYTE *data)
{
  struct vm_rg_struct *rg = get_symrg_byid(caller->mm, rgid);

  if (data == NULL || !rg_fits(rg, 1, offset, 1))
    return -1;
  return krnl_io_read(caller, rg->rg_start + offset, data);
}

static int kmem_write_nl(struct pcb_t *caller, int rgid, addr_t offset, BYTE value)
{
  struct vm_rg_struct *rg = get_symrg_byid(caller->mm, rgid);

  if (!rg_fits(rg, 1, offset, 1))
    return -1;
  return krnl_io_write(caller, rg->rg_start + offset, value);
}

static int umem_read_nl(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  struct vm_rg_struct *rg = get_symrg_byid(caller->mm, rgid);

  if (get_vma_by_num(caller->mm, vmaid) == NULL || !rg_fits(rg, 0, offset, 1))
    return -1;
  return pg_getval(caller->mm, rg->rg_start + offset, data, caller);
}

static int umem_write_nl(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  struct vm_rg_struct *rg = get_symrg_byid(caller->mm, rgid);

  if (get_vma_by_num(caller->mm, vmaid) == NULL || !rg_fits(rg, 0, offset, 1))
    return -1;
  return pg_setval(caller->mm, rg->rg_start + offset, value, caller);
}

/*libkmem_copy_from_user - copy [size] bytes from a user region to a kernel region
 *@caller: caller
 *@source: user region id         @destination: kernel region id
 *@offset: start offset in BOTH regions    @size: number of bytes
 *Return 0 on success, -1 on any error. Nothing is copied when a region is
 *missing, has the wrong kind, or is too small (checked before the copy).
 */
int libkmem_copy_from_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  struct vm_rg_struct *src, *dst;
  uint32_t i;
  BYTE b;
  int ret = -1;

  if (caller == NULL || caller->mm == NULL)
    return -1;
  if (size == 0)
    return 0;

  pthread_mutex_lock(&mmvm_lock);

  src = get_symrg_byid(caller->mm, source);
  dst = get_symrg_byid(caller->mm, destination);
  if (!rg_fits(src, 0, offset, size) || !rg_fits(dst, 1, offset, size))
    goto out;

  for (i = 0; i < size; i++)
  {
    if (umem_read_nl(caller, 0, source, (addr_t)offset + i, &b) != 0)
      goto out;
    if (kmem_write_nl(caller, destination, (addr_t)offset + i, b) != 0)
      goto out;
  }
  ret = 0;

out:
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/*libkmem_copy_to_user - copy [size] bytes from a kernel region to a user region
 *Same rules as libkmem_copy_from_user, source is the kernel region.
 */
int libkmem_copy_to_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  struct vm_rg_struct *src, *dst;
  uint32_t i;
  BYTE b;
  int ret = -1;

  if (caller == NULL || caller->mm == NULL)
    return -1;
  if (size == 0)
    return 0;

  pthread_mutex_lock(&mmvm_lock);

  src = get_symrg_byid(caller->mm, source);
  dst = get_symrg_byid(caller->mm, destination);
  if (!rg_fits(src, 1, offset, size) || !rg_fits(dst, 0, offset, size))
    goto out;

  for (i = 0; i < size; i++)
  {
    if (kmem_read_nl(caller, source, (addr_t)offset + i, &b) != 0)
      goto out;
    if (umem_write_nl(caller, 0, destination, (addr_t)offset + i, b) != 0)
      goto out;
  }
  ret = 0;

out:
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}


/*__read_kernel_mem - read value in kernel region memory
 *@caller: caller
 *@vmaid: ID vm area (unused: kernel regions are physical)
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 *Return 0 on success, -1 for an invalid region or offset.
 */
int __read_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  int ret;

  (void)vmaid;
  if (caller == NULL || caller->mm == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);
  ret = kmem_read_nl(caller, rgid, offset, data);
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/*__write_kernel_mem - write a kernel region memory
 *Same arguments as __read_kernel_mem, [value] is the byte written.
 */
int __write_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  int ret;

  (void)vmaid;
  if (caller == NULL || caller->mm == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);
  ret = kmem_write_nl(caller, rgid, offset, value);
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/*__read_user_mem - read value in user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 *Refuses kernel regions. Same as __read but without the debug output.
 */
int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  int ret;

  if (caller == NULL || caller->mm == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);
  ret = umem_read_nl(caller, vmaid, rgid, offset, data);
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}


/*__write_user_mem - write a user region memory
 *Same arguments as __read_user_mem, [value] is the byte written.
 */
int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  int ret;

  if (caller == NULL || caller->mm == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);
  ret = umem_write_nl(caller, vmaid, rgid, offset, value);
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}


/*free_pcb_memphy - collect all memphy of pcb
 *Give back to MEMRAM / MEMSWP every frame used by the process and clear
 *its page-table entries. Must be called once, when the process terminates.
 *@caller: the finished process
 *Return 0 on success, -1 on a bad argument.
 */
int free_pcb_memph(struct pcb_t *caller)
{
  struct krnl_t *krnl;
  struct mm_struct *mm;
  struct vm_area_struct *vma;
  struct pgn_t *pg, *nextpg;
  struct memphy_struct *swpdev;
  addr_t pgn, firstpgn, lastpgn, pte;

  if (caller == NULL || (krnl = caller->krnl) == NULL || (mm = caller->mm) == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);

  /* Only pages below the break pointer of a vm area can be mapped */
  for (vma = mm->mmap; vma != NULL; vma = vma->vm_next)
  {
    firstpgn = VM_PGN(vma->vm_start);
    lastpgn = (vma->sbrk + VM_PAGESZ - 1) / VM_PAGESZ; /* exclusive */

    for (pgn = firstpgn; pgn < lastpgn; pgn++)
    {
      pte = pte_get_entry(mm, pgn, caller);

      if (pte_in_ram(pte))
        MEMPHY_put_freefp(krnl->mram, PAGING_FPN(pte));
      else if (pte_in_swap(pte))
      {
        swpdev = get_swap_dev(krnl, PAGING_PTE_SWPTYP(pte));
        if (swpdev != NULL)
          MEMPHY_put_freefp(swpdev, PAGING_PTE_SWP(pte));
      }
      else
        continue; /* never mapped */

      pte_set_entry(mm, pgn, 0, caller);
    }
  }

  /* Forget the page-replacement list */
  for (pg = mm->fifo_pgn; pg != NULL; pg = nextpg)
  {
    nextpg = pg->pg_next;
    free(pg);
  }
  mm->fifo_pgn = NULL;

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}


/*find_victim_page - find victim page (FIFO: the page mapped longest ago)
 *@mm: memory management of the process
 *@retpgn: return page number
 *The newest page is at the head of fifo_pgn, so the victim is the tail.
 *Return 0 on success, -1 when the list is empty.
 */
int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
  struct pgn_t *pg, *prev = NULL;

  if (mm == NULL || retpgn == NULL)
    return -1;

  pg = mm->fifo_pgn;
  if (pg == NULL)
    return -1;

  while (pg->pg_next != NULL)
  {
    prev = pg;
    pg = pg->pg_next;
  }
  *retpgn = pg->pgn;

  if (prev != NULL)
    prev->pg_next = NULL;
  else
    mm->fifo_pgn = NULL; /* it was the only element */

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
