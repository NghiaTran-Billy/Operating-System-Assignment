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
 */

#include "mm.h"
#include "mm64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/*
 * Leaf lock protecting the free-frame lists (RAM and SWAP).
 * It is only held inside MEMPHY_format/get_freefp/put_freefp and never while
 * calling out to another module, so it cannot take part in a lock cycle.
 */
static pthread_mutex_t memphy_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef MM64
#define MEMPHY_PAGESZ PAGING64_PAGESZ
#else
#define MEMPHY_PAGESZ PAGING_PAGESZ
#endif

/*
 *  MEMPHY_mv_csr - move MEMPHY cursor
 *  @mp: memphy struct
 *  @offset: offset
 */
int MEMPHY_mv_csr(struct memphy_struct *mp, addr_t offset)
{
   int numstep = 0;

   mp->cursor = 0;
   while (numstep < offset && numstep < mp->maxsz)
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
   if (mp == NULL)
      return -1;

   if (!mp->rdmflg)
      return -1; /* Not compatible mode for sequential read */

   if (mp->storage == NULL || addr >= (addr_t)mp->maxsz)
      return -1; /* Disabled device or address out of range */

   MEMPHY_mv_csr(mp, addr);
   *value = (BYTE)mp->storage[addr];

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
   if (mp == NULL)
      return -1;

   if (mp->storage == NULL || addr >= (addr_t)mp->maxsz)
      return -1; /* Disabled device or address out of range */

   if (mp->rdmflg)
      *value = mp->storage[addr];
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

   if (!mp->rdmflg)
      return -1; /* Not compatible mode for sequential read */

   if (mp->storage == NULL || addr >= (addr_t)mp->maxsz)
      return -1; /* Disabled device or address out of range */

   MEMPHY_mv_csr(mp, addr);
   mp->storage[addr] = value;

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

   if (mp->storage == NULL || addr >= (addr_t)mp->maxsz)
      return -1; /* Disabled device or address out of range */

   if (mp->rdmflg)
      mp->storage[addr] = data;
   else /* Sequential access device */
      return MEMPHY_seq_write(mp, addr, data);

   return 0;
}

/*
 *  MEMPHY_format-format MEMPHY device
 *  Build the free-frame list fpn = 0 .. numfp-1 (ascending order).
 *  @mp: memphy struct
 *  @pagesz: page (frame) size in bytes
 */
int MEMPHY_format(struct memphy_struct *mp, int pagesz)
{
   /* This setting come with fixed constant PAGESZ */
   int numfp;
   struct framephy_struct *newfst, *fst = NULL;
   int iter;

   if (mp == NULL || pagesz <= 0)
      return -1;

   numfp = mp->maxsz / pagesz;

   pthread_mutex_lock(&memphy_lock);

   /* The struct may be uninitialized (stack object): never trust old pointers */
   mp->free_fp_list = NULL;
   mp->used_fp_list = NULL;

   if (numfp <= 0)
   {
      pthread_mutex_unlock(&memphy_lock);
      return -1;
   }

   /* Init head of free framephy list, then append the rest (numfp-1 nodes) */
   for (iter = 0; iter < numfp; iter++)
   {
      newfst = malloc(sizeof(struct framephy_struct));
      if (newfst == NULL)
      {
         /* Out of memory: release the partial list */
         while (mp->free_fp_list != NULL)
         {
            fst = mp->free_fp_list;
            mp->free_fp_list = fst->fp_next;
            free(fst);
         }
         pthread_mutex_unlock(&memphy_lock);
         return -1;
      }

      newfst->fpn = iter;
      newfst->fp_next = NULL; /* every node ends the list until a successor links in */
      newfst->owner = NULL;

      if (fst == NULL)
         mp->free_fp_list = newfst;
      else
         fst->fp_next = newfst;
      fst = newfst;
   }

   pthread_mutex_unlock(&memphy_lock);
   return 0;
}

/*
 *  MEMPHY_get_freefp - take one free frame
 *  @mp: memphy struct
 *  @retfpn: returned frame number
 *  return: 0 on success, -1 if no free frame is left
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

   /* Pop the head of the free list and hand its frame number to the caller */
   *retfpn = fp->fpn;
   mp->free_fp_list = fp->fp_next;

   pthread_mutex_unlock(&memphy_lock);

   /* The node only described the frame; the frame itself stays valid */
   free(fp);

   return 0;
}

/*
 *  MEMPHY_dump - print every non-zero byte of the device storage
 *  @mp: memphy struct
 */
int MEMPHY_dump(struct memphy_struct *mp)
{
   addr_t addr;
   addr_t nonzero = 0;

   if (mp == NULL || mp->storage == NULL)
      return -1;

   printf("===== PHYSICAL MEMORY DUMP =====\n");
   for (addr = 0; addr < (addr_t)mp->maxsz; addr++)
   {
      unsigned char b = (unsigned char)mp->storage[addr];

      if (b != 0)
      {
         printf("BYTE %08lx: %d\n", (unsigned long)addr, b);
         nonzero++;
      }
   }
   printf("===== PHYSICAL MEMORY END-DUMP =====\n");
   printf("(size %d bytes, %lu non-zero byte(s))\n", mp->maxsz, (unsigned long)nonzero);

   return 0;
}

/*
 *  MEMPHY_put_freefp - give a frame back to the free list
 *  @mp: memphy struct
 *  @fpn: frame number
 */
int MEMPHY_put_freefp(struct memphy_struct *mp, addr_t fpn)
{
   struct framephy_struct *newnode;

   if (mp == NULL)
      return -1;

   /* Reject frames that do not exist on this device */
   if (fpn >= (addr_t)(mp->maxsz / MEMPHY_PAGESZ))
      return -1;

   newnode = malloc(sizeof(struct framephy_struct));
   if (newnode == NULL)
      return -1;

   /* Create new node with value fpn and push it at the head */
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
 *  A device of size 0 is a switched-off device (unused swap slot):
 *  it has no storage and no frame, every access to it fails.
 */
int init_memphy(struct memphy_struct *mp, addr_t max_size, int randomflg)
{
   int ret = 0;

   if (mp == NULL)
      return -1;

   mp->storage = NULL;
   mp->maxsz = 0;
   mp->free_fp_list = NULL;
   mp->used_fp_list = NULL;
   mp->rdmflg = (randomflg != 0) ? 1 : 0;
   mp->cursor = 0; /* Serial device starts at the first cell */

   if (max_size == 0)
      return 0; /* Disabled device */

   mp->storage = (BYTE *)calloc(max_size, sizeof(BYTE));
   if (mp->storage == NULL)
      return -1;
   mp->maxsz = max_size;

   if (MEMPHY_format(mp, MEMPHY_PAGESZ) != 0)
      ret = -1; /* Smaller than one frame: storage exists but no frame to give */

   return ret;
}

// #endif
