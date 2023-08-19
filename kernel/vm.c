#include "include/param.h"
#include "include/types.h"
#include "include/memlayout.h"
#include "include/elf.h"
#include "include/riscv.h"
#include "include/vm.h"
#include "include/kalloc.h"
#include "include/proc.h"
#include "include/printf.h"
#include "include/string.h"
#include "include/uart8250.h"
/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.
extern char trampoline[]; // trampoline.S
/*
 * create a direct-map page table for the kernel.
 */
void
kvminit()
{
  kernel_pagetable = (pagetable_t) kalloc();
#ifdef DEBUG
  printf("kernel_pagetable: %p\n", kernel_pagetable);
#endif
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
#ifdef visionfive
  kvmmap(UART_V, UART, 0x10000, PTE_R | PTE_W);
#endif
  #ifdef QEMU
  // virtio mmio disk interface
  kvmmap(VIRTIO0_V, VIRTIO0, PGSIZE, PTE_R | PTE_W);
  #endif
  // CLINT
  kvmmap(CLINT_V, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC_V, PLIC, 0x4000, PTE_R | PTE_W | PTE_D | PTE_A);
  kvmmap(PLIC_V + 0x200000, PLIC + 0x200000, 0x4000, PTE_R | PTE_W);

  #ifdef visionfive
  kvmmap(SD_BASE_V, SD_BASE, 0x10000, PTE_R | PTE_W | PTE_A | PTE_D);
  #endif
  
  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X | PTE_A | PTE_D);
  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W | PTE_A | PTE_D);
  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X | PTE_A | PTE_D);

  #ifdef DEBUG
  printf("kvminit\n");
  #endif
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  
  sfence_vma();
  w_satp(MAKE_SATP(kernel_pagetable));
  //修改uart的地址映射
  uart8250_change_base_addr(UART_V);
  // sfence_vma();
  #ifdef DEBUG
  printf("kvminithart\n");
  #endif
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == NULL){
        // printf("walk: not valid\n");
        return NULL;
      }
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA){
    debug_print("walkaddr: va >= MAXVA\n");
    return NULL;
  }
    

  pte = walk(pagetable, va, 0);
  if(pte == 0){
    // printf("walkaddr: pte == 0\n");
    return NULL;
  }
  if((*pte & PTE_V) == 0){
    debug_print("va :%p walkaddr: *pte & PTE_V == 0\n", va);
    return NULL;
  }
  if((*pte & PTE_U) == 0){
    debug_print("walkaddr: *pte & PTE_U == 0\n");
    return NULL;
  }
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(uint64 va)
{
  return kwalkaddr(kernel_pagetable, va);
}

uint64
kwalkaddr(pagetable_t kpt, uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;
  
  pte = walk(kpt, va, 0);
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  //for visionfive 2
  perm |= PTE_A | PTE_D;
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  
  for(;;){
    if((pte = walk(pagetable, a, 1)) == NULL)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
vmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("vmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("vmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("vmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("vmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == NULL)
    return NULL;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, pagetable_t kpagetable, uchar *src, uint sz)
{
  char *mem;
  uint64 i;
  for(i = 0; i + PGSIZE < sz; i+=PGSIZE){
    mem = kalloc();
    // printf("[uvminit]kalloc: %p\n", mem);
    memset(mem, 0, PGSIZE);
    mappages(pagetable, i, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
    mappages(kpagetable, i, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X);
    memmove(mem, src + i, PGSIZE);
  }
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, i, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  mappages(kpagetable, i, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X);
  memmove(mem, src + i, sz % PGSIZE);
  debug_print("uvminit done sz:%d\n", sz);
  // for (int i = 0; i < sz; i ++) {
  //   printf("[uvminit]mem: %p, %x\n", mem + i, mem[i]);
  // }
}

uint64
uvmalloc1(pagetable_t pagetable, uint64 start, uint64 end, int perm)
{
  char *mem;
  uint64 a;
  if(start>=end)return -1;
  for(a = start; a < end; a += PGSIZE){
    mem = kalloc();
    if(mem == NULL){
      uvmdealloc1(pagetable, start, a);
      printf("uvmalloc kalloc failed\n");
      return -1;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, perm) != 0) {
      kfree(mem);
      uvmdealloc1(pagetable, start, a);
      printf("[uvmalloc]map user page failed\n");
      return -1;
    }
  }
  return 0;
}


uint64
uvmdealloc1(pagetable_t pagetable, uint64 start, uint64 end)
{
  
  if(start>=end)return -1;
  if(PGROUNDUP(start) <= PGROUNDUP(end)){
    int npages = (PGROUNDUP(end) - PGROUNDUP(start)) / PGSIZE;
    vmunmap(pagetable, PGROUNDUP(start), npages, 1);
  }

  return 0;
}


// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, pagetable_t kpagetable, uint64 oldsz, uint64 newsz, int perm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == NULL){
      uvmdealloc(pagetable, kpagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, perm | PTE_U) != 0) {
      kfree(mem);
      uvmdealloc(pagetable, kpagetable, a, oldsz);
      return 0;
    }
    if (mappages(kpagetable, a, PGSIZE, (uint64)mem, perm) != 0){
      int npages = (a - oldsz) / PGSIZE;
      vmunmap(pagetable, oldsz, npages + 1, 1);   // plus the page allocated above.
      vmunmap(kpagetable, oldsz, npages, 0);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, pagetable_t kpagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    vmunmap(kpagetable, PGROUNDUP(newsz), npages, 0);
    vmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      //panic("freewalk: leaf");
      continue;
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    vmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, pagetable_t knew, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i = 0, ki = 0;
  uint flags;
  char *mem;

  while (i < sz){
    if((pte = walk(old, i, 0)) == NULL)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == NULL)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
      kfree(mem);
      goto err;
    }
    i += PGSIZE;
    if(mappages(knew, ki, PGSIZE, (uint64)mem, flags & ~PTE_U) != 0){
      goto err;
    }
    ki += PGSIZE;
  }
  return 0;

 err:
  vmunmap(knew, 0, ki / PGSIZE, 0);
  vmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == NULL)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

