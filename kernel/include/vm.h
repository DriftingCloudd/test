#ifndef __VM_H 
#define __VM_H 

#include "types.h"
#include "riscv.h"
#include "proc.h"
void            kvminit(void);
void            kvminithart(void);
uint64          kvmpa(uint64);
void            kvmmap(uint64, uint64, uint64, int);
int             mappages(pagetable_t, uint64, uint64, uint64, int);
pagetable_t     uvmcreate(void);
// void            uvminit(pagetable_t, uchar *, uint);
void            uvminit(pagetable_t, pagetable_t, uchar *, uint);
uint64          uvmalloc(pagetable_t, pagetable_t, uint64, uint64, int);
uint64          uvmdealloc(pagetable_t, pagetable_t, uint64, uint64);
// int             uvmcopy(pagetable_t, pagetable_t, uint64);
int             uvmcopy(pagetable_t, pagetable_t, pagetable_t, uint64);
void            uvmfree(pagetable_t, uint64);
// void            uvmunmap(pagetable_t, uint64, uint64, int);
void            vmunmap(pagetable_t, uint64, uint64, int);
void            uvmclear(pagetable_t, uint64);
uint64          walkaddr(pagetable_t, uint64);
int             copyout_zero(pagetable_t pagetable, uint64 dstva, uint64 len);
int             copyout(pagetable_t, uint64, char *, uint64);
int             copyin(pagetable_t, char *, uint64, uint64);
int             copyinstr(pagetable_t, char *, uint64, uint64);
pagetable_t     proc_kpagetable(struct proc* p);
void            kvmfreeusr(pagetable_t kpt);
void            kvmfree(pagetable_t kpt, int stack_free, struct proc* p);
uint64          kwalkaddr(pagetable_t pagetable, uint64 va);
int             copyout2(uint64 dstva, char *src, uint64 len);
int             copyin2(char *dst, uint64 srcva, uint64 len);
int             copyinstr2(char *dst, uint64 srcva, uint64 max);
void            vmprint(pagetable_t pagetable);
uint64          uvmalloc1(pagetable_t pagetable, uint64 start, uint64 end, int perm);
uint64          uvmdealloc1(pagetable_t pagetable, uint64 start, uint64 end);
uint64          experm(pagetable_t pagetable, uint64 va,uint64 perm);
pte_t *         walk(pagetable_t pagetable, uint64 va, int alloc);
#endif 
