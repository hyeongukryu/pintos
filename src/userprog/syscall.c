#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "process.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"
#include "devices/input.h"
#include "vm/page.h"

static void syscall_handler (struct intr_frame *);

// 시스템 콜의 커널 쪽 서비스 루틴입니다.
// 이 함수들은 포인터가 가리키는 영역에 안전하게 접근할 수 있다고 가정합니다.
static void halt (void);
void exit (int);
static tid_t exec (const char *);
static int wait (tid_t);
static bool create (const char *, unsigned);
static bool remove (const char *);
static int open (const char *);
static int filesize (int);
static int read (int, void *, unsigned);
static int write (int, const void *, unsigned);
static void seek (int, unsigned);
static unsigned tell (int);
static void close (int);
static int mmap (int, void *);
static void mummap (int);


// 파일 작업 락입니다.
// 파일 읽기 또는 쓰기 작업을 수행할 때 사용해야 합니다.
struct lock file_lock;

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");

  // 파일 작업 락 초기화
  lock_init (&file_lock);
}

// 주소 addr이 유효한 유저 모드 주소가 아니면 프로세스를 종료합니다.
// 시스템 콜을 안전하게 수행하기 위하여 사용합니다.
static inline void
check_address (void *addr, void *esp)
{
  // 유저 영역 주소인지 확인한 다음, 올바른 가상 주소인지 확인합니다.
  if (!(
        is_user_vaddr (addr) &&
        addr >= (void *)0x08048000UL
     ))
    exit (-1);

  if (!find_vme (addr))
    {
      if (!verify_stack ((int32_t) addr, (int32_t) esp))
        exit (-1);
      expand_stack (addr);      
    }
}

// 4 바이트 값에 대한 안전한 포인터인지 검사합니다.
static inline void
check_address4 (void *addr, void *esp)
{
  check_address (addr, esp);
  check_address (addr + 3, esp);
}

// 4바이트 인자를 1개에서 4개 사이에서 가져옵니다.
static inline void
get_arguments (int32_t *esp, int32_t *args, int count, void *esp2)
{
  ASSERT (1 <= count && count <= 4);
  while (count--)
  {
    check_address4 (++esp, esp2);
    *(args++) = *esp;
  }
}

// 아래 함수와 같으며, 주어진 크기를 가정합니다.
static inline void
check_user_string_l (const char *str, unsigned size, void *esp)
{
  while (size--)
    check_address ((void *) (str++), esp);  
}

// 널 문자로 종료되는 사용자 문자열의 유효성을 확인합니다.
static inline void
check_user_string (const char *str, void *esp)
{
  for (; check_address ((void *) str, esp), *str; str++);
}

// 아래 함수와 같으며, 주어진 크기를 가정합니다.
static inline char *
get_user_string_l (const char *str, unsigned size)
{
  char *buffer = 0;
  buffer = malloc (size);
  if (!buffer)
    return 0;
  memcpy (buffer, str, size);
  return buffer;
}

// 사용자 문자열을 가져옵니다. 새로운 메모리를 동적 할당합니다.
static inline char *
get_user_string (const char *str)
{
  unsigned size;
  char *buffer;
  size = strlen (str) + 1;
  buffer = get_user_string_l (str, size);
  return buffer; 
}

// 플래그가 맞을 때 동적 할당된 문자열을 해제하고 널 포인터를 대입합니다.
static inline void
free_single_user_string (char **args, int flag, int index)
{
  if (flag & (0b1000 >> index))
    {
      free (args[index]);
      args[index] = 0;
    }
}

// 플래그의 마지막 4비트에 따라서 문자열들을 해제합니다.
static inline void
free_user_strings (char **args, int flag)
{
  ASSERT (0 <= flag && flag <= 0b1111);
  free_single_user_string (args, flag, 0);
  free_single_user_string (args, flag, 1);
  free_single_user_string (args, flag, 2);
  free_single_user_string (args, flag, 3);
}

// 플래그가 맞을 때 사용자 문자열을 복사합니다.
// 작업 중 실패하면 내용을 되돌리고 종료합니다.
// 유효성을 검증한 다음 이 작업을 실행해야 합니다.
static inline void
get_single_user_string (char **args, int flag, int index)
{
  if (flag & (0b1000 >> index))
    {
      args[index] = get_user_string (args[index]);
      if (!args[index])
        {
          free_user_strings (args, flag & (0b11110000 >> index));
          exit(-1);
        }
    }
}

// 플래그가 맞을 때 사용자 문자열의 유효성을 확인합니다.
// 유효하지 않으면 종료합니다.
static inline void
check_single_user_string (char **args, int flag, int index, int32_t esp)
{
  if (flag & (0b1000 >> index))
    check_user_string (args[index], esp);
}

