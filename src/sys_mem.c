/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "os-mm.h"
#include "syscall.h"
#include "libmem.h"
#include "queue.h"
#include <stdlib.h>
#include <pthread.h>
#ifdef MM64
#include "mm64.h"
#else
#include "mm.h"
#endif

//typedef char BYTE;
static pthread_mutex_t queue_lock;
int __sys_memmap(struct krnl_t *krnl, uint32_t pid, struct sc_regs* regs)
{
    if (krnl == NULL || krnl->running_list == NULL || regs == NULL) {
      return -1;
   }
   int memop = regs->a1;
   BYTE value;
   struct pcb_t *caller = NULL; //con trỏ chỉ pcb đang gọi syscall
   pthread_mutex_lock(&queue_lock);
   //duyệt running_list để tìm pcb tương ứng (dựa vào pid) và gán cho caller
   for (int i = 0; i < krnl->running_list->size; i++) {
      if (krnl->running_list->proc[i] != NULL && krnl->running_list->proc[i]->pid == pid) {
         caller = krnl->running_list->proc[i];
         break;
      }
   }
   pthread_mutex_unlock(&queue_lock);

   if (caller == NULL) {
      return -1;
   }

   //điều hướng các thao tác dựa vào memop
   switch (memop) {
   case SYSMEM_MAP_OP:
			vmap_pgd_memset(caller, regs->a2, regs->a3);
            break;
   case SYSMEM_INC_OP:
            inc_vma_limit(caller, regs->a2, regs->a3);
            break;
   case SYSMEM_SWP_OP:
            __mm_swap_page(caller, regs->a2, regs->a3);
            break;
   case SYSMEM_IO_READ:
            MEMPHY_read(krnl->mram, regs->a2, &value);
            regs->a3 = value;
            break;
   case SYSMEM_IO_WRITE:
            MEMPHY_write(krnl->mram, regs->a2, regs->a3);
            break;
   default:
            printf("Memop code: %d\n", memop);
            break;
   }
   
   return 0;
}