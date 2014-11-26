#include "vm/frame.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "threads/thread.h"
#include "threads/interrupt.h"
#include "threads/vaddr.h"

static struct list_elem *get_next_lru_clock (void);

struct list lru_list;
struct lock lru_list_lock;
struct list_elem *lru_clock;

void
lru_list_init (void)
{
  list_init (&lru_list);
  lock_init (&lru_list_lock);
  lru_clock = NULL;
}

void
add_page_to_lru_list (struct page *page)
{
  lock_acquire (&lru_list_lock);  
  list_push_back (&lru_list, &page->lru);
  lock_release (&lru_list_lock);
}

struct page *
find_page_from_lru_list (void *kaddr)
{
  lock_acquire (&lru_list_lock);
  ASSERT (pg_ofs (kaddr) == 0);

  struct list_elem *e;
  for (e = list_begin (&lru_list);
       e != list_end (&lru_list);
       e = list_next (e))
    {
      struct page *page = list_entry (e, struct page, lru);
      if (page->kaddr == kaddr)
        {
          lock_release (&lru_list_lock);
          return page; 
        }
    }

  lock_release (&lru_list_lock);
  return NULL;
}

void
del_page_from_lru_list (struct page *page)
{
  lock_acquire (&lru_list_lock);
  if (lru_clock == &page->lru)
    {
      lru_clock = list_remove (lru_clock);
    }
  else
    {
      list_remove (&page->lru);
    }
  lock_release (&lru_list_lock);
}

static struct list_elem *
get_next_lru_clock (void)
{
  if (lru_clock == NULL)
    return (lru_clock = list_begin (&lru_list));

  if (list_begin (&lru_list) == list_end (&lru_list))
  {
    return NULL;
  }
  struct page *page = list_entry (lru_clock, struct page, lru);
  
  ASSERT (page != NULL);
  ASSERT (page->thread != NULL);
  ASSERT (pg_ofs (page->kaddr) == 0);
  // ASSERT (page->vme != NULL);

  if (page->vme)
    pagedir_set_accessed (page->thread->pagedir, page->vme->vaddr, false);
  lru_clock = list_next (lru_clock);

  if (lru_clock == list_end (&lru_list))
    {
      lru_clock = list_begin (&lru_list);
    }
  return lru_clock;
}

struct page *
get_victim (void)
{
  lock_acquire (&lru_list_lock);
  struct page *page;
  struct list_elem *e;
  while (1)
    {
      while (!(e = get_next_lru_clock ()));
      page = list_entry (e, struct page, lru);

      ASSERT (page != NULL);
      ASSERT (page->thread != NULL);
      ASSERT (pg_ofs (page->kaddr) == 0);

      if (page->vme == NULL)
      {
        continue;
      }

      if (pagedir_is_accessed (page->thread->pagedir, page->vme->vaddr))
      {
        continue;
      }
      break;
    }

  e = list_remove (e);
  lock_release (&lru_list_lock);
  ASSERT (page != NULL);
  ASSERT (page->thread != NULL);
  ASSERT (pg_ofs (page->kaddr) == 0);
  return page;
}
