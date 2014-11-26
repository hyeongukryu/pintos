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
  ASSERT (pg_ofs (kaddr) == 0);

  enum intr_level old_level = intr_enable ();
  lock_acquire (&swap_lock);

  used_index <<= 3;
  struct block *block = block_get_role (BLOCK_SWAP);

  int i;
  ASSERT (pg_ofs (kaddr) == 0);
  for (i = 0; i < 8; i++)
    block_read (block, used_index + i, kaddr + BLOCK_SECTOR_SIZE * i);
  used_index >>= 3;

  bitmap_set_multiple (swap_bitmap, used_index, 1, false);
  ASSERT (pg_ofs (kaddr) == 0);
  lock_release (&swap_lock);
  intr_set_level (old_level);
}

size_t
swap_out (void *kaddr)
{
  ASSERT (pg_ofs (kaddr) == 0);

  enum intr_level old_level = intr_enable ();
  lock_acquire (&swap_lock);

  size_t swap_index;
  swap_index = bitmap_scan_and_flip (swap_bitmap, 0, 1, false);
  if (BITMAP_ERROR == swap_index)
  {
    NOT_REACHED();
    return BITMAP_ERROR;
  }

  struct block *block = block_get_role (BLOCK_SWAP);
  swap_index <<= 3;
  int i;
  ASSERT (pg_ofs (kaddr) == 0);
  for (i = 0; i < 8; i++)
    block_write (block, swap_index + i, kaddr + BLOCK_SECTOR_SIZE * i);
  swap_index >>= 3;

  lock_release (&swap_lock);
  intr_set_level (old_level);
  ASSERT (pg_ofs (kaddr) == 0);
  return swap_index;
}
