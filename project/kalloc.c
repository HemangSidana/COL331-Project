// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.


#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  uint num_free_pages;  //store number of free pages
  struct run *freelist;
} kmem;


struct rmap{
  struct spinlock lock;
  pte_t* pl[NPROC];
  int free[NPROC];
  int ref;
};

struct rmap allmap[PHYSTOP/PGSIZE];


void init_rmap(void){
  uint sz= PHYSTOP/PGSIZE;
  for(uint i=0; i<sz; i++){
    initlock(&(allmap[i].lock), "rmap");
    (&allmap[i])->ref=0;
    for(uint j=0; j<NPROC; j++){
      (&allmap[i])->free[j]=1;
    }
  }
}

void share_add(uint pa, pte_t* pte_child){
  uint index= pa/PGSIZE;
  struct rmap* cur= &allmap[index];
  acquire(&(cur->lock));
  cur->ref++;
  uint i;
  for(i=0; i<NPROC; i++){
    if(cur->free[i]==1) break;
  }
  if(i==NPROC){
    panic("rmap filled");
  }
  cur->free[i]=0;
  cur->pl[i]= pte_child;
  release(&(cur->lock));
}

void share_remove(uint pa, pte_t* pte_proc) {
  uint index = pa/PGSIZE;
  struct rmap* cur = &allmap[index];
  acquire(&(cur->lock));
  uint i;
  for(i=0; i<NPROC; i++){
    if(cur->pl[i]==pte_proc && cur->free[i]==0) break;
  }
  if(i==NPROC) panic("Page table entry not found in rmap");
  cur->free[i]=1;
  cur->ref--;
  if(cur->ref==1){
    for(uint j=0; j<NPROC; j++){
      if(cur->free[j]==0){
        *(cur->pl[j]) |= PTE_W;
      }
    }
  }
  release(&(cur->lock));
}

void share_split(uint pa, pte_t* pte_proc){
  uint flag= PTE_FLAGS(*pte_proc);
  flag |= PTE_W;
  share_remove(pa,pte_proc);
  char* mem= kalloc();
  *pte_proc = PTE_ADDR(V2P(mem)) | flag;
  share_add(V2P(mem),pte_proc);
}

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
  init_rmap();
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
  {
    kfree(p);
    kmem.num_free_pages+=1;
  }
    
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(char *v)
{
  struct run *r;

  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(v, 1, PGSIZE);

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = (struct run*)v;
  r->next = kmem.freelist;
  kmem.num_free_pages+=1;
  kmem.freelist = r;
  if(kmem.use_lock)
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char*
kalloc(void)
{
  struct run *r;

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
  {
    kmem.freelist = r->next;
    kmem.num_free_pages-=1;
  }
    
  if(kmem.use_lock)
    release(&kmem.lock);
  if(r){
    return (char*)r;
  }
  // allocate_page();
  panic("Insufficient Memory");
  return kalloc();
}

uint 
num_of_FreePages(void)
{
  acquire(&kmem.lock);

  uint num_free_pages = kmem.num_free_pages;
  
  release(&kmem.lock);
  
  return num_free_pages;
}