// 플래그의 마지막 4비트에 따라서 사용자 문자열을 확인하고 가져옵니다.
// 새로운 메모리를 동적 할당합니다. 
static inline void
get_user_strings (char **args, int flag, void *esp)
{
  ASSERT (0 <= flag && flag <= 0b1111);
  check_single_user_string (args, flag, 0, esp);
  check_single_user_string (args, flag, 1, esp);
  check_single_user_string (args, flag, 2, esp);
  check_single_user_string (args, flag, 3, esp);
  get_single_user_string (args, flag, 0);
  get_single_user_string (args, flag, 1);
  get_single_user_string (args, flag, 2);
  get_single_user_string (args, flag, 3);
}

static void
pin_address (void *addr, bool write)
{
  struct vm_entry *vme = find_vme (addr);
  if (write && !vme->writable)
    exit (-1);
  vme->pinned = true;
  if (vme->is_loaded == false)
    handle_mm_fault (vme);
}

static void
unpin_address (void *addr)
{
  find_vme (addr)->pinned = false;
}

static void
pin_string (const char *begin, const char *end, bool write)
{
  for (; begin < end; begin += PGSIZE)
    pin_address (begin, write);
}

static void
unpin_string (const char *begin, const char *end)
{
  for (; begin < end; begin += PGSIZE)
    unpin_address (begin);
}

static void
syscall_handler (struct intr_frame *f)
{
  int32_t args[4];
  check_address4 (f->esp, f->esp);

  switch (*(int *) f->esp)
    {
      case SYS_HALT:
        halt ();
        break;
      case SYS_EXIT:
        get_arguments (f->esp, args, 1, f->esp);
        exit (args[0]);
        break;
      case SYS_CREATE:
        get_arguments (f->esp, args, 2, f->esp);
        get_user_strings ((char **) args, 0b1000, f->esp);
        f->eax = create ((const char *) args[0], args[1]);
        free_user_strings ((char **) args, 0b1000);
        break;
      case SYS_REMOVE:
        get_arguments (f->esp, args, 1, f->esp);
        get_user_strings ((char **) args, 0b1000, f->esp);
        f->eax = remove ((const char *) args[0]);
        free_user_strings ((char **) args, 0b1000); 
        break;
      case SYS_WRITE:
        get_arguments (f->esp, args, 3, f->esp);
        check_user_string_l ((const char *) args[1], (unsigned) args[2], f->esp);
        args[1] = (int) get_user_string_l ((const char *) args[1], (unsigned) args[2]);
        f->eax = write ((int) args[0], (const void *) args[1], (unsigned) args[2]);
        free ((void *) args[1]);
        args[1] = 0;
        break;
      case SYS_EXEC:
        get_arguments (f->esp, args, 1, f->esp);
        get_user_strings ((char **) args, 0b1000, f->esp);
        f->eax = exec ((const char *) args[0]);
        free_user_strings ((char **) args, 0b1000);
        break;
      case SYS_WAIT:
        get_arguments (f->esp, args, 1, f->esp);
        f->eax = wait ((tid_t) args[0]);
        break;
      case SYS_OPEN:
        get_arguments (f->esp, args, 1, f->esp);
        get_user_strings ((char **) args, 0b1000, f->esp);
        f->eax = open ((const char *) args[0]);
        free_user_strings ((char **) args, 0b1000);
        break;
      case SYS_FILESIZE:
        get_arguments (f->esp, args, 1, f->esp);
        f->eax = filesize ((int) args[0]);
        break;
      case SYS_READ:
        get_arguments (f->esp, args, 3, f->esp);
        check_user_string_l ((const char *) args[1], (unsigned) args[2], f->esp);
        f->eax = read ((int) args[0], (void *) args[1], (unsigned) args[2]);
        break;
      case SYS_SEEK:
        get_arguments (f->esp, args, 2, f->esp);
        seek ((int) args[0], (unsigned) args[1]);
        break;
      case SYS_TELL:
        get_arguments (f->esp, args, 1, f->esp);
        f->eax = tell ((int) args[0]);
        break;
      case SYS_CLOSE:
        get_arguments (f->esp, args, 1, f->esp);
        close ((int) args[0]);
        break;
      case SYS_MMAP:
        get_arguments (f->esp, args, 2, f->esp);
        f->eax = mmap ((int) args[0], (void *) args[1]);
        break;
      case SYS_MUNMAP:
        get_arguments (f->esp, args, 1, f->esp);
        mummap ((int) args[0]);
        break;
      case SYS_MKDIR:
        break;
      case SYS_CHDIR:
      case SYS_READDIR:
      case SYS_ISDIR: 
      case SYS_INUMBER:
        // 시스템 콜 번호는 유효하나 아직 구현되지 않았습니다.
        printf("NotImplemented: %d\n", *(int *)f->esp);
        break;
      default:
        // 시스템 콜 번호가 유효하지 않습니다.
        exit(-1);
    }
}

