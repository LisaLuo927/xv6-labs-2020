// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);
static void kfree_to_cpu(void *pa, int id);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

void
kinit()
{
  for(int i = 0; i < NCPU; i++)
    initlock(&kmem[i].lock, "kmem");

  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  int id = 0;

  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    kfree_to_cpu(p, id);
    id = (id + 1) % NCPU;
  }
}

static void
kfree_to_cpu(void *pa, int id)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
}

void
kfree(void *pa)
{
  push_off();
  int id = cpuid();
  kfree_to_cpu(pa, id);
  pop_off();
}

void *
kalloc(void)
{
  struct run *r = 0;

  push_off();
  int id = cpuid();

  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);

  if(r == 0){
    for(int i = 1; i < NCPU; i++){
      int victim = (id + i) % NCPU;

      acquire(&kmem[victim].lock);
      r = kmem[victim].freelist;
      if(r)
        kmem[victim].freelist = 0;
      release(&kmem[victim].lock);

      if(r){
        struct run *rest = r->next;
        r->next = 0;

        if(rest){
          acquire(&kmem[id].lock);
          kmem[id].freelist = rest;
          release(&kmem[id].lock);
        }
        break;
      }
    }
  }

  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE);
  return (void*)r;
}
