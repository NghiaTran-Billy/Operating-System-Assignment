/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

// #ifdef MM_PAGING
/*
 * PAGING based Memory Management
 * Memory physical module mm/mm-memphy.c
 *
 * SV2 notes
 *  - A MEMPHY device (RAM or one SWAP) owns a byte array [storage] and a
 *    list of free frames [free_fp_list].
 *  - The free-frame lists are shared by every CPU thread, so all list
 *    operations are protected by one module lock (memphy_lock).
 *  - Every access is bounds checked and returns -1 instead of crashing.
 */

#include "mm.h"
#include "mm64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>

#ifdef MM64
#define MEMPHY_PAGESZ PAGING64_PAGESZ
#else
#define MEMPHY_PAGESZ PAGING_PAGESZ
#endif

/* Protects free_fp_list of every device and the cursor of sequential devices */
static pthread_mutex_t memphy_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 *  MEMPHY_mv_csr - move MEMPHY cursor
 *  @mp: memphy struct
 *  @offset: offset
 */
int MEMPHY_mv_csr(struct memphy_struct *mp, addr_t offset)
{
   int numstep = 0;

   if (mp == NULL || mp->maxsz <= 0)
      return -1;

   mp->cursor = 0;
   while ((addr_t)numstep < offset && numstep < mp->maxsz)
   {
      /* Traverse sequentially */
      mp->cursor = (mp->cursor + 1) % mp->maxsz;
      numstep++;
   }

   return 0;
}

/*
 *  MEMPHY_seq_read - read MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @value: obtained value
 */
int MEMPHY_seq_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   if (mp == NULL || value == NULL)
      return -1;

   if (mp->rdmflg)
      return -1; /* Not compatible mode: this is a random access device */

   if (addr >= (addr_t)mp->maxsz || mp->storage == NULL)
      return -1; /* Out of the device */

   pthread_mutex_lock(&memphy_lock);
   MEMPHY_mv_csr(mp, addr);
   *value = (BYTE)mp->storage[addr];
   pthread_mutex_unlock(&memphy_lock);

   return 0;
}

/*
 *  MEMPHY_read read MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @value: obtained value
 */
int MEMPHY_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   if (mp == NULL || value == NULL)
      return -1;

   if (mp->rdmflg)
   {
      if (addr >= (addr_t)mp->maxsz || mp->storage == NULL)
         return -1; /* Out of the device */
      *value = mp->storage[addr];
   }
   else /* Sequential access device */
      return MEMPHY_seq_read(mp, addr, value);

   return 0;
}

/*
 *  MEMPHY_seq_write - write MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @data: written data
 */
int MEMPHY_seq_write(struct memphy_struct *mp, addr_t addr, BYTE value)
{

   if (mp == NULL)
      return -1;

   if (mp->rdmflg)
      return -1; /* Not compatible mode: this is a random access device */

   if (addr >= (addr_t)mp->maxsz || mp->storage == NULL)
      return -1; /* Out of the device */

   pthread_mutex_lock(&memphy_lock);
   MEMPHY_mv_csr(mp, addr);
   mp->storage[addr] = value;
   pthread_mutex_unlock(&memphy_lock);

   return 0;
}

/*
 *  MEMPHY_write-write MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @data: written data
 */
int MEMPHY_write(struct memphy_struct *mp, addr_t addr, BYTE data)
{
   if (mp == NULL)
      return -1;

   if (mp->rdmflg)
   {
      if (addr >= (addr_t)mp->maxsz || mp->storage == NULL)
         return -1; /* Out of the device */
      mp->storage[addr] = data;
   }
   else /* Sequential access device */
      return MEMPHY_seq_write(mp, addr, data);

   return 0;
}

/*
 *  MEMPHY_format-format MEMPHY device
 *  Build the list of free frames 0 .. maxsz/pagesz - 1 (in ascending order)
 *  @mp: memphy struct
 *  Return 0 on success, -1 when the device cannot hold one single frame.
 */
int MEMPHY_format(struct memphy_struct *mp, int pagesz)
{
   /* This setting come with fixed constant PAGESZ */
   struct framephy_struct *head = NULL, *tail = NULL, *node;
   int numfp;
   int iter;

   if (mp == NULL || pagesz <= 0)
      return -1;

   numfp = mp->maxsz / pagesz;

   if (numfp <= 0)
   {
      /* Device smaller than one frame: keep the lists well defined (empty) */
      pthread_mutex_lock(&memphy_lock);
      mp->free_fp_list = NULL;
      pthread_mutex_unlock(&memphy_lock);
      return -1;
   }

   for (iter = 0; iter < numfp; iter++)
   {
      node = malloc(sizeof(struct framephy_struct));
      if (node == NULL)
      {
         /* Out of host memory: release what was built so far */
         while (head != NULL)
         {
            node = head->fp_next;
            free(head);
            head = node;
         }
         return -1;
      }
      node->fpn = iter;
      node->fp_next = NULL;
      node->owner = NULL;

      if (tail == NULL)
         head = node;
      else
         tail->fp_next = node;
      tail = node;
   }

   pthread_mutex_lock(&memphy_lock);
   mp->free_fp_list = head;
   pthread_mutex_unlock(&memphy_lock);

   return 0;
}

