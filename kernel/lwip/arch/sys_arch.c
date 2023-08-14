#include "../include/arch/sys_arch.h"
#include "../include/lwip/err.h"
#include "../include/lwip/sys.h"

// sys protect

static struct {
  struct spinlock lk;
  struct cpu *cpu;
  u64_t depth;
} lwprot;

sys_prot_t sys_arch_protect(void) {
  sys_prot_t r;

  push_off();
  if (lwprot.cpu == mycpu())
    r = lwprot.depth++;
  else {
    acquire(&lwprot.lk);
    r = lwprot.depth++;
    lwprot.cpu = mycpu();
  }
  pop_off();

  return r;
}

void sys_arch_unprotect(sys_prot_t pval) {
  if (lwprot.cpu != mycpu() || lwprot.depth == 0)
    panic("sys_arch_unprotect");
  lwprot.depth--;
  if (lwprot.depth == 0) {
    lwprot.cpu = NULL;
    release(&lwprot.lk);
  }
}

// sys sem

err_t sys_sem_new(sys_sem_t *sem, u8_t count) {
  sem_init(sem, count);
  return ERR_OK;
}

void sys_sem_free(sys_sem_t *sem) { sem_destroy(sem); }

void sys_sem_signal(sys_sem_t *sem) { sem_post(sem); }

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout) {
  if (timeout == 0) {
    sem_wait(sem);
    return 0;
  } else {
    return sem_wait_with_milli_timeout(sem, timeout);
  }
  return 0;
}

int sys_sem_valid(sys_sem_t *sem) { return sem_is_valid(sem); }

void sys_sem_set_invalid(sys_sem_t *sem) { sem_set_invalid(sem); }

// sys mailbox
err_t sys_mbox_new(sys_mbox_t *mbox, int size) {
  if (size > MBOXSLOTS) {
    printf("sys_mbox_new: size %u\n", size);
    return ERR_MEM;
  }
  mbox->head = 0;
  mbox->tail = 0;
  mbox->invalid = 0;
  initlock(&mbox->s, "lwIP mbox");
  sem_init(&mbox->sem, 0);

  return ERR_OK;
}

void sys_mbox_set_invalid(sys_mbox_t *mbox) { mbox->invalid = 1; }

int sys_mbox_valid(sys_mbox_t *mbox) { return !mbox->invalid; }

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg) {
  err_t r = ERR_MEM;

  acquire(&mbox->s);
  if (mbox->head - mbox->tail < MBOXSLOTS) {
    mbox->msg[mbox->head % MBOXSLOTS] = msg;
    mbox->head++;
    sem_post(&mbox->sem);
    r = ERR_OK;
  }
  release(&mbox->s);

  return r;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg) {
  acquire(&mbox->s);
  while (mbox->head - mbox->tail == MBOXSLOTS)
    sem_wait(&mbox->sem);
  mbox->msg[mbox->head % MBOXSLOTS] = msg;
  mbox->head++;
  sem_post(&mbox->sem);
  release(&mbox->s);
}

void sys_mbox_free(sys_mbox_t *mbox) {
  if (mbox->head != mbox->tail)
    panic("sys_mbox_free");
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout) {
  u64_t start, to;
  u32_t r;

  acquire(&mbox->s);
  start = get_timeval().tv_usec;
  to = (u64_t)timeout * 1000 + start;
  while (mbox->head - mbox->tail == 0) {
    if (timeout != 0) {
      if (to < get_timeval().tv_usec) {
        r = SYS_ARCH_TIMEOUT;
        goto done;
      }
      sem_wait_with_milli_timeout(&mbox->sem, to);
    } else {
      sem_wait(&mbox->sem);
    }
  }
  r = get_timeval().tv_usec - start;
  if (msg)
    *msg = mbox->msg[mbox->tail % MBOXSLOTS];
  mbox->tail++;

done:
  release(&mbox->s);
  return r;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg) {
  u32_t r = SYS_MBOX_EMPTY;

  acquire(&mbox->s);
  if (mbox->head - mbox->tail != 0) {
    if (msg)
      *msg = mbox->msg[mbox->tail % MBOXSLOTS];
    mbox->tail++;
    r = 0;
  }
  release(&mbox->s);
  return r;
}

// init
void sys_init(void) { initlock(&lwprot.lk, "lwIP lwprot"); }
