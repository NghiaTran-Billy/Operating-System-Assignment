/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

/*
 * PAGING based Memory Management
 * Memory management unit mm/mm.c
 */

#include "mm64.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>

#if defined(MM64)

/*
 * ------------------------------------------------------------------------
 * SV3: 5-level page table (PGD -> P4D -> PUD -> PMD -> PT)
 *
 *  - Virtual address (57 bits): | PGD 9 | P4D 9 | PUD 9 | PMD 9 | PT 9 | offset 12 |
 *  - Every table has 512 entries of addr_t (4096 bytes), so one level
 *    consumes exactly one 4 KB page of host memory.
 *  - mm->pgd is the root table. An entry of PGD/P4D/PUD/PMD holds the
 *    address of the next-level table (0 = not allocated). An entry of PT
 *    is the PTE itself (0 = page never mapped), in the format of mm.h.
 *  - Tables are allocated on demand (sparse): a process using a few
 *    pages near address 0 owns 5 tables, not a 2^45-entry array.
 *  - mm->p4d, mm->pud, mm->pmd, mm->pt keep the FIRST table created at
 *    each level, i.e. the path to the lowest mapped address; they are only
 *    used for debugging output. The real tree is reachable from mm->pgd.
 *  - ALWAYS pass the mm of the process (caller->mm) to the pte_* functions:
 *    a table shared between processes would break isolation.
 * ------------------------------------------------------------------------
 */
#define PGTBL_ENTRIES 512
#define PGTBL_LEVELS 5
#define PGN_BITS 45 /* 57-bit address - 12-bit offset */
#define PTE_FPN_MAX ((addr_t)(PAGING_PTE_FPN_MASK >> PAGING_PTE_FPN_LOBIT))
#define PTE_SWPTYP_MAX ((addr_t)(PAGING_PTE_SWPTYP_MASK >> PAGING_PTE_SWPTYP_LOBIT))
#define PTE_SWPOFF_MAX ((addr_t)(PAGING_PTE_SWPOFF_MASK >> PAGING_PTE_SWPOFF_LOBIT))

/* Protects table creation and the access statistics. Lock order:
 * (libmem mmvm_lock) -> pgtbl_lock; nothing is called while holding it. */
static pthread_mutex_t pgtbl_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long stat_walks = 0; /* page-table walks performed          */
static unsigned long stat_reads = 0; /* table entries read by those walks   */

/*
 * init_pte - Initialize PTE entry
 * @pre : the page exists   @swp : the page lives in swap (else in RAM)
 * Frame number 0 is a valid frame.
 */
int init_pte(addr_t *pte,
             int pre,    // present
             addr_t fpn,    // FPN
             int drt,    // dirty
             int swp,    // swap
             int swptyp, // swap type
             addr_t swpoff) // swap offset
{
  if (pte == NULL)
    return -1;

  *pte = 0;
  if (pre == 0)
    return 0; /* page not mapped at all */

  if (swp == 0)
  { /* Non swap ~ page online */
    if (fpn > PTE_FPN_MAX)
      return -1; /* frame number does not fit in the PTE */

    SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
    SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);
  }
  else
  { /* page swapped: PRESENT stays 0, SWAPPED = 1 */
    if ((addr_t)swptyp > PTE_SWPTYP_MAX || swpoff > PTE_SWPOFF_MAX)
      return -1;

    SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);
    SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
    SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);
  }

  if (drt)
    SETBIT(*pte, PAGING_PTE_DIRTY_MASK);

  return 0;
}


/*
 * get_pd_from_pagenum - Parse address to 5 page directory level
 * @addr  : virtual address
 * @pgd   : page global directory
 * @p4d   : page level directory
 * @pud   : page upper directory
 * @pmd   : page middle directory
 * @pt    : page table
 * Return -1 when the address is not canonical (bits 63..57 must be 0).
 */
