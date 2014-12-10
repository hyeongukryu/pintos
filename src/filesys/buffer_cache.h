#ifndef FILESYS_BUFFER_CACHE_H
#define FILESYS_BUFFER_CACHE_H

#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/synch.h"

struct buffer_head
  {
  	// 더러운지를 나타냅니다.
    bool dirty;
    // 사용 중인지를 나타냅니다.
    bool valid;
    // 캐시된 섹터 번호입니다.
    block_sector_t address;
    // clock 알고리즘에서 사용합니다.
    bool clock;
    // 쓰기 작업을 하기 전에 이 락을 획득합니다.
    struct lock lock;
    // 데이터 버퍼를 가리킵니다.
    void *buffer;
  };

void bc_init (void);
void bc_term (void);
bool bc_read (block_sector_t, void *, off_t, int, int);
bool bc_write (block_sector_t, void *, off_t, int, int);
struct buffer_head *bc_lookup (block_sector_t);
struct buffer_head *bc_select_victim (void);
void bc_flush_entry (struct buffer_head *);
bool bc_ok (void);

#endif