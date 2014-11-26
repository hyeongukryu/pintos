#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "syscall.h"
#include "vm/page.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

int process_add_file (struct file *);
struct file *process_get_file (int);
void process_close_file (int);

bool handle_mm_fault (struct vm_entry *);
void do_mummap (struct mmap_file *);
struct mmap_file *find_mmap_file (int);

#endif /* userprog/process.h */
