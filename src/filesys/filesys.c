#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/buffer_cache.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  // 버퍼 캐시 초기화
  bc_init ();

  inode_init ();
  free_map_init ();
  if (format) 
    do_format ();
  free_map_open ();

  // 현재 스레드의 작업 디렉터리를 루트로 설정합니다.
  thread_current ()->working_dir = dir_open_root ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();

  // 버퍼 캐시 종료
  bc_term ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *path, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  char name[PATH_MAX_LEN + 1];
  struct dir *dir = parse_path (path, name);

  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, 0)
                  && dir_add (dir, name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *path)
{
  char name[PATH_MAX_LEN + 1];
  struct dir *dir = parse_path (path, name);
  if (dir == NULL)
    return NULL;
  struct inode *inode = NULL;
  dir_lookup (dir, name, &inode);
  dir_close (dir);
  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *path) 
{
  char name[PATH_MAX_LEN + 1];
  struct dir *dir = parse_path (path, name);

  struct inode *inode;
  dir_lookup (dir, name, &inode);

  struct dir *cur_dir = NULL;
  char temp[PATH_MAX_LEN + 1];

  bool success = false;
  if (!inode_is_dir (inode) ||
    ((cur_dir = dir_open (inode)) && !dir_readdir (cur_dir, temp)))
    success = dir != NULL && dir_remove (dir, name);
  dir_close (dir);
  
  if (cur_dir)
    dir_close (cur_dir);
  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");

  struct dir *root = dir_open_root ();
  dir_add (root, ".", ROOT_DIR_SECTOR);
  dir_add (root, "..", ROOT_DIR_SECTOR);  
  dir_close (root);

  free_map_close ();
  printf ("done.\n");
}

struct dir *
parse_path (const char *path_o, char *file_name)
{
  struct dir *dir = NULL;

  // 기본 예외 처리
  if (!path_o || !file_name)
    return NULL;
  if (strlen (path_o) == 0)
    return NULL;

  char path[PATH_MAX_LEN + 1];
  strlcpy (path, path_o, PATH_MAX_LEN);

  if (path[0] == '/')
    dir = dir_open_root ();
  else
    dir = dir_reopen (thread_current ()->working_dir);

  // 아이노드가 어떤 이유로 제거되었거나 디렉터리가 아닌 경우
  if (!inode_is_dir (dir_get_inode (dir)))
    return NULL;

  char *token, *next_token, *save_ptr;
  token = strtok_r (path, "/", &save_ptr);
  next_token = strtok_r (NULL, "/", &save_ptr);

  if (token == NULL)
    {
      strlcpy (file_name, ".", PATH_MAX_LEN);
      return dir;
    }

  while (token && next_token)
    {
      struct inode *inode = NULL;
      if (!dir_lookup (dir, token, &inode))
        {
          dir_close (dir);
          return NULL;
        }
      if (!inode_is_dir (inode))
        {
          dir_close (dir);
          return NULL;
        }
      dir_close (dir);
      dir = dir_open (inode);

      token = next_token;
      next_token = strtok_r (NULL, "/", &save_ptr);
    }
  strlcpy (file_name, token, PATH_MAX_LEN);
  return dir;
}

bool
filesys_create_dir (const char *path)
{
  block_sector_t inode_sector = 0;
  char name[PATH_MAX_LEN + 1];
  struct dir *dir = parse_path (path, name);

  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && dir_create (inode_sector, 16)
                  && dir_add (dir, name, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);

  if (success)
    {
      struct dir *new_dir = dir_open (inode_open (inode_sector));
      dir_add (new_dir, ".", inode_sector);
      dir_add (new_dir, "..", inode_get_inumber (dir_get_inode (dir)));
      dir_close (new_dir);
    }
  dir_close (dir);
  return success;
}
