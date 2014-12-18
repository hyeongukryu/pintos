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

// struct inode_disk의 크기가 BLOCK_SECTOR_SIZE와 같도록 하는 값입니다.
#define DIRECT_BLOCK_ENTRIES 123

// struct inode_indirect_block의 크기가 BLOCK_SECTOR_SIZE와 같도록 하는 값입니다.
#define INDIRECT_BLOCK_ENTRIES (BLOCK_SECTOR_SIZE / sizeof (block_sector_t))

// 계층 테이블 구조에서 테이블의 유형을 나타내기 위한 열거형합니다.
enum direct_t
  {
    NORMAL_DIRECT,
    INDIRECT,
    DOUBLE_INDIRECT,
    OUT_LIMIT
  };

// 테이블 유형과 두 단계 테이블의 인덱스를 묶어서 다루기 위한 구조체입니다.
struct sector_location
  {
    // 테이블 유형, enum direct_t
    int directness;
    // 두 인덱스
    int index1;
    int index2;
  };

struct inode_indirect_block
  {
    // 여러 섹터 번호를 나타냅니다. 이 구조체의 크기는 BLOCK_SECTOR_SIZE입니다.
    // 중간 계층 테아블이라면 이 섹터 번호는 마지막 계층 테이블이 저장된 섹터의 번호입니다.
    // 마지막 계층 테이블이라면 이 섹터 번호는 파일 데이터가 저장된 섹터의 번호입니다.
    block_sector_t map_table[INDIRECT_BLOCK_ENTRIES];
  };

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */

    // 디렉터리를 나타내면 1, 디렉터리가 아니면 0입니다.
    uint32_t is_dir;

    // 아래에 나열된 순서대로 테이블을 사용합니다.

    // 파일 데이터가 저장된 불연속적인 섹터 번호들을 나타냅니다.
    block_sector_t direct_map_table[DIRECT_BLOCK_ENTRIES];
    // 이 섹터에 한 단계 테이블이 저장됩니다.
    block_sector_t indirect_block_sec;
    // 이 섹터에 두 단계 테이블이 저장됩니다.
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

    // 아이노드에 관련된 데이터에 접근할 때 사용하는 락입니다.
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

  // 테이블을 메모리에서 다루기 위한 변수입니다.
  struct inode_indirect_block ind_block;
  // 테이블의 유형과 테이블에서의 위치를 나타냅니다.
  struct sector_location sec_loc;

  // 현재 살펴보고 있는 테이블의 섹터 번호입니다.
  // 실행 흐름에 따라서 한 단계 테이블 또는 두 단계 테이블을 가리킵니다.
  block_sector_t table_sector = inode_disk->indirect_block_sec;

  if ((pos < inode_disk->length) == false)
    return -1;

  // 바이트 단위 위치에서, 테이블 유형과 테이블에서의 위치를 얻습니다.
  locate_byte (pos, &sec_loc);
  switch (sec_loc.directness)
    {
      case NORMAL_DIRECT:
        // 바로 가져옵니다.
        return inode_disk->direct_map_table[sec_loc.index1];
      case DOUBLE_INDIRECT:
        // 한 번 참조합니다.
        if (inode_disk->double_indirect_block_sec == (block_sector_t) -1)
          return -1;
        if (!bc_read (inode_disk->double_indirect_block_sec, &ind_block, 0, sizeof (struct inode_indirect_block), 0))
          return -1;
        // 아직 수행하지 않은 한 번의 참조는 아래에서 계속 수행합니다.
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
  // 여기에 도달할 수 없습니다.
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
inode_create (block_sector_t sector, off_t length, uint32_t is_dir)
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
      // 바이트 -1로 구조체를 초기화합니다.
      // block_sector_t는 unsigned 정수형이며, 결과적으로 최댓값으로 초기화됩니다.
      memset (disk_inode, -1, sizeof (struct inode_disk));

      // 초기 크기에 따른 첫 파일 크기 확장을 수행합니다.
      disk_inode->length = 0;
      if (!inode_update_file_length (disk_inode, disk_inode->length, length))
        NOT_REACHED ();

      disk_inode->magic = INODE_MAGIC;

      // 디렉터리인지, 그렇지 않은지를 지정합니다.
      disk_inode->is_dir = is_dir;
      
      // 디스크 아이노드를 버퍼 캐시를 통하여 기록합니다.
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

  // 락 초기화
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
          // 아이노드에 연관된 섹터 해제
          free_inode_sectors (&inode_disk);
          // 디스크 아이노드 해제
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

  // 먼저 락을 취득합니다.
  lock_acquire (&inode->extend_lock);

  // 디스크 아이노드를 버퍼 캐시에서 읽습니다.
  get_disk_inode (inode, &inode_disk);

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */

      // 경쟁적으로 테이블에 접근할 수 있으므로 락을 취득한 상태에서 수행합니다.
      block_sector_t sector_idx = byte_to_sector (&inode_disk, offset);
      if (sector_idx == (block_sector_t) -1)
        break;

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
          // 루프의 시작 직전과 종료 직후에서 락을 취득한 상태로 유지합니다.
          lock_acquire (&inode->extend_lock);
          break;
        }

      // 섹터 번호가 정해진 이후, 데이터 읽기 작업은 락을 해제한 상태에서 수행해도 괜찮습니다.
      bc_read (sector_idx, buffer, bytes_read, chunk_size, sector_ofs);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;

      // 다음 byte_to_sector 작업 이전에, 락을 미리 취득합니다.
      lock_acquire (&inode->extend_lock);
    }
  // 마지막으로 락을 해제합니다.
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

  // 먼저 락을 취득합니다.
  lock_acquire (&inode->extend_lock);

  // 디스크 아이노드를 버퍼 캐시에서 읽습니다.
  get_disk_inode (inode, &inode_disk);
  
  if (inode_disk.length < offset + size)
    {
      // 크기 변화가 이 쓰기로 인하여 발생됩니다.
      if (!inode_update_file_length (&inode_disk, inode_disk.length, offset + size))
        NOT_REACHED ();
      // 디스크 아이노드는 바로 앞의 수행에서 잠재적으로 변경되었습니다.
      bc_write (inode->sector, &inode_disk, 0, BLOCK_SECTOR_SIZE, 0);
    }
  
  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */

      // 경쟁적으로 테이블에 접근할 수 있으므로 락을 취득한 상태에서 수행합니다.
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
          // 루프의 시작 직전과 종료 직후에서 락을 취득한 상태로 유지합니다.
          lock_acquire (&inode->extend_lock);
          break;
        }

      // 섹터 번호가 정해진 이후, 데이터 쓰기 작업은 락을 해제한 상태에서 수행해도 괜찮습니다.
      bc_write (sector_idx, (void *)buffer, bytes_written, chunk_size, sector_ofs);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
      // 다음 byte_to_sector 작업 이전에, 락을 미리 취득합니다.
      lock_acquire (&inode->extend_lock);
    }
  // 마지막으로 락을 해제합니다.
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