int get_pd_from_address(addr_t addr, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
	/* Extract page direactories */
	*pgd = (addr&PAGING64_ADDR_PGD_MASK)>>PAGING64_ADDR_PGD_LOBIT;
	*p4d = (addr&PAGING64_ADDR_P4D_MASK)>>PAGING64_ADDR_P4D_LOBIT;
	*pud = (addr&PAGING64_ADDR_PUD_MASK)>>PAGING64_ADDR_PUD_LOBIT;
	*pmd = (addr&PAGING64_ADDR_PMD_MASK)>>PAGING64_ADDR_PMD_LOBIT;
	*pt = (addr&PAGING64_ADDR_PT_MASK)>>PAGING64_ADDR_PT_LOBIT;

	/* Canonical address layout: the unused high bits must be zero */
	if (addr >> PAGING64_CPU_BUS_WIDTH)
		return -1;

	return 0;
}

/*
 * get_pd_from_pagenum - Parse page number to 5 page directory level
 * @pgn   : pagenumer
 * @pgd   : page global directory
 * @p4d   : page level directory
 * @pud   : page upper directory
 * @pmd   : page middle directory
 * @pt    : page table 
 */
int get_pd_from_pagenum(addr_t pgn, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
	/* Shift the address to get page num and perform the mapping*/
	return get_pd_from_address(pgn << PAGING64_ADDR_PT_SHIFT,
                         pgd,p4d,pud,pmd,pt);
}

/*
 * pgtbl_walk - find the PTE slot of a page
 * @mm     : memory management of the process
 * @pgn    : page number
 * @create : allocate the missing directories on the way (demand allocation)
 * Return a pointer to the PTE, or NULL when the path does not exist
 * (create == 0) or cannot be built.
 * Every call is one "translation"; each table entry read counts as one
 * memory access (the statistics of the report).
 */
static addr_t *pgtbl_walk(struct mm_struct *mm, addr_t pgn, int create)
{
  addr_t idx[PGTBL_LEVELS];
  addr_t **first[4];
  addr_t *tbl, *next;
  int lvl;

  if (mm == NULL || mm->pgd == NULL)
    return NULL;
  if (pgn >> PGN_BITS)
    return NULL; /* not a canonical address */

  get_pd_from_pagenum(pgn, &idx[0], &idx[1], &idx[2], &idx[3], &idx[4]);

  first[0] = &mm->p4d;
  first[1] = &mm->pud;
  first[2] = &mm->pmd;
  first[3] = &mm->pt;

  pthread_mutex_lock(&pgtbl_lock);
  stat_walks++;

  tbl = mm->pgd;
  for (lvl = 0; lvl < 4; lvl++)
  {
    addr_t *ent = &tbl[idx[lvl]];

    stat_reads++;
    if (*ent == 0)
    {
      if (!create)
      {
        pthread_mutex_unlock(&pgtbl_lock);
        return NULL;
      }
      next = calloc(PGTBL_ENTRIES, sizeof(addr_t));
      if (next == NULL)
      {
        pthread_mutex_unlock(&pgtbl_lock);
        return NULL;
      }
      *ent = (addr_t)(uintptr_t)next;
      if (*first[lvl] == NULL)
        *first[lvl] = next;
    }
    tbl = (addr_t *)(uintptr_t)*ent;
  }

  stat_reads++; /* the PT entry */
  pthread_mutex_unlock(&pgtbl_lock);

  return &tbl[idx[4]];
}

static struct mm_struct *pick_mm(struct mm_struct *mm, struct pcb_t *caller)
{
  if (mm != NULL)
    return mm;
  return (caller != NULL) ? caller->mm : NULL;
}

/*
 * pte_set_swap - Set PTE entry for swapped page
 * @caller : process (its mm holds the page table)
 * @pgn    : page number
 * @swptyp : swap type (id of the swap device)
 * @swpoff : swap offset (frame number inside that device)
 * The page is NOT present in RAM afterwards: PRESENT = 0, SWAPPED = 1.
 */
