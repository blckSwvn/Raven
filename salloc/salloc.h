#ifndef SALLOC_H
#define SALLOC_H

#include <stdbool.h>
#include <stddef.h>

typedef struct node node;
typedef struct master master;

struct node {
    bool dead;
    size_t length;
    node *next;
    node *prev;
    char data[];
};

struct master {
    void *base;
    size_t memUsed;
    size_t memFree;
    node *freelist;
    node *tail;
};

// API functions
bool sinit(master *m, size_t requested, bool use_mmap);
void sfree(master *m, void *ptr);
void *salloc(master *m, size_t requested);
void *srealloc(master *m, void *ptr, size_t requested);

void dump_m(master *m);
void dump_f(master *m);
void dump_a(master *m);

#endif