// 디스크 아이노드 읽기를 위한 간단한 도움 함수입니다.
static bool
get_disk_inode (const struct inode *inode, struct inode_disk *inode_disk)
{
  return bc_read (inode->sector, inode_disk, 0, sizeof (struct inode_disk), 0);
}

// 파일 시작에서부터 블럭 단위로 잰 위치를 입력받고,
// 어떤 테이블의 어떤 위치에서 찾을 수 있는지를 반환합니다.
static void
locate_byte (off_t pos, struct sector_location *sec_loc)
{
  // 바이트 단위 거리를 블럭 단위로 변환합니다.
  off_t pos_sector = pos / BLOCK_SECTOR_SIZE;

  // 기본값을 오류로 설정
  sec_loc->directness = OUT_LIMIT;

  if (pos_sector < DIRECT_BLOCK_ENTRIES)
    {
      // 디스크 아이노드에서 직접 참조
      sec_loc->directness = NORMAL_DIRECT;
      sec_loc->index1 = pos_sector;
    }
  else if ((pos_sector -= DIRECT_BLOCK_ENTRIES) < INDIRECT_BLOCK_ENTRIES)
    {
      // 한 단계 참조
      sec_loc->directness = INDIRECT;
      sec_loc->index1 = pos_sector;
    }
  else if ((pos_sector -= INDIRECT_BLOCK_ENTRIES) < INDIRECT_BLOCK_ENTRIES * INDIRECT_BLOCK_ENTRIES)
    {
      // 두 단계 참조
      sec_loc->directness = DOUBLE_INDIRECT;
      // index2 이후 index1 순서입니다. 이 순서는 다른 부분의 코드를 간단하게 합니다.
      sec_loc->index2 = pos_sector / INDIRECT_BLOCK_ENTRIES;
      sec_loc->index1 = pos_sector % INDIRECT_BLOCK_ENTRIES;
    }
}

// 주어진 테이블 유형의 주어진 위치에, 주어진 섹터 번호를 씁니다.
static bool
register_sector (struct inode_disk *inode_disk,
                 block_sector_t new_sector,
                 struct sector_location sec_loc)
{
  struct inode_indirect_block first_block, second_block;