int pte_set_swap(struct pcb_t *caller, addr_t pgn, int swptyp, addr_t swpoff)
{
  addr_t *pte;

  if (caller == NULL || swptyp < 0 || (addr_t)swptyp > PTE_SWPTYP_MAX || swpoff > PTE_SWPOFF_MAX)
    return -1;

  pte = pgtbl_walk(caller->mm, pgn, 1);
  if (pte == NULL)
    return -1;

  *pte = 0; /* drop the old frame number and flags */
  SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);
  SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
  SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);

  return 0;
}

/*
 * pte_set_fpn - Set PTE entry for on-line page
 * @mm    : memory management of the process
 * @pgn   : page number
 * @fpn   : frame page number (FPN)
 * @caller: caller
 * Return -1 when the frame number does not fit in the 13-bit FPN field
 * (max 8191): refusing is safer than silently mapping a wrong frame.
 */
int pte_set_fpn(struct mm_struct *mm, addr_t pgn, addr_t fpn, struct pcb_t *caller)
{
  addr_t *pte;

  if (fpn > PTE_FPN_MAX)
  {
    fprintf(stderr, "[ERROR] %s: frame %lu does not fit in the PTE (max %lu)\n",
            __func__, (unsigned long)fpn, (unsigned long)PTE_FPN_MAX);
    return -1;
  }

  pte = pgtbl_walk(pick_mm(mm, caller), pgn, 1);
  if (pte == NULL)
    return -1;

  if (*pte & PAGING_PTE_SWAPPED_MASK)
    *pte = 0; /* leaving swap: forget the swap type/offset bits */

  SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
  CLRBIT(*pte, PAGING_PTE_SWAPPED_MASK);
  SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);

  return 0;
}


/* Get PTE page table entry
 * @mm     : memory management of the process
 * @pgn    : page number
 * @caller : caller
 * @ret    : page table entry value (0 = page never mapped)
 * Reading never allocates a directory.
 **/
addr_t pte_get_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller)
{
  addr_t *pte = pgtbl_walk(pick_mm(mm, caller), pgn, 0);

  return (pte != NULL) ? *pte : 0;
}

/* Set PTE page table entry
 * @mm      : memory management of the process
 * @pgn     : page number
 * @pte_val : raw PTE value (0 clears the entry)
 * @caller  : caller
 * @ret     : 0 on success
 **/
int pte_set_entry(struct mm_struct *mm, addr_t pgn, uint64_t pte_val, struct pcb_t *caller)
{
  /* Clearing a page that was never mapped must not allocate directories */
  addr_t *pte = pgtbl_walk(pick_mm(mm, caller), pgn, pte_val != 0);

  if (pte == NULL)
    return (pte_val == 0) ? 0 : -1;

  *pte = pte_val;
  return 0;
}

/*
 * vmap_pgd_memset - map a range of page at aligned address
 */
int vmap_pgd_memset(struct pcb_t *caller,           // process call
                    addr_t addr,                       // start address which is aligned to pagesz
                    int pgnum)                      // num of mapping page
{
  //int pgit = 0;
  //uint64_t pattern = 0xdeadbeef;

  /* TODO memset the page table with given pattern
   */

  return 0;
}

/*
 * vmap_page_range - map a range of page at aligned address
 */
addr_t vmap_page_range(struct pcb_t *caller,           // process call
                    addr_t addr,                       // start address which is aligned to pagesz // start
                    int pgnum,                      // num of mapping page
                    struct framephy_struct *frames, // list of the mapped frames
                    struct vm_rg_struct *ret_rg)    // return mapped region, the real mapped fp
{                                                   // no guarantee all given pages are mapped
//struct framephy_struct *fpit;
//int pgit = 0;
//addr_t pgn;

  /* TODO: update the rg_end and rg_start of ret_rg 
  //ret_rg->rg_end =  ....
  //ret_rg->rg_start = ...
  //ret_rg->vmaid = ...
  */

  /* TODO map range of frame to address space
   *      [addr to addr + pgnum*PAGING_PAGESZ
   *      in page table caller->krnl->mm->pgd,
   *                    caller->krnl->mm->pud...
   *                    ...
   */

  /* Tracking for later page replacement activities (if needed)
   * Enqueue new usage page */
  //enlist_pgn_node(&caller->krnl->mm->fifo_pgn, pgn64 + pgit);

