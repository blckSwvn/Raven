#include <sys/mman.h>
#include <string.h>


typedef struct master {
	void *base;
	void *tail;
	void *end;
} master;


void *arena_init(size_t size){
	void *start = mmap(NULL, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	static _Thread_local master m;
	m.base = start;
	m.tail = start;
	m.end = (char *)start + size;
	return &m;
}


static inline master *get_master(){
	static _Thread_local master m;
	return &m;
}


void *arena_alloc(size_t size){
	master *m = get_master();

	if((char *)m->tail + size > (char *)m->end) return NULL;

	void *ptr = m->tail;
	m->tail = (char *)m->tail + size;
	return ptr;
}


void arena_reset(){
	master *m = get_master();
	m->tail = m->base;
}


void arena_free(){
	master *m = get_master();
	munmap(m->base, (char *)m->end - (char *)m->base);
	m->end = NULL;
	m->base = NULL;
	m->tail = NULL;
}