  // 두 단계 참조인 경우, 첫 번째 참조 테이블이 갱신되어야 하는지를 나타내는 플래그입니다.
  bool first_dirty = false;

  // 참조 테이블의 섹터 번호를 저장하고 있는 변수에 대한 포인터입니다.
  // 실행 흐름에 따라서 다양한 장소를 가리킵니다.
  block_sector_t *table_sector = &inode_disk->indirect_block_sec;

  switch (sec_loc.directness)
    {
    case NORMAL_DIRECT:
      // 디스크 아이노드 직접 참조입니다.
      inode_disk->direct_map_table[sec_loc.index1] = new_sector;
      return true;
    case DOUBLE_INDIRECT:
      // 두 단계 참조가 일어납니다.
      table_sector = &inode_disk->double_indirect_block_sec;
      if (*table_sector == (block_sector_t) -1)
        {
          // 두 단계 참조 테이블을 처음으로 사용하는 경우입니다.
          if (!free_map_allocate (1, table_sector))
            return false;
          // unsigned 정수의 가장 큰 값을 유효하지 않은 섹터 번호를 나타내기 위하여 예약하기로 합니다.
          memset (&first_block, -1, sizeof (struct inode_indirect_block));
        }
      else
        {
          // 두 단계 참조 테이블이 이미 존재하는 경우입니다. 테이블을 읽습니다.
          if (!bc_read (*table_sector, &first_block, 0, sizeof (struct inode_indirect_block), 0))
            return false;
        }
      // 메모리에 읽은 두 단계 테이블에서, 다음 테이블에 대한 섹터 번호를 저장하고 있는 변수에 대한 포인터
      table_sector = &first_block.map_table[sec_loc.index2];

      // 더러움 플래그가 활성화되는 경우는 마지막 단계 테이블이 할당되지 않은 경우입니다.
      // 마지막 단계 테이블의 섹터 번호는 첫 단계 테이블에 저장되므로 첫 단계 테이블을 다시 쓸 필요가 있기 때문입니다.
      if (*table_sector == (block_sector_t) -1)
          first_dirty = true;
    case INDIRECT:
      // 여기에서 table_sector는 한 단계 테이블의 유일한 테이블 또는 두 단계 테이블의 마지막 테이블을 가리킵니다.
      if (*table_sector == (block_sector_t) -1)
        {
          // 테이블이 없는 경우에 할당하고
          if (!free_map_allocate (1, table_sector))
            return false;
          memset (&second_block, -1, sizeof (struct inode_indirect_block));
        }
      else
        {
          // 테이블이 있다면 읽습니다.
          if (!bc_read (*table_sector, &second_block, 0, sizeof (struct inode_indirect_block), 0))
            return false;
        }
      if (second_block.map_table[sec_loc.index1] == (block_sector_t) -1)
        second_block.map_table[sec_loc.index1] = new_sector;
      else
        // 여기에 도달할 수 없습니다.
        NOT_REACHED ();

      // 첫 단계 테이블이 더러운 경우에 다시 씁니다.
      if (first_dirty)
        {
          if (!bc_write (inode_disk->double_indirect_block_sec, &first_block, 0, sizeof (struct inode_indirect_block), 0))
            return false;
        }
      // 마지막 단계 테이블은 항상 다시 씁니다.
      if (!bc_write (*table_sector, &second_block, 0, sizeof (struct inode_indirect_block), 0))
        return false;
      return true;
    default:
      return false;
    }
  NOT_REACHED ();
}

// 파일의 이전 크기와 새로운 크기를 입력받아, 추가되어야 할 블럭을 추가합니다.
static bool
inode_update_file_length (struct inode_disk *inode_disk, off_t length, off_t new_length)
{
  static char zeros[BLOCK_SECTOR_SIZE];

  // 이전 크기와 새로운 크기가 같다면 즉시 작업을 완료한 것으로 처리합니다.
  if (length == new_length)
    return true;
  // 파일 크기를 줄이는 작업은 무효입니다.
  if (length > new_length)
    return false;
  
  ASSERT (length < new_length);

  inode_disk->length = new_length;

  // [length, new_length) 범위를
  // [length, new_length] 범위로 바꿉니다.
  new_length--;

  // 블럭의 시작 위치로 위치를 정리합니다.
  length = length / BLOCK_SECTOR_SIZE * BLOCK_SECTOR_SIZE;
  new_length = new_length / BLOCK_SECTOR_SIZE * BLOCK_SECTOR_SIZE;

  for (; length <= new_length; length += BLOCK_SECTOR_SIZE)
    {
      struct sector_location sec_loc;

      block_sector_t sector = byte_to_sector (inode_disk, length);
      
      // 유효한 섹터 번호를 얻었다면 새로 할당할 필요가 없습니다.
      if (sector != (block_sector_t) -1)
        continue;
      
      // 파일 데이터가 저장되는 새로운 섹터를 얻습니다.
      if (!free_map_allocate (1, &sector))
        return false;
      // 섹터 정보가 저장되어야 하는 테이블의 종류와 그 테이블에서의 위치를 얻고
      locate_byte (length, &sec_loc);
      // 테이블에 새로운 섹터 정보를 씁니다.
      if (!register_sector (inode_disk, sector, sec_loc))
        return false;
      // 새로운 섹터가 0으로 초기화되도록 합니다.
      if (!bc_write (sector, zeros, 0, BLOCK_SECTOR_SIZE, 0))
        return false; 
    }
  return true;
}

