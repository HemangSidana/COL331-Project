#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "x86.h"
#include "proc.h"
#include "buf.h"

#define NSLOTS SWAPBLOCKS/8

struct swap_slot ss[NSLOTS];


struct rmap{
  struct spinlock lock;
  pte_t* pl[NPROC];
  int free[NPROC];
  int ref;
};

struct rmap allmap[PHYSTOP/PGSIZE];


// Initialize rmap 
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


// Add pte_t* in rmap corresponding to physical page with address pa
void share_add(uint pa, pte_t* pte_child){
  if(*pte_child & PTE_S) panic("page is in swap space");
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


// Remove pte_t* in rmap corresponding to physical page with address pa
int share_remove(uint pa, pte_t* pte_proc) {
  if(*pte_proc & PTE_S) panic("page is in swap blocks");
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
  return cur->ref;
}


// Make separate page for pte_t* trying to write shared page
void share_split(uint pa, pte_t* pte_proc){
  uint flag= PTE_FLAGS(*pte_proc);
  flag |= PTE_W;
  share_remove(pa,pte_proc);
  char* mem= kalloc();
  memmove(mem,(char*)P2V(pa),PGSIZE);
  *pte_proc = PTE_ADDR(V2P(mem)) | flag;
  share_add(V2P(mem),pte_proc);
}


// Add physical page with address pa in swap slot
void add_swap(uint pa, uint new_add, uint slot){
  uint z = pa/PGSIZE;
  struct rmap* cur = &allmap[z];
  acquire(&(cur->lock));
  uint index=0;
  for(uint i=0; i<NPROC; i++){
    if(cur->free[i]==0){
      ss[slot].page_perm= PTE_FLAGS(*(cur->pl[i]));
      cur->ref--;
      cur->free[i]=1;
      *(cur->pl[i])= new_add;
      ss[slot].pl[index]=cur->pl[i];
      ss[slot].present[index]=1;
      index++;
    }
  }
  ss[slot].num=index;
  if(cur->ref!=0) panic("Reference count should be 0");
  release(&(cur->lock));
}


// Initialize swap slots
void init_slot(){
  for(int i = 0; i<NSLOTS; i++){
    ss[i].is_free = 1;
    ss[i].num= 0;
    for(int j=0; j<NPROC; j++){
      ss[i].present[j]=0;
    }
  }
}


// Return page table entry pointing to victim page
pte_t* victim_page(){
  while(1){
    struct proc *p = victim_proc();
    int count = 0;
    for(int i = 0; i < p->sz; i+=PGSIZE){
      pte_t* pte = walkpgdir(p->pgdir, (void*)i, 0);
      if(*pte & PTE_P){
        if(!(*pte & PTE_A)){
          int found=1;
          int ind= PTE_ADDR(*pte)/PGSIZE;
          struct rmap* cur= &allmap[ind];
          acquire(&(cur->lock));
          for(uint j=0; j<NPROC; j++){
            if(cur->free[j]==0){
              if(*(cur->pl[j]) & PTE_A){
                found=0; break;
              }
            }
          }
          release(&(cur->lock));
          if(found){
            change_rss(PTE_ADDR(*pte),-1);
            return pte;
          } 
          else count++;
          // change_rss(PTE_ADDR(*pte),-1);
          // return pte;
        }
        else{
          count++;
        }
      }
    }
    unset_access(p->pgdir,count);
  }
  return 0;
}


// Unset 10% access bits of process with page directory p
void unset_access(pde_t* p, int count){
  int z = (count+9)/10;
  int i=0;
  while(z){
    pte_t* pte = walkpgdir(p, (void*)i, 0);
    if(*pte & PTE_P){
      if(*pte & PTE_A){
        // *pte &= ~PTE_A;
        uint ind= PTE_ADDR(*pte)/PGSIZE;
        struct rmap* cur= &allmap[ind];
        acquire(&(cur->lock));
        for(uint i=0; i<NPROC; i++){
          if(cur->free[i]==0){
            *(cur->pl[i]) &= ~PTE_A;
          }
        }
        release(&cur->lock);
        z--;
      }
    }
    i+=PGSIZE;
  }
}


// Move page into swap slot to free memory
void allocate_page(){
  pte_t* pte = victim_page();
  uint slot = NSLOTS;
  for(slot=0; slot<NSLOTS; slot++){
      if(ss[slot].is_free) break;
  }
  if(slot == NSLOTS){
      panic("Slots filled");
  }
  char* page = (char*)P2V(PTE_ADDR(*pte));   
  write_page(page,2+8*slot);
  ss[slot].is_free = 0;
  ss[slot].page_perm = PTE_FLAGS(*pte);
  uint new_add= (slot << 12) | PTE_S;
  add_swap(PTE_ADDR(*pte),new_add,slot);
  kfree(page);
  return;
}


// Remove pte from list of page table entries in swap slot 
void remove_swap(uint slot, pte_t* pte){
  if(ss[slot].is_free) panic("slot is free");
  uint i;
  for(i=0; i<NPROC; i++){
    if(ss[slot].present[i]==1 && ss[slot].pl[i]==pte){
      break;
    }
  }
  if(i==NPROC) panic("pte not found in slot");
  ss[slot].present[i]=0;
  ss[slot].num--;
  if(ss[slot].num==0) ss[slot].is_free=1;
}


// Clean pointers of process with page directory pde in swap space
void clean_swap(pde_t* pde){
  for(int i = 0; i < NPDENTRIES; i++){
    if(pde[i] & PTE_P){
      pte_t* pte= (pte_t*)P2V(PTE_ADDR(pde[i]));
      for(int j=0; j< NPTENTRIES; j++){
        if(pte[j] & PTE_S){
          uint slot= PTE_ADDR(pte[j]) >> 12;
          remove_swap(slot,&pte[j]);
        }
      }
    }
  }
}


// Transfer page in swap slot to memory with new physical page address pa
void recover_swap(uint pa, uint slot){
  if(ss[slot].is_free) panic("slot is empty");
  for(int i=0; i<NPROC; i++){
    if(ss[slot].present[i]==1){
      *(ss[slot].pl[i])= pa;
      ss[slot].num--;
      ss[slot].present[i]=0;
      share_add(pa,ss[slot].pl[i]);
    }
  }
  if(ss[slot].num!=0) panic("present list and num are inconsistend");
  ss[slot].is_free=1;
}


// Either page is in swap space or it does not have write permissions
void page_fault(){
  uint va = rcr2();
  struct proc *p = myproc();
  pte_t *pte = walkpgdir(p->pgdir, (void*)va, 0);
  if(*pte & PTE_S){
    uint slot = *pte >> 12;
    char* page = kalloc();
    read_page(page, 8*slot+2);
    uint perm = ss[slot].page_perm;
    uint new_add= V2P(page) | perm | PTE_A;
    recover_swap(new_add,slot);
    change_rss(V2P(page),1);
    // lcr3(V2P(p->pgdir));
  }
  else if(!(*pte & PTE_W)){
    uint pa= PTE_ADDR(*pte);
    share_split(pa,pte);
    lcr3(V2P(p->pgdir));
  }
  else{
    panic("page fault cannot be handled");
  }
}

void page_fault_swap(pte_t* pte){
  if(*pte & PTE_S){
    uint slot = *pte >> 12;
    char* page = kalloc();
    read_page(page, 8*slot+2);
    uint perm = ss[slot].page_perm;
    uint new_add= V2P(page) | perm | PTE_A;
    recover_swap(new_add,slot);
    // change_rss(V2P(page),1);
    // lcr3(V2P(p->pgdir));
  }
  else{
    panic("page fault swap cannot be handled");
  }
}

// Update rss value of process using physical page with address pa
void change_rss(uint pa, int d){
  for(int z=0; z<NPROC; z++){
    if(is_proc(z)){
      struct proc* p= get_proc(z);
      pde_t* pde= p->pgdir;
      for(int i = 0; i < NPDENTRIES; i++){
        if(pde[i] & PTE_P){
          pte_t* pte= (pte_t*)P2V(PTE_ADDR(pde[i]));
          for(int j=0; j< NPTENTRIES; j++){
            if(pte[j]==pa){
              p->rss+=d*PGSIZE;
            }
          }
        }
      }
    }
  }
}
