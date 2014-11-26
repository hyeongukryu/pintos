#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stddef.h>

void swap_init (size_t);
void swap_in (size_t, void *);
size_t swap_out (void *);

#endif