  return 0;
}

/*
 * alloc_pages_range - allocate req_pgnum of frame in ram
 * @caller    : caller
 * @req_pgnum : request page num
 * @frm_lst   : frame list
 */

addr_t alloc_pages_range(struct pcb_t *caller, int req_pgnum, struct framephy_struct **frm_lst)
{
  //addr_t fpn;
  //int pgit;
  //struct framephy_struct *newfp_str = NULL;
  //struct framephy_struct **cur = frm_lst;

  /* TODO: allocate the page 
  //caller-> ...
  //frm_lst-> ...
  */


/*
  for (pgit = 0; pgit < req_pgnum; pgit++)
  {
    // TODO: allocate the page 
    if (MEMPHY_get_freefp(caller->mram, &fpn) == 0)
    {
      newfp_str->fpn = fpn;
    }
    else
    { // TODO: ERROR CODE of obtaining somes but not enough frames
    }
  }
*/


  /* End TODO */

  return 0;
}

/*
 * vm_map_ram - do the mapping all vm are to ram storage device
 * @caller    : caller
 * @astart    : vm area start
 * @aend      : vm area end
 * @mapstart  : start mapping point
 * @incpgnum  : number of mapped page
 * @ret_rg    : returned region
 */
addr_t vm_map_ram(struct pcb_t *caller, addr_t astart, addr_t aend, addr_t mapstart, int incpgnum, struct vm_rg_struct *ret_rg)
{
  struct framephy_struct *frm_lst = NULL;
  addr_t ret_alloc = 0;

  /*@bksysnet: author provides a feasible solution of getting frames
   *FATAL logic in here, wrong behaviour if we have not enough page
   *i.e. we request 1000 frames meanwhile our RAM has size of 3 frames
   *Don't try to perform that case in this simple work, it will result
   *in endless procedure of swap-off to get frame and we have not provide
   *duplicate control mechanism, keep it simple
   */
  ret_alloc = alloc_pages_range(caller, incpgnum, &frm_lst);

  if (ret_alloc < 0 && ret_alloc != -3000)
    return -1;

  /* Out of memory */
  if (ret_alloc == -3000)
  {
    return -1;
  }

  /* it leaves the case of memory is enough but half in ram, half in swap
   * do the swaping all to swapper to get the all in ram */
   vmap_page_range(caller, mapstart, incpgnum, frm_lst, ret_rg);

  return 0;
}

/* Swap copy content page from source frame to destination frame
 * @mpsrc  : source memphy
 * @srcfpn : source physical page number (FPN)
 * @mpdst  : destination memphy
 * @dstfpn : destination physical page number (FPN)
 **/
int __swap_cp_page(struct memphy_struct *mpsrc, addr_t srcfpn,
                   struct memphy_struct *mpdst, addr_t dstfpn)
{
  int cellidx;
  addr_t addrsrc, addrdst;

  if (mpsrc == NULL || mpdst == NULL)
    return -1;

  /* Both frames must lie completely inside their device */
  if ((srcfpn + 1) * PAGING64_PAGESZ > (addr_t)mpsrc->maxsz ||
      (dstfpn + 1) * PAGING64_PAGESZ > (addr_t)mpdst->maxsz)
    return -1;

  for (cellidx = 0; cellidx < PAGING64_PAGESZ; cellidx++)
  {
    addrsrc = srcfpn * PAGING64_PAGESZ + cellidx;
    addrdst = dstfpn * PAGING64_PAGESZ + cellidx;

    BYTE data;
    if (MEMPHY_read(mpsrc, addrsrc, &data) != 0)
      return -1;
    if (MEMPHY_write(mpdst, addrdst, data) != 0)
      return -1;
  }

  return 0;
}

/*
 *Initialize a empty Memory Management instance
 * @mm:     self mm
 * @caller: mm owner (may be NULL for the kernel mm)
 * Every field is set here: the struct may come straight from malloc().
 */
