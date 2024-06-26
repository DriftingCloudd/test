#include "include/vma.h"
#include "include/elf.h"
#include "include/kalloc.h"
#include "include/memlayout.h"
#include "include/param.h"
#include "include/printf.h"
#include "include/proc.h"
#include "include/riscv.h"
#include "include/string.h"
#include "include/types.h"
#include "include/vm.h"

// 初始化进程的vma,在proc.c中调用
struct vma *vma_init(struct proc *p) {
  if (NULL == p) {
    printf("p is not existing\n");
    return NULL;
  }
  struct vma *vma = (struct vma *)kalloc();
  if (NULL == vma) {
    printf("vma kalloc failed\n");
    return NULL;
  }

  vma->type = NONE;
  vma->prev = vma->next = vma;
  p->vma = vma;

  if (NULL == alloc_mmap_vma(p, 0, USER_MMAP_START, 0, 0, 0, 0)) {
    // free_vma_list(p);
    kfree(vma);
    return NULL;
  }
  // if(alloc_vma_stack(p) != 0){
  //     kfree(vma);
  //     return NULL;
  // }

  return vma;
}

struct vma *alloc_vma(struct proc *p, enum segtype type, uint64 addr, uint64 sz,
                      int perm, int alloc, uint64 pa) {
  uint64 start = PGROUNDDOWN(addr);
  uint64 end = addr + sz;
  end = PGROUNDUP(end);

  struct vma *find_vma = p->vma->next;
  while (find_vma != p->vma) {
    if (end <= find_vma->addr)
      break;
    else if (start >= find_vma->end)
      find_vma = find_vma->next;
    else if (start >= find_vma->addr && end <= find_vma->end) {
      return find_vma;
    } else {
      printf("vma address overflow\n");
      return NULL;
    }
  }
  struct vma *vma = (struct vma *)kalloc();
  if (NULL == vma) {
    printf("vma kalloc failed\n");
    return NULL;
  }
  if (0 != sz) {
    if (alloc) {
      if (0 != uvmalloc1(p->pagetable, start, end, perm)) {
        printf("uvmalloc failed\n");
        kfree(vma);
        return NULL;
      }
    } else if (pa != 0) {
      if (0 != mappages(p->pagetable, start, sz, pa, perm)) {
        printf("mappages failed\n");
        kfree(vma);
        return NULL;
      }
    }
  }
  vma->addr = start;
  vma->sz = sz;
  vma->perm = perm;
  vma->end = end;
  vma->fd = -1;
  vma->f_off = 0;
  vma->type = type;
  vma->prev = find_vma->prev;
  vma->next = find_vma;
  find_vma->prev->next = vma;
  find_vma->prev = vma;

  return vma;
}

struct vma *find_mmap_vma(struct vma *head) {
  struct vma *vma = head->next;
  while (vma != head) {
    if (MMAP == vma->type)
      return vma;
    vma = vma->next;
  }
  return NULL;
}

struct vma *alloc_mmap_vma(struct proc *p, int flags, uint64 addr, uint64 sz,
                           int perm, int fd, uint64 f_off) {
  struct vma *vma = NULL;
  struct vma *find_vma = find_mmap_vma(p->vma);
  if (0 == addr && sz < find_vma->addr) {
    addr = PGROUNDDOWN(find_vma->addr - sz);
  }
  vma = alloc_vma(p, MMAP, addr, sz, perm, 1, NULL);
  if (NULL == vma) {
    printf("alloc_mmap_vma: alloc_vma failed\n");
    return NULL;
  }
  vma->fd = fd;
  vma->f_off = f_off;

  return vma;
}

struct vma *vma_copy(struct proc *np, struct vma *head) {
  struct vma *new_vma = (struct vma *)kalloc();
  if (NULL == new_vma) {
    debug_print("vma copy failed\n");
    goto failure;
  }
  new_vma->next = new_vma->prev = new_vma;
  new_vma->type = NONE;
  np->vma = new_vma;

  struct vma *pre_vma = head->next;
  struct vma *nvma = NULL;
  while (pre_vma != head) {
    nvma = (struct vma *)kalloc(); // TODO: fix this
    if (NULL == nvma) {
      goto failure;
    }
    memmove(nvma, pre_vma, sizeof(struct vma));
    nvma->next = nvma->prev = NULL;
    nvma->prev = new_vma->prev;
    nvma->next = new_vma;
    new_vma->prev->next = nvma;
    new_vma->prev = nvma;
    pre_vma = pre_vma->next;
  }

  return new_vma;

failure:
  np->vma = NULL;
  // TODO free_vma_list
  return NULL;
}