//向指定的用户地址输出长度为len的0值
int
copyout_zero(pagetable_t pagetable, uint64 dstva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == NULL)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    
    memset((void *)(pa0 + (dstva - va0)), 0, n);
    
    len -= n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == NULL)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

int
copyout2(uint64 dstva, char *src, uint64 len)
{
  uint64 sz = myproc()->sz;
  if (dstva + len > sz || dstva >= sz) {
    return -1;
  }
  memmove((void *)dstva, src, len);
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == NULL){
      debug_print("copyin: pa0 is NULL\n");
      return -1;
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

int
copyin2(char *dst, uint64 srcva, uint64 len)
{
  uint64 sz = myproc()->sz;
  if (srcva + len > sz || srcva >= sz) {
    return -1;
  }
  memmove(dst, (void *)srcva, len);
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == NULL)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

int
copyinstr2(char *dst, uint64 srcva, uint64 max)
{
  int got_null = 0;
  uint64 sz = myproc()->sz;
  while(srcva < sz && max > 0){
    char *p = (char *)srcva;
    if(*p == '\0'){
      *dst = '\0';
      got_null = 1;
      break;
    } else {
      *dst = *p;
    }
    --max;
    srcva++;
    dst++;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// initialize kernel pagetable for each process.
pagetable_t
proc_kpagetable()
{
  pagetable_t kpt = (pagetable_t) kalloc();
  if (kpt == NULL)
    return NULL;
  memmove(kpt, kernel_pagetable, PGSIZE);

  // remap stack and trampoline, because they share the same page table of level 1 and 0
  char *pstack = kalloc();
  if(pstack == NULL)
    goto fail;
  if (mappages(kpt, VKSTACK, PGSIZE, (uint64)pstack, PTE_R | PTE_W) != 0)
    goto fail;
  
  return kpt;

fail:
  kvmfree(kpt, 1);
  return NULL;
}

// only free page table, not physical pages
void
kfreewalk(pagetable_t kpt)
{
  for (int i = 0; i < 512; i++) {
    pte_t pte = kpt[i];
    if ((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0) {
      kfreewalk((pagetable_t) PTE2PA(pte));
      kpt[i] = 0;
    } else if (pte & PTE_V) {
      break;
    }
  }
  kfree((void *) kpt);
}

void
kvmfreeusr(pagetable_t kpt)
{
  pte_t pte;
  for (int i = 0; i < PX(2, MAXUVA); i++) {
    pte = kpt[i];
    if ((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0) {
      kfreewalk((pagetable_t) PTE2PA(pte));
      kpt[i] = 0;
    }
  }
}

void
kvmfree(pagetable_t kpt, int stack_free)
{
  if (stack_free) {
    vmunmap(kpt, VKSTACK, 1, 1);
    pte_t pte = kpt[PX(2, VKSTACK)];
    if ((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0) {
      kfreewalk((pagetable_t) PTE2PA(pte));
    }
    pte = kpt[PX(2, VKSTACK - PGSIZE)];
    if ((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0) {
      kfreewalk((pagetable_t) PTE2PA(pte));
    }
  }
  kvmfreeusr(kpt);
  kfree(kpt);
}

void vmprint(pagetable_t pagetable)
{
  const int capacity = 512;
  printf("page table %p\n", pagetable);
  for (pte_t *pte = (pte_t *) pagetable; pte < pagetable + capacity; pte++) {
    if (*pte & PTE_V)
    {
      pagetable_t pt2 = (pagetable_t) PTE2PA(*pte); 
      printf("..%d: pte %p pa %p\n", pte - pagetable, *pte, pt2);

      for (pte_t *pte2 = (pte_t *) pt2; pte2 < pt2 + capacity; pte2++) {
        if (*pte2 & PTE_V)
        {
          pagetable_t pt3 = (pagetable_t) PTE2PA(*pte2);
          printf(".. ..%d: pte %p pa %p\n", pte2 - pt2, *pte2, pt3);

          for (pte_t *pte3 = (pte_t *) pt3; pte3 < pt3 + capacity; pte3++)
            if (*pte3 & PTE_V)
              printf(".. .. ..%d: pte %p pa %p\n", pte3 - pt3, *pte3, PTE2PA(*pte3));
        }
      }
    }
  }
  return;
}

uint64
experm(pagetable_t pagetable, uint64 va,uint64 perm)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return NULL;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return NULL;
  if((*pte & PTE_V) == 0)
    return NULL;
  if((*pte & PTE_U) == 0)
    return NULL;
  *pte |= perm;
  pa = PTE2PA(*pte);
  return pa;
}