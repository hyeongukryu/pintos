#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include <string.h>

static void syscall_handler (struct intr_frame *);

// 커널에서 시스템 콜을 실제로 처리합니다.
// 이 함수들은 포인터가 가리키는 값에 안전하게 접근할 수 있다고 가정합니다.
static void halt (void);
static void exit (int);
static bool create (const char *file, unsigned initial_size);
static bool remove (const char *file);
static int write (int, const void *, unsigned);

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

// 주소 addr이 유효한 유저 모드 주소가 아니면 프로세스를 종료합니다.
// 시스템 콜을 안전하게 수행하기 위하여 사용합니다.
static inline void
check_address (void *addr)
{
  // TODO: 가상 메모리를 고려합니다.
  if ((is_user_vaddr (addr) && addr >= (void *)0x08048000UL) == false)
    exit (-1);
}

static inline void
check_address4 (void *addr)
{
  check_address (addr);
  check_address (addr + 3);
}

// 4바이트 인자를 1개에서 4개 사이에서 가져옵니다.
static inline void
get_arguments (int32_t *esp, int32_t *args, int count)
{
  ASSERT (1 <= count && count <= 4);
  while (count--)
  {
    check_address4 (++esp);
    *(args++) = *esp;
  }
}

// 사용자 문자열의 유효성을 확인합니다.
static inline void
check_user_string (const char *str)
{
  for (; check_address ((void *)str), *str; str++);
}

// 사용자 문자열을 가져옵니다. 새로운 메모리를 동적 할당합니다.
static inline char *
get_user_string (const char *str)
{
  int size = strlen (str) + 1;
  char *buffer = 0;
  buffer = malloc (size);
  if (!buffer)
    return 0;
  strlcpy (buffer, str, size);
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

// 플래그의 마지막 4비트 설정에 따라서 문자열들을 해제합니다.
static inline void
free_user_strings (char **args, int flag)
{
  ASSERT (0 <= flag && flag <= 0b1111);
  free_single_user_string (args, flag, 0);
  free_single_user_string (args, flag, 1);
  free_single_user_string (args, flag, 2);
  free_single_user_string (args, flag, 3);
}

// 플래그에 맞을 때 사용자 문자열을 복사합니다.
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
check_single_user_string (char **args, int flag, int index)
{
  if (flag & (0b1000 >> index))
    check_user_string (args[index]);
}

// 플래그의 마지막 4비트에 따라서 사용자 문자열을 확인하고 가져옵니다.
// 새로운 메모리를 동적 할당합니다. 
static inline void 
get_user_strings (char **args, int flag)
{
  ASSERT (0 <= flag && flag <= 0b1111);
  check_single_user_string (args, flag, 0);
  check_single_user_string (args, flag, 1);
  check_single_user_string (args, flag, 2);
  check_single_user_string (args, flag, 3);
  get_single_user_string (args, flag, 0);
  get_single_user_string (args, flag, 1);
  get_single_user_string (args, flag, 2);
  get_single_user_string (args, flag, 3);
}

static void
syscall_handler (struct intr_frame *f)
{
  int32_t args[4];
  check_address4(f->esp);

  switch (*(int *)f->esp)
    {
      case SYS_HALT:
        halt ();
        break;
      case SYS_EXIT:
        get_arguments (f->esp, args, 1);
        exit (args[0]);
        break;
      case SYS_CREATE:
        get_arguments (f->esp, args, 2);
        get_user_strings ((char **)args, 0b1000);
        f->eax = create ((const char *)args[0], args[1]);
  	    free_user_strings ((char **)args, 0b1000);
        break;
      case SYS_REMOVE:
        get_arguments (f->esp, args, 1);
        get_user_strings ((char **)args, 0b1000);
        f->eax = remove ((const char *)args[0]);
        free_user_strings ((char **)args, 0b1000); 
        break;
      case SYS_WRITE:
        get_arguments (f->esp, args, 3);
        get_user_strings ((char **)args, 0b0100);
        f->eax = write((int)args[0], (const void *)args[1], (unsigned)args[2]);
        free_user_strings ((char **)args, 0b0100);
        break;
      case SYS_EXEC:
      case SYS_WAIT:
      case SYS_OPEN:
      case SYS_FILESIZE:
      case SYS_READ:
      case SYS_SEEK:
      case SYS_TELL:
      case SYS_CLOSE:
      case SYS_MMAP:
      case SYS_MUNMAP:
      case SYS_CHDIR:
      case SYS_MKDIR:
      case SYS_READDIR:
      case SYS_ISDIR: 
      case SYS_INUMBER:
        // 시스템 콜 번호는 유효하나 아직 구현되지 않았습니다.
        printf("NotImplemented: %d\n", *(int *)f->esp);
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

static void
exit (int status)
{
  // TODO: 종료 상태 코드를 기록할 수 있게 하는 메커니즘이 필요합니다.
  printf ("%s: exit(%d)\n", thread_name (), status);
  thread_exit ();
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
write (int fd, const void *buffer, unsigned size)
{
  if (fd != 1)
    return 0;
  putbuf(buffer, size);
  return size;
}