int init_mm(struct mm_struct *mm, struct pcb_t *caller)
{
  struct vm_area_struct *vma0;
  struct vm_rg_struct *first_rg;

  (void)caller;
  if (mm == NULL)
    return -1;

  /* page directories, symbol table, FIFO list, kmem pool: all empty */
  memset(mm, 0, sizeof(struct mm_struct));

  /* top level of the page table: PGD with 512 empty entries.
   * The lower levels are created on demand by pgtbl_walk(). */
  mm->pgd = calloc(PGTBL_ENTRIES, sizeof(addr_t));
  if (mm->pgd == NULL)
    return -1;

  vma0 = calloc(1, sizeof(struct vm_area_struct));
  if (vma0 == NULL)
  {
    free(mm->pgd);
    mm->pgd = NULL;
    return -1;
  }

  /* By default the owner comes with at least one vma */
  vma0->vm_id = 0;
  vma0->vm_start = 0;
  vma0->vm_end = vma0->vm_start;
  vma0->sbrk = vma0->vm_start;
  first_rg = init_vm_rg(vma0->vm_start, vma0->vm_end);
  enlist_vm_rg_node(&vma0->vm_freerg_list, first_rg);

  vma0->vm_next = NULL;

  /* Point vma owner backward */
  vma0->vm_mm = mm;

  mm->mmap = vma0;

  return 0;
}

/* free_level - release a table and, recursively, every table below it */
static void free_level(addr_t *tbl, int lvl)
{
  int i;

  if (tbl == NULL)
    return;

  if (lvl < PGTBL_LEVELS - 1)
    for (i = 0; i < PGTBL_ENTRIES; i++)
      if (tbl[i] != 0)
        free_level((addr_t *)(uintptr_t)tbl[i], lvl + 1);

  free(tbl);
}

/*
 * free_pgtbl - release every page-table table of a process
 * Call it when the process ends, after free_pcb_memph() gave the frames back.
 */
void free_pgtbl(struct mm_struct *mm)
{
  if (mm == NULL)
    return;

  pthread_mutex_lock(&pgtbl_lock);
  free_level(mm->pgd, 0);
  mm->pgd = mm->p4d = mm->pud = mm->pmd = mm->pt = NULL;
  pthread_mutex_unlock(&pgtbl_lock);
}

struct vm_rg_struct *init_vm_rg(addr_t rg_start, addr_t rg_end)
{
  struct vm_rg_struct *rgnode = malloc(sizeof(struct vm_rg_struct));

  rgnode->rg_start = rg_start;
  rgnode->rg_end = rg_end;
  rgnode->rg_next = NULL;

  return rgnode;
}

int enlist_vm_rg_node(struct vm_rg_struct **rglist, struct vm_rg_struct *rgnode)
{
  rgnode->rg_next = *rglist;
  *rglist = rgnode;

  return 0;
}

int enlist_pgn_node(struct pgn_t **plist, addr_t pgn)
{
  struct pgn_t *pnode = malloc(sizeof(struct pgn_t));

  pnode->pgn = pgn;
  pnode->pg_next = *plist;
  *plist = pnode;

  return 0;
}

int print_list_fp(struct framephy_struct *ifp)
{
  struct framephy_struct *fp = ifp;

  printf("print_list_fp: ");
  if (fp == NULL) { printf("NULL list\n"); return -1;}
  printf("\n");
  while (fp != NULL)
  {
    printf("fp[" FORMAT_ADDR "]\n", fp->fpn);
    fp = fp->fp_next;
  }
  printf("\n");
  return 0;
}

