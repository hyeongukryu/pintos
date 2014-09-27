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

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

// 주소 addr이 유저 모드 주소가 아니면 프로세스를 종료합니다.
// 시스템 콜을 안전하게 수행하기 위하여 사용합니다.
static inline void
check_address (void *addr)
{
  if ((is_user_vaddr (addr) && addr >= (void *)0x08048000UL) == false)
    exit (-1);
}

static inline void
check_address4 (void *addr)
{
  check_address (addr);
  check_address (addr + 3);
}

static void
get_arguments (int32_t *esp, int32_t *args, int count)
{
  ASSERT (1 <= count && count <= 4);
  while (count--)
  {
    check_address4 (++esp);
    *(args++) = *esp;
  }
}

static void
check_user_string (const char *str)
{
  for (; check_address ((void *)str), *str; str++);
}

static char *
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

#define free_single_user_string(args, flag, index) do { \
  if (flag & (0b1000 >> index)) \
  { \
    free (args[index]); \
    args[index] = 0; \
  }\
} while (0)

static void
free_user_strings (char **args, int flag)
{
  ASSERT (0 <= flag && flag <= 0b1111);
  free_single_user_string(args, flag, 0);
  free_single_user_string(args, flag, 1);
  free_single_user_string(args, flag, 2);
  free_single_user_string(args, flag, 3);
}

#define get_single_user_string(args, flag, index) do { \
  if (flag & (0b1000 >> index)) \
  { \
    args[index] = get_user_string (args[index]); \
    if (!args[index]) \
    { \
      free_user_strings (args, flag & (0b11110000 >> index)); \
      exit(-1); \
    } \
  } \  
} while (0)

#define check_single_user_string(args, flag, index) do { \
  if (flag & (0b1000 >> index)) { \
    check_user_string (args[index]); \
  } \
} while (0)

static void 
get_user_strings (char **args, int flag)
{
  ASSERT (0 <= flag && flag <= 0b1111);
  check_single_user_string(args, flag, 0);
  check_single_user_string(args, flag, 1);
  check_single_user_string(args, flag, 2);
  check_single_user_string(args, flag, 3);
  get_single_user_string(args, flag, 0);
  get_single_user_string(args, flag, 1);
  get_single_user_string(args, flag, 2);
  get_single_user_string(args, flag, 3);
}


static void
syscall_handler (struct intr_frame *f) 
{
  int32_t args[4];
  int32_t *esp = f->esp;
  check_address4(esp);

  switch (*esp)
  {
    case SYS_HALT:
      halt ();
      break;
    case SYS_EXIT:
      get_arguments (esp, args, 1);
      exit (args[0]);
      break;
    case SYS_CREATE:
      get_arguments (esp, args, 2);
      get_user_strings ((char **)args, 0b1000);
      f->eax = create ((const char *)args[0], args[1]);
	  free_user_strings ((char **)args, 0b1000);
      break;
    case SYS_REMOVE:
      get_arguments (esp, args, 1);
      get_user_strings ((char **)args, 0b1000);
      f->eax = remove ((const char *)args[0]);
      free_user_strings ((char **)args, 0b1000); 
      break;
    case SYS_EXEC:
    case SYS_WAIT:
    case SYS_OPEN:
    case SYS_FILESIZE:
    case SYS_READ:
    case SYS_WRITE:
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
      printf("NotImplemented: %d\n", *esp);
    default:
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

