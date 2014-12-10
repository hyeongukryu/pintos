#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "filesys/buffer_cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define DIRECT_BLOCK_ENTRIES 124
#define INDIRECT_BLOCK_ENTRIES (BLOCK_SECTOR_SIZE / sizeof (block_sector_t))

enum direct_t
  {
    NORMAL_DIRECT,
    INDIRECT,
    DOUBLE_INDIRECT,
    OUT_LIMIT
  };

struct sector_location
  {
    int directness;
    int index1;
    int index2;
  };

struct inode_indirect_block
  {
    block_sector_t map_table[INDIRECT_BLOCK_ENTRIES];
  };

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    block_sector_t direct_map_table[DIRECT_BLOCK_ENTRIES];
    block_sector_t indirect_block_sec;
    block_sector_t double_indirect_block_sec;
  };

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct lock extend_lock;
  };

static bool get_disk_inode (const struct inode *, struct inode_disk *);
static void locate_byte (off_t, struct sector_location *);
static bool register_sector (struct inode_disk *, block_sector_t, struct sector_location);
static bool inode_update_file_length (struct inode_disk *, off_t, off_t);
static void free_inode_sectors (struct inode_disk *);

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode_disk *inode_disk, off_t pos) 
{
  ASSERT (inode_disk != NULL);

  struct inode_indirect_block ind_block;
  struct sector_location sec_loc;
  block_sector_t table_sector = inode_disk->indirect_block_sec;

  if ((pos < inode_disk->length) == false)
    return -1;

  locate_byte (pos, &sec_loc);
  switch (sec_loc.directness)
    {
      case NORMAL_DIRECT:
        return inode_disk->direct_map_table[sec_loc.index1];
      case DOUBLE_INDIRECT:
        if (inode_disk->double_indirect_block_sec == (block_sector_t) -1)
          return -1;
        if (!bc_read (inode_disk->double_indirect_block_sec, &ind_block, 0, sizeof (struct inode_indirect_block), 0))
          return -1;
        table_sector = ind_block.map_table[sec_loc.index2];
      case INDIRECT:
        if (table_sector == (block_sector_t) -1)
          return -1;
        if (!bc_read (table_sector, &ind_block, 0, sizeof (struct inode_indirect_block), 0))
          return -1;
        return ind_block.map_table[sec_loc.index1];
      default:
        return -1;
    }
  NOT_REACHED ();
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      memset (disk_inode, -1, sizeof (struct inode_disk));
      disk_inode->length = 0;

      if (!inode_update_file_length (disk_inode, disk_inode->length, length))
        NOT_REACHED ();
      disk_inode->magic = INODE_MAGIC;
      
      bc_write (sector, disk_inode, 0, BLOCK_SECTOR_SIZE, 0);
      free (disk_inode);
      success = true;
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init (&inode->extend_lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          struct inode_disk inode_disk;
          bc_read (inode->sector, &inode_disk, 0, BLOCK_SECTOR_SIZE, 0);
          free_inode_sectors (&inode_disk);
          free_map_release (inode->sector, 1);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  struct inode_disk inode_disk;
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  lock_acquire (&inode->extend_lock);
  get_disk_inode (inode, &inode_disk);

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (&inode_disk, offset);
      lock_release (&inode->extend_lock);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_disk.length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        {
          lock_acquire (&inode->extend_lock);
          break;
        }

      bc_read (sector_idx, buffer, bytes_read, chunk_size, sector_ofs);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;

      lock_acquire (&inode->extend_lock);
    }
  lock_release (&inode->extend_lock);
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  struct inode_disk inode_disk;
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  lock_acquire (&inode->extend_lock);
  get_disk_inode (inode, &inode_disk);
  if (inode_disk.length < offset + size)
    {
      if (!inode_update_file_length (&inode_disk, inode_disk.length, offset + size))
        NOT_REACHED ();
      bc_write (inode->sector, &inode_disk, 0, BLOCK_SECTOR_SIZE, 0);
    }
  
  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (&inode_disk, offset);
      lock_release (&inode->extend_lock);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;
  
      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_disk.length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        {
          lock_acquire (&inode->extend_lock);
          break;
        }

      bc_write (sector_idx, (void *)buffer, bytes_written, chunk_size, sector_ofs);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
      lock_acquire (&inode->extend_lock);
    }
  lock_release (&inode->extend_lock);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

static bool
get_disk_inode (const struct inode *inode, struct inode_disk *inode_disk)
{
  return bc_read (inode->sector, inode_disk, 0, sizeof (struct inode_disk), 0);
}

static void
locate_byte (off_t pos, struct sector_location *sec_loc)
{
  off_t pos_sector = pos / BLOCK_SECTOR_SIZE;
  sec_loc->directness = OUT_LIMIT;
  if (pos_sector < DIRECT_BLOCK_ENTRIES)
    {
      sec_loc->directness = NORMAL_DIRECT;
      sec_loc->index1 = pos_sector;
    }
  else if ((pos_sector -= DIRECT_BLOCK_ENTRIES) < INDIRECT_BLOCK_ENTRIES)
    {
      sec_loc->directness = INDIRECT;
      sec_loc->index1 = pos_sector;
    }
  else if ((pos_sector -= INDIRECT_BLOCK_ENTRIES) < INDIRECT_BLOCK_ENTRIES * INDIRECT_BLOCK_ENTRIES)
    {
      sec_loc->directness = DOUBLE_INDIRECT;
      sec_loc->index2 = pos_sector / INDIRECT_BLOCK_ENTRIES;
      sec_loc->index1 = pos_sector % INDIRECT_BLOCK_ENTRIES;
    }
}

