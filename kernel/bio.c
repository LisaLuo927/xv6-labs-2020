// Buffer cache.
//
// The buffer cache is a hash table of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 31

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  struct {
    struct spinlock lock;
    struct buf head;
  } bucket[NBUCKET];
} bcache;

static uint
bhash(uint dev, uint blockno)
{
  return (dev ^ blockno) % NBUCKET;
}

static void
bdetach(struct buf *b)
{
  b->prev->next = b->next;
  b->next->prev = b->prev;
  b->prev = 0;
  b->next = 0;
}

static void
binsert(struct buf *head, struct buf *b)
{
  b->next = head->next;
  b->prev = head;
  head->next->prev = b;
  head->next = b;
}

void
binit(void)
{
  struct buf *b;
  int i;

  initlock(&bcache.lock, "bcache");

  for(i = 0; i < NBUCKET; i++){
    initlock(&bcache.bucket[i].lock, "bcache.bucket");
    bcache.bucket[i].head.prev = &bcache.bucket[i].head;
    bcache.bucket[i].head.next = &bcache.bucket[i].head;
  }

  i = 0;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++, i++){
    initsleeplock(&b->lock, "buffer");
    b->timestamp = 0;
    binsert(&bcache.bucket[i % NBUCKET].head, b);
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint h = bhash(dev, blockno);

  acquire(&bcache.bucket[h].lock);
  for(b = bcache.bucket[h].head.next; b != &bcache.bucket[h].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bucket[h].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bucket[h].lock);

  acquire(&bcache.lock);

  acquire(&bcache.bucket[h].lock);
  for(b = bcache.bucket[h].head.next; b != &bcache.bucket[h].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bucket[h].lock);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bucket[h].lock);

  struct buf *victim = 0;
  uint vh = 0;
  for(;;){
    uint oldest = ~0;
    victim = 0;

    for(uint i = 0; i < NBUCKET; i++){
      acquire(&bcache.bucket[i].lock);
      for(b = bcache.bucket[i].head.next; b != &bcache.bucket[i].head; b = b->next){
        if(b->refcnt == 0 && (victim == 0 || b->timestamp < oldest)){
          victim = b;
          vh = i;
          oldest = b->timestamp;
        }
      }
      release(&bcache.bucket[i].lock);
    }

    if(victim == 0)
      panic("bget: no buffers");

    acquire(&bcache.bucket[vh].lock);
    if(victim->refcnt == 0)
      break;
    release(&bcache.bucket[vh].lock);
  }

  if(vh != h){
    bdetach(victim);
    acquire(&bcache.bucket[h].lock);
    victim->dev = dev;
    victim->blockno = blockno;
    victim->valid = 0;
    victim->refcnt = 1;
    victim->timestamp = ticks;
    binsert(&bcache.bucket[h].head, victim);
    release(&bcache.bucket[h].lock);
    release(&bcache.bucket[vh].lock);
  } else {
    victim->dev = dev;
    victim->blockno = blockno;
    victim->valid = 0;
    victim->refcnt = 1;
    victim->timestamp = ticks;
    release(&bcache.bucket[vh].lock);
  }

  release(&bcache.lock);

  acquiresleep(&victim->lock);
  return victim;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint h = bhash(b->dev, b->blockno);
  acquire(&bcache.bucket[h].lock);
  b->refcnt--;
  if(b->refcnt == 0)
    b->timestamp = ticks;
  release(&bcache.bucket[h].lock);
}

void
bpin(struct buf *b)
{
  uint h = bhash(b->dev, b->blockno);
  acquire(&bcache.bucket[h].lock);
  b->refcnt++;
  release(&bcache.bucket[h].lock);
}

void
bunpin(struct buf *b)
{
  uint h = bhash(b->dev, b->blockno);
  acquire(&bcache.bucket[h].lock);
  b->refcnt--;
  if(b->refcnt == 0)
    b->timestamp = ticks;
  release(&bcache.bucket[h].lock);
}
