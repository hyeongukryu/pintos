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

void vm_destory (struct hash *vm)
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
  free (hash_entry (e, struct vm_entry, elem));
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
  free (vme);
  return true;
}

bool load_file (void *kaddr, struct vm_entry *vme)
{
  ASSERT (kaddr != NULL);
  ASSERT (vme != NULL);
  ASSERT (vme->type == VM_BIN || vme->type == VM_FILE);

  intr_enable ();
  if (file_read_at (vme->file, kaddr, vme->read_bytes, vme->offset) != (int)vme->read_bytes)
    {
      return false;
    }
  intr_disable ();
  memset (kaddr + vme->read_bytes, 0, vme->zero_bytes);
  return true;
}

static void collect (void)
{
  struct page *victim = get_victim ();
  bool dirty = pagedir_is_dirty (victim->thread->pagedir, victim->vme->vaddr);
  pagedir_clear_page (victim->thread->pagedir, victim->vme->vaddr);
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
            if (file_write_at (victim->vme->file, victim->vme->vaddr, victim->vme->read_bytes, victim->vme->offset)
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
  palloc_free_page (victim->kaddr);
  free (victim);
  // __free_page (victim);
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
  page->kaddr = palloc_get_page (flags);
  while (page->kaddr == NULL)
    {
      collect ();
      page->kaddr = palloc_get_page (flags);
    }
  add_page_to_lru_list (page);

  return page;
}

extern struct list lru_list;
extern struct list_elem *lru_clock;

void
free_page (void *kaddr)
{
  enum intr_level old_level;
  old_level = intr_disable ();

  struct page *page = find_page_from_lru_list (kaddr);
  __free_page(page);

  intr_set_level (old_level);
}

void
__free_page (struct page *page)
{
  enum intr_level old_level;
  old_level = intr_disable ();

  ASSERT (page != NULL);
  ASSERT (page->thread != NULL);
  ASSERT (page->vme != NULL);

  pagedir_clear_page (page->thread->pagedir, page->vme->vaddr);
  del_page_from_lru_list (page);
  palloc_free_page (page->kaddr);
  free (page);

  intr_set_level (old_level);
}