/*
 *  MEMPHY_get_freefp - take one free frame from the device
 *  @mp: memphy struct
 *  @retfpn: returned frame number
 *  Return 0 on success, -1 when no frame is left (device exhausted).
 */
int MEMPHY_get_freefp(struct memphy_struct *mp, addr_t *retfpn)
{
   struct framephy_struct *fp;

   if (mp == NULL || retfpn == NULL)
      return -1;

   pthread_mutex_lock(&memphy_lock);

   fp = mp->free_fp_list;
   if (fp == NULL)
   {
      pthread_mutex_unlock(&memphy_lock);
      return -1;
   }

   /* Hand out the frame and unlink the node: the frame is now "used" */
   *retfpn = fp->fpn;
   mp->free_fp_list = fp->fp_next;

   pthread_mutex_unlock(&memphy_lock);

   /* MEMPHY is iteratively used up until it is exhausted,
    * the list node itself is no longer needed. */
   free(fp);

   return 0;
}

/*
 *  MEMPHY_count_freefp - number of free frames left in the device
 *  (used by tests and by the report statistics)
 */
int MEMPHY_count_freefp(struct memphy_struct *mp)
{
   struct framephy_struct *fp;
   int cnt = 0;

   if (mp == NULL)
      return 0;

   pthread_mutex_lock(&memphy_lock);
   for (fp = mp->free_fp_list; fp != NULL; fp = fp->fp_next)
      cnt++;
   pthread_mutex_unlock(&memphy_lock);

   return cnt;
}

/*
 *  MEMPHY_dump - print every non-zero byte of the device
 *  for tracing the memory content
 */
int MEMPHY_dump(struct memphy_struct *mp)
{
   int i;

   if (mp == NULL || mp->storage == NULL)
      return -1;

   printf("===== PHYSICAL MEMORY DUMP =====\n");
   for (i = 0; i < mp->maxsz; i++)
   {
      if (mp->storage[i] != 0)
         printf("BYTE %08x: %d\n", i, mp->storage[i]);
   }
   printf("===== PHYSICAL MEMORY END-DUMP =====\n");
   printf("================================================================\n");

   return 0;
}

/*
 *  MEMPHY_put_freefp - give a frame back to the device
 *  @mp: memphy struct
 *  @fpn: frame number
 *  Return -1 for a frame that does not belong to this device.
 */
int MEMPHY_put_freefp(struct memphy_struct *mp, addr_t fpn)
{
   struct framephy_struct *newnode;

   if (mp == NULL)
      return -1;

   if (fpn >= (addr_t)(mp->maxsz / MEMPHY_PAGESZ))
      return -1; /* Not a frame of this device */

   newnode = malloc(sizeof(struct framephy_struct));
   if (newnode == NULL)
      return -1;

   /* Create new node with value fpn */
   newnode->fpn = fpn;
   newnode->owner = NULL;

   pthread_mutex_lock(&memphy_lock);
   newnode->fp_next = mp->free_fp_list;
   mp->free_fp_list = newnode;
   pthread_mutex_unlock(&memphy_lock);

   return 0;
}

/*
 *  Init MEMPHY struct
 *  A device of size 0 is valid: it simply is switched off (no frame).
 */
int init_memphy(struct memphy_struct *mp, addr_t max_size, int randomflg)
{
   if (mp == NULL)
      return -1;

   /* The struct may come from the stack: make every field well defined first */
   mp->storage = NULL;
   mp->maxsz = 0;
   mp->cursor = 0;
   mp->free_fp_list = NULL;
   mp->used_fp_list = NULL;
   mp->rdmflg = (randomflg != 0) ? 1 : 0;

   if (max_size == 0)
      return 0; /* deactivated device */

   /* maxsz is an int: refuse sizes it cannot represent instead of wrapping */
   if (max_size > (addr_t)INT_MAX)
   {
      fprintf(stderr, "[WARN] init_memphy: size %llu too large, limited to %d bytes\n",
              (unsigned long long)max_size, INT_MAX);
      max_size = (addr_t)INT_MAX;
   }

   /* calloc: zeroed memory without touching every page of a big device */
   mp->storage = (BYTE *)calloc((size_t)max_size, sizeof(BYTE));
   if (mp->storage == NULL)
   {
      fprintf(stderr, "[ERROR] init_memphy: cannot allocate %llu bytes\n",
              (unsigned long long)max_size);
      return -1;
   }
   mp->maxsz = (int)max_size;

   if (MEMPHY_format(mp, MEMPHY_PAGESZ) != 0)
      fprintf(stderr, "[WARN] init_memphy: %d bytes is smaller than one page (%d bytes): no frame\n",
              mp->maxsz, MEMPHY_PAGESZ);

   if (!mp->rdmflg) /* Not Ramdom acess device, then it serial device*/
      mp->cursor = 0;

   return 0;
}

// #endif