static bool
register_sector (struct inode_disk *inode_disk,
                 block_sector_t new_sector,
                 struct sector_location sec_loc)
{
  struct inode_indirect_block first_block, second_block;
  bool first_dirty = false;
  block_sector_t *table_sector = &inode_disk->indirect_block_sec;

  switch (sec_loc.directness)
    {
    case NORMAL_DIRECT:
      inode_disk->direct_map_table[sec_loc.index1] = new_sector;
  
      return true;
    case DOUBLE_INDIRECT:
      table_sector = &inode_disk->double_indirect_block_sec;
      if (*table_sector == (block_sector_t) -1)
        {
          if (!free_map_allocate (1, table_sector))
            return false;
          memset (&first_block, -1, sizeof (struct inode_indirect_block));
        }
      else
        {
          if (!bc_read (*table_sector, &first_block, 0, sizeof (struct inode_indirect_block), 0))
            return false;
        }
      table_sector = &first_block.map_table[sec_loc.index2];
      if (*table_sector == (block_sector_t) -1)
          first_dirty = true;
    case INDIRECT:
      if (*table_sector == (block_sector_t) -1)
        {
          if (!free_map_allocate (1, table_sector))
            return false;
          memset (&second_block, -1, sizeof (struct inode_indirect_block));
        }
      else
        {
          if (!bc_read (*table_sector, &second_block, 0, sizeof (struct inode_indirect_block), 0))
            return false;
        }
      if (second_block.map_table[sec_loc.index1] == (block_sector_t) -1)
        second_block.map_table[sec_loc.index1] = new_sector;
      else
        NOT_REACHED ();
      if (first_dirty)
        {
          if (!bc_write (inode_disk->double_indirect_block_sec, &first_block, 0, sizeof (struct inode_indirect_block), 0))
            return false;
        }
      if (!bc_write (*table_sector, &second_block, 0, sizeof (struct inode_indirect_block), 0))
        return false;
      return true;
    default:
      return false;
    }
  NOT_REACHED ();
}

static bool
inode_update_file_length (struct inode_disk *inode_disk, off_t length, off_t new_length)
{
  static char zeros[BLOCK_SECTOR_SIZE];

  if (length == new_length)
    return true;
  if (length > new_length)
    return false;
  
  ASSERT (length < new_length);

  inode_disk->length = new_length;
  new_length--;
  length = length / BLOCK_SECTOR_SIZE * BLOCK_SECTOR_SIZE;
  new_length = new_length / BLOCK_SECTOR_SIZE * BLOCK_SECTOR_SIZE;

  for (; length <= new_length; length += BLOCK_SECTOR_SIZE)
    {
      struct sector_location sec_loc;

      block_sector_t sector = byte_to_sector (inode_disk, length);
      
      if (sector != (block_sector_t) -1)
        continue;
      
      if (!free_map_allocate (1, &sector))
        return false;
      locate_byte (length, &sec_loc);
      if (!register_sector (inode_disk, sector, sec_loc))
        return false;
      if (!bc_write (sector, zeros, 0, BLOCK_SECTOR_SIZE, 0))
        return false; 
    }
  return true;
}

static void
free_sectors (block_sector_t sector)
{
  int index;
  struct inode_indirect_block block;
  bc_read (sector, &block, 0, sizeof (struct inode_indirect_block), 0);
  for (index = 0; index < INDIRECT_BLOCK_ENTRIES; index++)
    {
      if (block.map_table[index] == (block_sector_t) -1)
        return;
      free_map_release (block.map_table[index], 1);
    }
}

static void
free_inode_sectors (struct inode_disk *inode_disk)
{
  int index;
  for (index = 0; index < DIRECT_BLOCK_ENTRIES; index++)
    {
      if (inode_disk->direct_map_table[index] == (block_sector_t) -1)
        return;
      free_map_release (inode_disk->direct_map_table[index], 1);
    }
  if (inode_disk->indirect_block_sec == (block_sector_t) -1)
    return;
  free_sectors (inode_disk->indirect_block_sec);
  free_map_release (inode_disk->indirect_block_sec, 1);
  if (inode_disk->double_indirect_block_sec == (block_sector_t) -1)
    return;
  struct inode_indirect_block block;
  bc_read (inode_disk->double_indirect_block_sec, &block, 0, sizeof (struct inode_indirect_block), 0);
  for (index = 0; index < DIRECT_BLOCK_ENTRIES; index++)
  {
    if (block.map_table[index] == (block_sector_t) -1)
      return;
    free_sectors (block.map_table[index]);
    free_map_release (block.map_table[index], 1);
  }
  free_map_release (inode_disk->double_indirect_block_sec, 1);
}

off_t
inode_length (const struct inode *inode)
{
  struct inode_disk inode_disk;
  bc_read (inode->sector, &inode_disk, 0, BLOCK_SECTOR_SIZE, 0);
  return inode_disk.length;
}
