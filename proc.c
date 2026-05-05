#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

int
cpuid()
{
  return mycpu()-cpus;
}

struct cpu*
mycpu(void)
{
  int apicid, i;

  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  for(i = 0; i < ncpu; ++i){
    if(cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

struct proc*
myproc(void)
{
  struct cpu *c;
  struct proc *p;

  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();

  return p;
}

static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->priority = 5;

  release(&ptable.lock);

  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }

  sp = p->kstack + KSTACKSIZE;

  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  initproc = p;

  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");

  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;

  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  acquire(&ptable.lock);
  p->state = RUNNABLE;
  release(&ptable.lock);
}

int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;

  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz+n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz+n)) == 0)
      return -1;
  }

  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  if((np = allocproc()) == 0)
    return -1;

  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->state = UNUSED;
    return -1;
  }

  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);

  np->cwd = idup(curproc->cwd);
  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);
  np->state = RUNNABLE;
  release(&ptable.lock);

  return pid;
}

void
scheduler(void)
{
  struct proc *p;
  struct proc *best;
  struct cpu *c = mycpu();

  c->proc = 0;

  for(;;){
    sti();

    acquire(&ptable.lock);

    best = 0;

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      if(best == 0 || p->priority < best->priority)
        best = p;
    }

    if(best != 0){
      p = best;
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      c->proc = 0;
    }

    release(&ptable.lock);
  }
}

void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");

  if(mycpu()->ncli != 1)
    panic("sched locks");

  if(p->state == RUNNING)
    panic("sched running");

  if(readeflags()&FL_IF)
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

void
yield(void)
{
  acquire(&ptable.lock);
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

void
forkret(void)
{
  static int first = 1;

  release(&ptable.lock);

  if(first){
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }
}

void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  wakeup1(curproc->parent);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);

  for(;;){
    havekids = 0;

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;

      havekids = 1;

      if(p->state == ZOMBIE){
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->priority = 5;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    sleep(curproc, &ptable.lock);
  }
}

void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  if(lk != &ptable.lock){
    acquire(&ptable.lock);
    release(lk);
  }

  p->chan = chan;
  p->state = SLEEPING;

  sched();

  p->chan = 0;

  if(lk != &ptable.lock){
    release(&ptable.lock);
    acquire(lk);
  }
}

static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;

      if(p->state == SLEEPING)
        p->state = RUNNABLE;

      release(&ptable.lock);
      return 0;
    }
  }

  release(&ptable.lock);
  return -1;
}

void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };

  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;

    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";

    cprintf("%d %s %s", p->pid, state, p->name);

    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }

    cprintf("\n");
  }
}

void
ps(void)
{
  struct proc *p;
  struct proc *best;
  char *state;
  int printed[NPROC];
  int i, count, idx;

  for(i = 0; i < NPROC; i++)
    printed[i] = 0;

  acquire(&ptable.lock);

  cprintf("PID   STATE       PRIORITY   NAME\n");

  for(count = 0; count < NPROC; count++){
    best = 0;

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      idx = p - ptable.proc;

      if(p->state == UNUSED || printed[idx])
        continue;

      if(best == 0 ||
         p->priority < best->priority ||
         (p->priority == best->priority && p->pid < best->pid)){
        best = p;
      }
    }

    if(best == 0)
      break;

    idx = best - ptable.proc;
    printed[idx] = 1;

    switch(best->state){
    case EMBRYO:
      state = "EMBRYO";
      break;
    case SLEEPING:
      state = "SLEEPING";
      break;
    case RUNNABLE:
      state = "RUNNABLE";
      break;
    case RUNNING:
      state = "RUNNING";
      break;
    case ZOMBIE:
      state = "ZOMBIE";
      break;
    default:
      state = "UNKNOWN";
      break;
    }

    cprintf("%d     %s     %d          %s\n",
            best->pid,
            state,
            best->priority,
            best->name);
  }

  release(&ptable.lock);
}

int
setpriority(int pid, int priority)
{
  struct proc *p;

  if(priority < 0)
    return -1;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->priority = priority;
      release(&ptable.lock);
      return 0;
    }
  }

  release(&ptable.lock);
  return -1;
}