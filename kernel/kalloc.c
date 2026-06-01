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

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

struct {
  struct spinlock lock;
  int cnt[(PHYSTOP - KERNBASE) / PGSIZE];
} kmem_ref;

static int
pa_index(void *pa)
{
  return ((uint64)pa - KERNBASE) / PGSIZE;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&kmem_ref.lock, "kmem_ref");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    acquire(&kmem_ref.lock);
    kmem_ref.cnt[pa_index(p)] = 1;
    release(&kmem_ref.lock);
    kfree(p);
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kmem_ref.lock);
  if(kmem_ref.cnt[pa_index(pa)] < 1)
    panic("kfree ref");
  kmem_ref.cnt[pa_index(pa)]--;
  if(kmem_ref.cnt[pa_index(pa)] > 0){
    release(&kmem_ref.lock);
    return;
  }
  release(&kmem_ref.lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r){
    memset((char*)r, 5, PGSIZE); // fill with junk
    acquire(&kmem_ref.lock);
    kmem_ref.cnt[pa_index(r)] = 1;
    release(&kmem_ref.lock);
  }
  return (void*)r;
}

void
kincref(void *pa)
{
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kincref");

  acquire(&kmem_ref.lock);
  if(kmem_ref.cnt[pa_index(pa)] < 1)
    panic("kincref ref");
  kmem_ref.cnt[pa_index(pa)]++;
  release(&kmem_ref.lock);
}

int
krefcnt(void *pa)
{
  int cnt;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("krefcnt");

  acquire(&kmem_ref.lock);
  cnt = kmem_ref.cnt[pa_index(pa)];
  release(&kmem_ref.lock);
  return cnt;
}
