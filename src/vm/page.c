#include "page.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"
#include "threads/interrupt.h"
#include <string.h>

static unsigned vm_hash_func (const struct hash_elem *, void * UNUSED);
static bool vm_less_func (const struct hash_elem *, const struct hash_elem *, void * UNUSED);
static void vm_destroy_func (struct hash_elem *, void * UNUSED);

void vm_init (struct hash *vm)
{
  ASSERT (vm != NULL);
  hash_init (vm, vm_hash_func, vm_less_func, NULL);
}

void vm_destroy (struct hash *vm)
{
  ASSERT (vm != NULL);
  hash_destroy (vm, vm_destroy_func);
}

static unsigned
vm_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  ASSERT (e != NULL);
  return hash_int (hash_entry (e, struct vm_entry, elem)->vaddr);
}

static bool
vm_less_func (const struct hash_elem *a,
              const struct hash_elem *b, void *aux UNUSED)
{
  ASSERT (a != NULL);
  ASSERT (b != NULL);
  return hash_entry (a, struct vm_entry, elem)->vaddr
    < hash_entry (b, struct vm_entry, elem)->vaddr;
}

static void
vm_destroy_func (struct hash_elem *e, void *aux UNUSED)
{
  ASSERT (e != NULL);
  struct vm_entry *vme = hash_entry (e, struct vm_entry, elem);
  free_page_vaddr (vme->vaddr);
  swap_clear (vme->swap_slot);
  free (vme);
}

struct vm_entry *
find_vme (void *vaddr)
{
  struct hash *vm;
  struct vm_entry vme;
  struct hash_elem *elem;

  vm = &thread_current ()->vm;
  vme.vaddr = pg_round_down (vaddr);
  ASSERT (pg_ofs (vme.vaddr) == 0);
  elem = hash_find (vm, &vme.elem);
  return elem ? hash_entry (elem, struct vm_entry, elem) : NULL;
}

bool
insert_vme (struct hash *vm, struct vm_entry *vme)
{
  ASSERT (vm != NULL);
  ASSERT (vme != NULL);
  ASSERT (pg_ofs (vme->vaddr) == 0);
  return hash_insert (vm, &vme->elem) == NULL;
}

bool
delete_vme (struct hash *vm, struct vm_entry *vme)
{
  ASSERT (vm != NULL);
  ASSERT (vme != NULL);
  if (!hash_delete (vm, &vme->elem))
    return false;
  free_page_vaddr (vme->vaddr);
  swap_clear (vme->swap_slot);
  free (vme);
  return true;
}

bool load_file (void *kaddr, struct vm_entry *vme)
{
  ASSERT (kaddr != NULL);
  ASSERT (vme != NULL);
  ASSERT (vme->type == VM_BIN || vme->type == VM_FILE);

  if (file_read_at (vme->file, kaddr, vme->read_bytes, vme->offset) != (int) vme->read_bytes)
    {
      return false;
    }

  memset (kaddr + vme->read_bytes, 0, vme->zero_bytes);
  return true;
}

static void collect (void)
{
  lock_acquire (&lru_list_lock);
  struct page *victim = get_victim ();
  
  ASSERT (victim != NULL);
  ASSERT (victim->thread != NULL);
  ASSERT (victim->thread->magic == 0xcd6abf4b);
  ASSERT (victim->vme != NULL);

  bool dirty = pagedir_is_dirty (victim->thread->pagedir, victim->vme->vaddr);
  switch (victim->vme->type)
    {
      case VM_BIN:
        if (dirty)
          {
            victim->vme->swap_slot = swap_out (victim->kaddr);
            victim->vme->type = VM_ANON;
          }
        break;
      case VM_FILE:
        if (dirty)
          {
            if (file_write_at (victim->vme->file, victim->vme->vaddr,
                               victim->vme->read_bytes, victim->vme->offset)
              != (int) victim->vme->read_bytes)
              NOT_REACHED ();
          }
        break;
      case VM_ANON:
        victim->vme->swap_slot = swap_out (victim->kaddr);
        break;
      default:
        NOT_REACHED ();
    }
  victim->vme->is_loaded = false;
  __free_page(victim);
  lock_release (&lru_list_lock);
}

struct page *
alloc_page (enum palloc_flags flags)
{
  struct page *page;
  page = (struct page *)malloc (sizeof (struct page));
  if (page == NULL)
    return NULL;
  memset (page, 0, sizeof (struct page));
  page->thread = thread_current ();
  ASSERT (page->thread);
  ASSERT (page->thread->magic == 0xcd6abf4b);
  page->kaddr = palloc_get_page (flags);
  while (page->kaddr == NULL)
    {
      collect ();
      page->kaddr = palloc_get_page (flags);
    }
  return page;
}

extern struct list lru_list;

void
free_page_kaddr (void *kaddr)
{
  lock_acquire (&lru_list_lock);

  struct page *page = find_page_from_lru_list (kaddr);
  if (page)
    __free_page(page);

  lock_release (&lru_list_lock);
}

void
free_page_vaddr (void *vaddr)
{
  free_page_kaddr (pagedir_get_page (thread_current ()->pagedir, vaddr));
}

void
__free_page (struct page *page)
{
  ASSERT (lock_held_by_current_thread (&lru_list_lock));

  ASSERT (page != NULL);
  ASSERT (page->thread != NULL);
  ASSERT (page->thread->magic == 0xcd6abf4b);
  ASSERT (page->vme != NULL);

  pagedir_clear_page (page->thread->pagedir, page->vme->vaddr);
  del_page_from_lru_list (page);
  palloc_free_page (page->kaddr);
  free (page);
}
