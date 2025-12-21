#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <string.h>


typedef struct {
	void *base;
	void *tail;
	void *end;
} master;

static _Thread_local master m; 

void *arena_init(size_t size){
	void *start = mmap(NULL, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	m.base = start;
	m.tail = start;
	m.end = (char *)start + size;
	return &m;
}


void *arena_alloc(size_t size){
	const size_t align = alignof(max_align_t);
	uintptr_t x = (uintptr_t)m.tail;
	void *aligned = (void *)((x + (align - 1)) & ~ (align -1));
	if((char *)aligned + size > (char *)m.end) return NULL;

	m.tail = (char *)aligned + size;
	return aligned;
}


void arena_reset(){
	m.tail = m.base;
}


void arena_free(void){
	munmap(m.base, (char *)m.end - (char *)m.base);
	m.end = NULL;
	m.base = NULL;
	m.tail = NULL;
}
