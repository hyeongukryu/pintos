#include "page.h"
#include "threads/vaddr.h"
#include "threads/thread.h"

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
  return !!hash_delete (vm, &vme->elem);
}

bool load_file (void *kaddr, struct vm_entry *vme)
{
  ASSERT (kaddr != NULL);
  ASSERT (vme != NULL);
  ASSERT (vme->type == VM_BIN);

  if (file_read_at (vme->file, kaddr, vme->read_bytes, vme->offset) != (int)vme->read_bytes)
    {
      return false;
    }
  memset (kaddr + vme->read_bytes, 0, vme->zero_bytes);
  return true;
}
