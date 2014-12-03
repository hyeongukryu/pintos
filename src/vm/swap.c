#include "vm/swap.h"
#include <bitmap.h>
#include "threads/synch.h"
#include "devices/block.h"
#include "threads/vaddr.h"
#include "threads/interrupt.h"

struct lock swap_lock;
struct bitmap *swap_bitmap;

void
swap_init (size_t size)
{
  lock_init (&swap_lock);
  swap_bitmap = bitmap_create (size);
}

void
swap_in (size_t used_index, void *kaddr)
{
  struct block *swap_block;
  swap_block = block_get_role (BLOCK_SWAP);

  if (used_index-- == 0)
    NOT_REACHED ();

  lock_acquire (&swap_lock);
  ASSERT (pg_ofs (kaddr) == 0);

  used_index <<= 3;
  int i;
  for (i = 0; i < 8; i++)
    block_read (swap_block, used_index + i, kaddr + BLOCK_SECTOR_SIZE * i);
  used_index >>= 3;

  bitmap_set_multiple (swap_bitmap, used_index, 1, false);
  ASSERT (pg_ofs (kaddr) == 0);

  lock_release (&swap_lock);
}

void swap_clear (size_t used_index)
{
  if (used_index-- == 0)
    return;
  lock_acquire (&swap_lock);
  bitmap_set_multiple (swap_bitmap, used_index, 1, false);
  lock_release (&swap_lock);
}

size_t
swap_out (void *kaddr)
{
  struct block *swap_block;
  swap_block = block_get_role (BLOCK_SWAP);

  lock_acquire (&swap_lock);
  ASSERT (pg_ofs (kaddr) == 0);
  size_t swap_index;
  swap_index = bitmap_scan_and_flip (swap_bitmap, 0, 1, false);
  if (BITMAP_ERROR == swap_index)
  {
    NOT_REACHED();
    return BITMAP_ERROR;
  }
  swap_index <<= 3;
  int i;
  for (i = 0; i < 8; i++)
    block_write (swap_block, swap_index + i, kaddr + BLOCK_SECTOR_SIZE * i);
  swap_index >>= 3;

  ASSERT (pg_ofs (kaddr) == 0);
  lock_release (&swap_lock);
  return swap_index + 1;
}