// sector가 마지막 단계 참조 테이블을 가리키는 섹터 번호일 때, 해제 작업을 수행합니다.
static void
free_sectors (block_sector_t sector)
{
  int index;
  struct inode_indirect_block block;
  // 테이블을 읽습니다.
  bc_read (sector, &block, 0, sizeof (struct inode_indirect_block), 0);
  for (index = 0; index < INDIRECT_BLOCK_ENTRIES; index++)
    {
      // 테이블은 순서대로 사용하므로, 유효하지 않은 항목이 처음으로 나왔을 때 종료합니다.
      if (block.map_table[index] == (block_sector_t) -1)
        return;
      // 데이터 섹터를 해제합니다.
      free_map_release (block.map_table[index], 1);
    }
}

// 아이노드에 연관된 블럭들을 모두 해제합니다.
static void
free_inode_sectors (struct inode_disk *inode_disk)
{
  // 디스크 아이노드가 직접 참조하는 모든 데이터 섹터를 해제합니다.
  int index;
  for (index = 0; index < DIRECT_BLOCK_ENTRIES; index++)
    {
      // 테이블은 순서대로 사용하므로, 유효하지 않은 항목이 처음으로 나왔을 때 종료합니다.
      if (inode_disk->direct_map_table[index] == (block_sector_t) -1)
        return;
      // 데이터 섹터를 해제합니다.
      free_map_release (inode_disk->direct_map_table[index], 1);
    }
  // 한 단계 참조 테이블이 없다면 종료합니다.
  if (inode_disk->indirect_block_sec == (block_sector_t) -1)
    return;
  // 한 단계 참조 테이블이 가리키는 모든 데이터 섹터를 해제합니다.
  free_sectors (inode_disk->indirect_block_sec);
  // 한 단계 참조 테이블 그 자체를 해제합니다.
  free_map_release (inode_disk->indirect_block_sec, 1);

  // 두 단계 참조 테이블이 없다면 종료합니다.
  if (inode_disk->double_indirect_block_sec == (block_sector_t) -1)
    return;

  // 두 단계 참조 테이블을 순회합니다.
  struct inode_indirect_block block;
  bc_read (inode_disk->double_indirect_block_sec, &block, 0, sizeof (struct inode_indirect_block), 0);
  for (index = 0; index < DIRECT_BLOCK_ENTRIES; index++)
  {
    // 테이블은 순서대로 사용하므로, 유효하지 않은 항목이 처음으로 나왔을 때 종료합니다.
    if (block.map_table[index] == (block_sector_t) -1)
      return;
    // 두 단계 참조 테이블이 가리키는 마지막 단계 참조 테이블을, 같은 방법으로 해제합니다.
    free_sectors (block.map_table[index]);
    // 두 단계 참조 테이블이 가리키는 마지막 단계 참조 테이블 그 자체를 해제합니다.
    free_map_release (block.map_table[index], 1);
  }
  // 두 단계 참조 테이블을 그 자체를 해제합니다.
  free_map_release (inode_disk->double_indirect_block_sec, 1);
}

off_t
inode_length (const struct inode *inode)
{
  struct inode_disk inode_disk;
  bc_read (inode->sector, &inode_disk, 0, BLOCK_SECTOR_SIZE, 0);
  return inode_disk.length;
}

// 주어진 아이노드가 디렉터리이면 true, 그렇지 않으면 false를 반환합니다.
bool
inode_is_dir (const struct inode *inode)
{
  struct inode_disk inode_disk;
  if (inode->removed)
    return false;
  if (!get_disk_inode (inode, &inode_disk))
    return false;
  return inode_disk.is_dir;
}