static void
halt (void)
{
  shutdown_power_off ();
}

void
exit (int status)
{
  thread_current ()->exit_status = status;
  printf ("%s: exit(%d)\n", thread_name (), status);
  thread_exit ();
}

static tid_t
exec (const char *file)
{
  tid_t tid;
  struct thread *child;

  // 여기에서 실패하면 스레드 자료 구조 생성 실패입니다.
  if ((tid = process_execute (file)) == TID_ERROR)
    return TID_ERROR;

  child = thread_get_child (tid);
  ASSERT (child);

  sema_down (&child->load_sema);

  // 여기에서 실패하면 프로그램 적재 실패입니다.
  if (!child->load_succeeded)
    return TID_ERROR;

  return tid;
}

static int
wait (tid_t tid)
{
  return process_wait (tid);
}

static bool
create (const char *file, unsigned initial_size)
{
  return filesys_create (file, initial_size); 
}

static bool
remove (const char *file)
{
  return filesys_remove (file);
}

static int
open (const char *file)
{
  int result = -1;
  lock_acquire (&file_lock);
  // process_add_file는 NULL에서 -1 반환하므로 안전합니다.
  result = process_add_file (filesys_open (file));
  lock_release (&file_lock);
  return result;
}

static int
filesize (int fd)
{
  struct file *f = process_get_file (fd);
  if (f == NULL)
    return -1;
	return file_length (f);
}

static int
read (int fd, void *buffer, unsigned size)
{
  struct file *f;
  pin_string (buffer, buffer + size, true);
  lock_acquire (&file_lock);

  if (fd == STDIN_FILENO)
  {
    // 표준 입력
    unsigned count = size;
    while (count--)
      *((char *)buffer++) = input_getc();
    lock_release (&file_lock);
    unpin_string (buffer, buffer + size);
    return size;
  }
  if ((f = process_get_file (fd)) == NULL)
    {
      lock_release (&file_lock);
      unpin_string (buffer, buffer + size);
      return -1;
    }
  size = file_read (f, buffer, size);
  lock_release (&file_lock);
  unpin_string (buffer, buffer + size);
  return size;
}

static int
write (int fd, const void *buffer, unsigned size)
{
  struct file *f;
  lock_acquire (&file_lock);
  if (fd == STDOUT_FILENO)
    {
      putbuf (buffer, size);
      lock_release (&file_lock);
      return size;  
    }
  if ((f = process_get_file (fd)) == NULL)
    {
      lock_release (&file_lock);
      return 0;
    }
  size = file_write (f, buffer, size);
  lock_release (&file_lock);
  return size;
}

static void
seek (int fd, unsigned position)
{
  struct file *f = process_get_file (fd);
  if (f == NULL)
    return;
  file_seek (f, position);  
}

static unsigned
tell (int fd)
{
  struct file *f = process_get_file (fd);
  if (f == NULL)
    exit (-1);
  return file_tell (f);
}

static void
close (int fd)
{ 
  process_close_file (fd);
}

static int
mmap (int fd, void *addr)
{
  struct mmap_file *mmap_file;
  size_t offset = 0;

  if (pg_ofs (addr) != 0 || !addr)
    return -1;
  if (is_user_vaddr (addr) == false)
    return -1;
  mmap_file = (struct mmap_file *)malloc (sizeof (struct mmap_file));
  if (mmap_file == NULL)
    return -1;
  memset (mmap_file, 0, sizeof(struct mmap_file));
  list_init (&mmap_file->vme_list);
  if (!(mmap_file->file = process_get_file (fd)))
    return -1;
  mmap_file->file = file_reopen(mmap_file->file);
  mmap_file->mapid = thread_current ()->next_mapid++;
  list_push_back (&thread_current ()->mmap_list, &mmap_file->elem);

  int length = file_length (mmap_file->file);
  while (length > 0)
    {
      if (find_vme (addr))
        return -1;

      struct vm_entry *vme = (struct vm_entry *)malloc (sizeof (struct vm_entry));
      memset (vme, 0, sizeof (struct vm_entry));
      vme->type = VM_FILE;
      vme->writable = true;
      vme->vaddr = addr;
      vme->offset = offset;
      vme->read_bytes = length < PGSIZE ? length : PGSIZE;
      vme->zero_bytes = 0;
      vme->file = mmap_file->file;

      list_push_back (&mmap_file->vme_list, &vme->mmap_elem);
      insert_vme (&thread_current ()->vm, vme);
      addr += PGSIZE;
      offset += PGSIZE;
      length -= PGSIZE;
    }
  return mmap_file->mapid;
}

static void
mummap (int mapid)
{
  struct mmap_file *f = find_mmap_file (mapid);
  if (!f)
    return;
  do_mummap (f);
}