// 拷贝vma实际的内容
int vma_map(pagetable_t old, pagetable_t new, struct vma *vma) {
  uint64 start = vma->addr;
  pte_t *pte;
  uint64 pa;
  char *mem;
  long flags;

  while (start < vma->end) {
    if ((pte = walk(old, start, 0)) == NULL) {
      panic("uvmcopy: pte should exist");
    }
    if ((*pte & PTE_V) == 0) {
      panic("uvmcopy: page not present");
    }
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    mem = (char *)kalloc();

    if (NULL == mem)
      goto failure2;
    memmove(mem, (char *)pa, PGSIZE);

    if (mappages(new, start, PGSIZE, (uint64)mem, flags) != 0) {
      kfree(mem);
      goto failure2;
    }

    start += PGSIZE;
  }

  pa = walkaddr(new, vma->addr);
  return 0;

failure2:
  vmunmap(new, vma->addr, (start - vma->addr) / PGSIZE, 1);
  return -1;
}

int free_vma(struct proc *p, struct vma *del) {
  if (del == NULL) {
    debug_print("[free_vma] del is nil\n");
    return 0;
  }
  if (del->prev == NULL || del->next == NULL) {
    debug_print("[free_vma] del is illegal\n");
    return 0;
  }

  struct vma *prev = del->prev;
  struct vma *next = del->next;
  prev->next = next;
  next->prev = prev;
  del->next = del->prev = NULL;
  if (uvmdealloc1(p->pagetable, del->addr, del->end) != 0) {
    debug_print("[free_vma] uvmdealloc fail\n");
    return 0;
  }
  kfree(del);
  del = NULL;
  return 1;
}

int free_vma_list(struct proc *p) {
  struct vma *vma_head = p->vma;
  if (vma_head == NULL) {
    return 1;
  }
  struct vma *vma = vma_head->next;

  while (vma != vma_head) {
    uint64 a;
    pte_t *pte;
    for (a = vma->addr; a < vma->end; a += PGSIZE) {
      if ((pte = walk(p->pagetable, a, 0)) == 0)
        continue;
      if ((*pte & PTE_V) == 0)
        continue;
      if (PTE_FLAGS(*pte) == PTE_V)
        continue;
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
      *pte = 0;
    }
    vma = vma->next;
    kfree(vma->prev);
  }
  kfree(vma);
  p->vma = NULL;
  return 1;
}

// stack从maxuva - pgsize 开始向下分配，最多分配到USER_MMAP_START
uint64 alloc_vma_stack(struct proc *p) {
  // 起始分配100个page
  uint64 end = USER_STACK_TOP;
  uint64 start = end - INIT_STACK_SIZE;
  struct vma *find_vma = p->vma->next;
  // stack 放到链表的最后端
  while (find_vma != p->vma && find_vma->next != p->vma) {
    find_vma = find_vma->next;
  }

  struct vma *vma = (struct vma *)kalloc();
  if (NULL == vma) {
    printf("vma kalloc failed\n");
    return -1;
  }

  if (uvmalloc1(p->pagetable, start, end, PTE_R | PTE_W | PTE_U) != 0) {
    printf("user stack vma alloc failed\n");
    kfree(vma);
    return -1;
  }
  vma->type = STACK;
  vma->perm = PTE_R | PTE_W;
  vma->addr = start;
  vma->end = end;
  vma->sz = INIT_STACK_SIZE;
  vma->flags = 0;
  vma->fd = -1;
  vma->f_off = -1;
  vma->prev = find_vma;
  vma->next = find_vma->next;
  find_vma->next->prev = vma;
  find_vma->next = vma;
  return 0;
}

// 栈空间不足，需要分配新的栈空间，至多分配到USER_MMAP_START
uint64 handle_stack_page_fault(struct proc *p, uint64 va) {

  if (!(va >= USER_STACK_DOWN && va < USER_STACK_TOP)) {
    return -1;
  }
  struct vma *vma = p->vma->next;
  while (vma != p->vma) {
    if (vma->type == STACK) {
      break;
    }
    vma = vma->next;
  }
  if (vma->type != STACK) {
    printf("handle_stack_page_fault: vma type is not stack\n");
    return -1;
  }
  printf("handle stack page fault now start %p, now end :%p, va:%p\n",
         vma->addr, vma->end, va);
  uint64 start = vma->addr - INCREASE_STACK_SIZE_PER_FAULT;
  if (start > va) {
    start = PGROUNDDOWN(va);
  }
  uint64 end = vma->addr;

  if (uvmalloc1(p->pagetable, start, end, PTE_R | PTE_W | PTE_U) != 0) {
    printf("user stack vma alloc failed\n");
    return -1;
  }
  vma->addr = start;
  vma->sz = vma->sz + INCREASE_STACK_SIZE_PER_FAULT;

  return 0;
}

uint64 get_proc_sp(struct proc *p) {
  struct vma *vma = p->vma->next;
  while (vma != p->vma) {
    if (vma->type == STACK) {
      break;
    }
    vma = vma->next;
  }
  if (vma->type != STACK) {
    printf("get_proc_sp: vma type is not stack\n");
    return -1;
  }
  return vma->end;
}