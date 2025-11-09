#ifndef ARENA_H
#define ARENA_H

#include <sys/mman.h>
#include <string.h>

void *arena_init(size_t size);
void *arena_alloc(size_t size);
void arena_reset();
void arena_free();

#endif
