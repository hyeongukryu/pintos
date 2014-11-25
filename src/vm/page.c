#include "page.h"

static unsigned vm_hash_func (const struct hash_elem *, void * UNUSED);
static bool vm_less_func (const struct hash_elem *, const struct hash_elem *, void * UNUSED);
static void vm_destroy_func (struct hash_elem *, void * UNUSED);


void vm_init (struct hash *vm)
{
}

void vm_destory (struct hash *vm)
{
}

static unsigned
vm_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
}

static bool
vm_less_func (const struct hash_elem *a,
              const struct hash_elem *b, void *aux UNUSED)
{
}

static void
vm_destroy_func (struct hash_elem *e, void *aux UNUSED)
{
}

struct vm_entry *
find_vme (void *vaddr)
{
}

bool
insert_vme (struct hash *vm, struct vm_entry *vme)
{
}

bool
delete_vme (struct hash *vm, struct vm_entry *vme)
{
}

bool load_file (void *kaddr, struct vm_entry *vme)
{
}
