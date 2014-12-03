#include "filesys/buffer_cache.h"
#include "threads/palloc.h"
#include <string.h>
#include <debug.h>

// 캐시할 블록의 수
#define BUFFER_CACHE_ENTRIES 64

// 실제 데이터를 제외한, 캐시 스스로에 대한 설명을 가지고 있습니다.
static struct buffer_head buffer_head[BUFFER_CACHE_ENTRIES];

// 실제 데이터가 저장되는 버퍼입니다.
static char p_buffer_cache[BUFFER_CACHE_ENTRIES * BLOCK_SECTOR_SIZE];

// clock 알고리즘을 위한 시계 바늘입니다.
static struct buffer_head *clock_hand;

// buffer_head 배열에 새로운 값을 추가하거나 뺄 때 중간 과정을 보이지 않게 하는 락입니다.
static struct lock bc_lock;

static void init_head (struct buffer_head *, void *);

// 버퍼 캐시 시스템을 초기화합니다.
void
bc_init (void)
{
  // 바로 위에서 설명한 자료 구조를 각각 초기화합니다.
  struct buffer_head *head;
  void *cache = p_buffer_cache;
  for (head = buffer_head;
       head != buffer_head + BUFFER_CACHE_ENTRIES;
       head++, cache += BLOCK_SECTOR_SIZE)
    init_head (head, cache);
  clock_hand = buffer_head;
  lock_init (&bc_lock);
}

// 하나의 buffer_head를, 주어지는 버퍼를 가리키도록 하여 초기화합니다.
static void
init_head (struct buffer_head *head, void *buffer)
{
  // 더럽지 않고, 아직 유효하지 않은 상태로 초기화합니다.
  memset (head, 0, sizeof (struct buffer_head));
  lock_init (&head->lock);
  head->buffer = buffer;
}

// 버퍼 캐시 시스템을 종료합니다.
void
bc_term (void)
{
  struct buffer_head *head;
  for (head = buffer_head;
       head != buffer_head + BUFFER_CACHE_ENTRIES; head++)
    {
      // 아직 쓰지 않은 모든 더러운 캐시를 써서 정리합니다.
      lock_acquire (&head->lock);
      bc_flush_entry (head);
      lock_release (&head->lock);
    }
}

// 버퍼를 이용하여 읽기 작업을 수행합니다.
bool
bc_read (block_sector_t address, void *buffer,
         off_t offset, int chunk_size, int sector_ofs)
{
  struct buffer_head *head;
  if (!(head = bc_lookup (address)))
    {
      // 여기에 도달하였다면 버퍼에 섹터가 없습니다.
      head = bc_select_victim ();
      // head에 캐시에서 제거할 섹터가 있습니다.
      bc_flush_entry (head);
      head->valid = true;
      head->dirty = false;
      head->address = address;
      // address를 지정하였으므로 락을 해제합니다.
      lock_release (&bc_lock);
      // 실제 읽기 작업
      block_read (fs_device, address, head->buffer);
    }
  head->clock = true;
  // 버퍼에서 읽기 작업
  memcpy (buffer + offset, head->buffer + sector_ofs, chunk_size);
  lock_release (&head->lock);
  return true;
}

// 버퍼를 이용하여 읽기 작업을 수행합니다.
bool
bc_write (block_sector_t address, void *buffer,
         off_t offset, int chunk_size, int sector_ofs)
{
  struct buffer_head *head;
  if (!(head = bc_lookup (address)))
    {
      // 여기에 도달하였다면 버퍼에 섹터가 없습니다.
      head = bc_select_victim ();
      // head에 캐시에서 제거할 섹터가 있습니다.
      bc_flush_entry (head);
      head->valid = true;
      head->address = address;
      // address를 지정하였으므로 락을 해제합니다.
      lock_release (&bc_lock);
      // 실제 읽기 작업
      block_read (fs_device, address, head->buffer);
    }
  head->clock = true;
  // 곧 이 버퍼는 더러워집니다.
  head->dirty = true;
  // 버퍼에서 쓰기 작업
  memcpy (head->buffer + sector_ofs, buffer + offset, chunk_size);
  lock_release (&head->lock);
  return true;
}

struct buffer_head *
bc_lookup (block_sector_t address)
{
  // 버퍼에서 항목을 제거하는 작업의 중간 과정이 보이지 않도록 해야 합니다.
  // 락을 걸지 않으면 한 섹터에 대한 캐시가 여러 개 만들어질 수 있습니다.
  lock_acquire (&bc_lock);
  struct buffer_head *head;
  for (head = buffer_head;
       head != buffer_head + BUFFER_CACHE_ENTRIES; head++)
    {
      if (head->valid && head->address == address)
        {
          // 캐시 적중 상황입니다.
          // 데이터에 접근하기 전에 더 구체적인 락을 획득하고,
          lock_acquire (&head->lock);
          // 처음에 잠근 락을 해제합니다.
          lock_release (&bc_lock);
          return head;
        }
    }
  // 캐시 미스 상황입니다. 이 상황을 유지하기 위해서
  // 처음에 잠근 락이 걸린 상태로 반환합니다.
  return NULL;
}

// clock 알고리즘으로 교체할 캐시를 선택합니다.
struct buffer_head *
bc_select_victim (void)
{
  // 이 루프는 최대 두 번 수행됩니다.
  for (;;)
    {
      for (;
           clock_hand != buffer_head + BUFFER_CACHE_ENTRIES;
           clock_hand++)
        {
          lock_acquire (&clock_hand->lock);
          if (!clock_hand->valid || !clock_hand->clock)
            {
              return clock_hand++;
            }
          clock_hand->clock = false;
          lock_release (&clock_hand->lock);   
        }
      clock_hand = buffer_head;
    }
  NOT_REACHRED ();
}

// 주어진 엔트리가 사용 중인 엔트리이고 더러운 경우에
// 버퍼를 실제로 쓰고, 상태를 깨끗하게 만듭니다.
void
bc_flush_entry (struct buffer_head *entry)
{
  ASSERT (lock_held_by_current_thread (&entry->lock));
  if (!entry->valid || !entry->dirty)
    return;
  entry->dirty = false;
  block_write (fs_device, entry->address, entry->buffer);
}
