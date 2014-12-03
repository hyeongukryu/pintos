#ifndef FILESYS_BUFFER_CACHE_H
#define FILESYS_BUFFER_CACHE_H

#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/synch.h"

struct buffer_head
  {
    bool dirty;
    bool valid;
    block_sector_t address;
    bool clock;
    struct lock lock;
    void *buffer;
  };

void bc_init (void);
void bc_term (void);
bool bc_read (block_sector_t, void *, off_t, int, int);
bool bc_write (block_sector_t, void *, off_t, int, int);
struct buffer_head *bc_lookup (block_sector_t);
struct buffer_head *bc_select_victim (void);
void bc_flush_entry (struct buffer_head *);

#endif