int print_list_rg(struct vm_rg_struct *irg)
{
  struct vm_rg_struct *rg = irg;

  printf("print_list_rg: ");
  if (rg == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (rg != NULL)
  {
    printf("rg[" FORMAT_ADDR "->"  FORMAT_ADDR "]\n", rg->rg_start, rg->rg_end);
    rg = rg->rg_next;
  }
  printf("\n");
  return 0;
}

int print_list_vma(struct vm_area_struct *ivma)
{
  struct vm_area_struct *vma = ivma;

  printf("print_list_vma: ");
  if (vma == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (vma != NULL)
  {
    printf("va[" FORMAT_ADDR "->" FORMAT_ADDR "]\n", vma->vm_start, vma->vm_end);
    vma = vma->vm_next;
  }
  printf("\n");
  return 0;
}

int print_list_pgn(struct pgn_t *ip)
{
  printf("print_list_pgn: ");
  if (ip == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (ip != NULL)
  {
    printf("va[" FORMAT_ADDR "]-\n", ip->pgn);
    ip = ip->pg_next;
  }
  printf("\n");
  return 0;
}

/* count_level - statistics of one table and everything below it */
static void count_level(addr_t *tbl, int lvl, addr_t base, struct pgtbl_stats *st)
{
  int i;

  st->tables[lvl]++;
  for (i = 0; i < PGTBL_ENTRIES; i++)
  {
    addr_t pgn = (base << 9) | (addr_t)i;

    if (tbl[i] == 0)
      continue;
    if (lvl < PGTBL_LEVELS - 1)
      count_level((addr_t *)(uintptr_t)tbl[i], lvl + 1, pgn, st);
    else
    {
      st->mapped_pages++;
      if (pgn > st->max_pgn)
        st->max_pgn = pgn;
    }
  }
}

/*
 * pgtbl_get_stats - how much memory the multi-level page table really uses
 * and what one flat table covering the same pages would cost
 */
int pgtbl_get_stats(struct mm_struct *mm, struct pgtbl_stats *st)
{
  int lvl;
  addr_t total = 0;

  if (st == NULL)
    return -1;
  memset(st, 0, sizeof(*st));
  if (mm == NULL || mm->pgd == NULL)
    return -1;

  pthread_mutex_lock(&pgtbl_lock);
  count_level(mm->pgd, 0, 0, st);
  pthread_mutex_unlock(&pgtbl_lock);

  for (lvl = 0; lvl < PGTBL_LEVELS; lvl++)
    total += st->tables[lvl];
  st->table_bytes = total * PGTBL_ENTRIES * sizeof(addr_t);
  st->flat_bytes = st->mapped_pages ? (st->max_pgn + 1) * sizeof(addr_t) : 0;

  return 0;
}

/* pgtbl_access_stats - global counters of all page-table walks so far */
void pgtbl_access_stats(unsigned long *walks, unsigned long *reads)
{
  pthread_mutex_lock(&pgtbl_lock);
  if (walks != NULL)
    *walks = stat_walks;
  if (reads != NULL)
    *reads = stat_reads;
  pthread_mutex_unlock(&pgtbl_lock);
}

/* print_pgtbl_stats - statistics of one process, for the report */
int print_pgtbl_stats(struct pcb_t *caller)
{
  struct pgtbl_stats st;
  unsigned long walks, reads;

  if (caller == NULL || pgtbl_get_stats(caller->mm, &st) != 0)
    return -1;

  pgtbl_access_stats(&walks, &reads);
  printf("=== PAGE TABLE STATISTICS (PID %u) ===\n", caller->pid);
  printf(" tables  PGD=%lu P4D=%lu PUD=%lu PMD=%lu PT=%lu\n",
         (unsigned long)st.tables[0], (unsigned long)st.tables[1], (unsigned long)st.tables[2],
         (unsigned long)st.tables[3], (unsigned long)st.tables[4]);
  printf(" page-table memory : %lu bytes (5-level, on demand)\n", (unsigned long)st.table_bytes);
  printf(" mapped pages      : %lu (highest page number 0x%lx)\n",
         (unsigned long)st.mapped_pages, (unsigned long)st.max_pgn);
  printf(" one flat table    : %lu bytes for the same range\n", (unsigned long)st.flat_bytes);
  if (walks > 0)
    printf(" memory accesses   : %.2f table reads per translation (+1 for the data itself)\n",
           (double)reads / (double)walks);
  printf("======================================\n");
  return 0;
}

/*
 * print_pgtbl - print the page directories on the path of address [start]
 * The libmem layer calls print_pgtbl(proc, 0, -1) after every operation.
 * One line: the root table and the P4D, PUD, PMD tables reached by [start]
 * (0 where the table does not exist yet).
 */
int print_pgtbl(struct pcb_t *caller, addr_t start, addr_t end)
{
  struct mm_struct *mm;
  addr_t idx[PGTBL_LEVELS];
  addr_t *tbl[4];
  int lvl;

  (void)end;
  if (caller == NULL || (mm = caller->mm) == NULL)
    return -1;

  tbl[0] = mm->pgd;
  tbl[1] = tbl[2] = tbl[3] = NULL;

  if (get_pd_from_address(start, &idx[0], &idx[1], &idx[2], &idx[3], &idx[4]) == 0)
  {
    for (lvl = 0; lvl < 3; lvl++)
      if (tbl[lvl] != NULL && tbl[lvl][idx[lvl]] != 0)
        tbl[lvl + 1] = (addr_t *)(uintptr_t)tbl[lvl][idx[lvl]];
      else
        break;
  }

  printf(" PDG=%lx P4g=%lx PUD=%lx PMD=%lx\n",
         (unsigned long)(uintptr_t)tbl[0], (unsigned long)(uintptr_t)tbl[1],
         (unsigned long)(uintptr_t)tbl[2], (unsigned long)(uintptr_t)tbl[3]);

  return 0;
}

/* entries_range - print the mapped pages of [lo, hi] (page numbers) */
static unsigned long entries_range(addr_t *tbl, int lvl, addr_t base, addr_t lo, addr_t hi)
{
  unsigned long printed = 0;
  int shift = 9 * (PGTBL_LEVELS - 1 - lvl);
  int i;

  for (i = 0; i < PGTBL_ENTRIES; i++)
  {
    addr_t cb = (base << 9) | (addr_t)i;
    addr_t span_lo = cb << shift;
    addr_t span_hi = span_lo + (((addr_t)1 << shift) - 1);

    if (tbl[i] == 0 || span_hi < lo || span_lo > hi)
      continue;

    if (lvl < PGTBL_LEVELS - 1)
    {
      printed += entries_range((addr_t *)(uintptr_t)tbl[i], lvl + 1, cb, lo, hi);
      continue;
    }

    if (tbl[i] & PAGING_PTE_SWAPPED_MASK)
      printf("  pgn 0x%lx (va 0x%lx) -> SWAP dev %lu frame %lu   [pte 0x%lx]\n",
             (unsigned long)cb, (unsigned long)(cb << PAGING64_ADDR_PT_SHIFT),
             (unsigned long)PAGING_PTE_SWPTYP(tbl[i]), (unsigned long)PAGING_PTE_SWP(tbl[i]),
             (unsigned long)tbl[i]);
    else
      printf("  pgn 0x%lx (va 0x%lx) -> RAM frame %lu%s   [pte 0x%lx]\n",
             (unsigned long)cb, (unsigned long)(cb << PAGING64_ADDR_PT_SHIFT),
             (unsigned long)PAGING_FPN(tbl[i]),
             (tbl[i] & PAGING_PTE_DIRTY_MASK) ? " dirty" : "", (unsigned long)tbl[i]);
    printed++;
  }

  return printed;
}

/*
 * print_pgtbl_entries - list every mapped page between two addresses.
 * Only the existing tables are visited, so [0, -1] is cheap.
 * Return the number of pages printed.
 */
int print_pgtbl_entries(struct pcb_t *caller, addr_t start, addr_t end)
{
  addr_t lo, hi, maxpgn = ((addr_t)1 << PGN_BITS) - 1;

  if (caller == NULL || caller->mm == NULL || caller->mm->pgd == NULL)
    return -1;

  lo = start >> PAGING64_ADDR_PT_SHIFT;
  hi = end >> PAGING64_ADDR_PT_SHIFT;
  if (hi > maxpgn)
    hi = maxpgn;
  if (lo > hi)
    return 0;

  return (int)entries_range(caller->mm->pgd, 0, 0, lo, hi);
}

#endif  //def MM64
