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
int nexttid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
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
  p->master = p;
  p->tid = 0;
  p->front = 0;
  p->rear = 0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
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
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }

  acquire(&ptable.lock);

	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
		if(p->pid == 0)
			continue;
		if(curproc->pid == p->pid)
			p->sz = sz;
	}

	release(&ptable.lock);
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  // int i, pid;
  // struct proc *np;
  // struct proc *curproc = myproc();

  // // Allocate process.
  // if((np = allocproc()) == 0){
  //   return -1;
  // }

  // // Copy process state from proc.
  // if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
  //   kfree(np->kstack);
  //   np->kstack = 0;
  //   np->state = UNUSED;
  //   return -1;
  // }
  // np->sz = curproc->sz;
  // np->parent = curproc;
  // *np->tf = *curproc->tf;

  // // Clear %eax so that fork returns 0 in the child.
  // np->tf->eax = 0;

  // for(i = 0; i < NOFILE; i++)
  //   if(curproc->ofile[i])
  //     np->ofile[i] = filedup(curproc->ofile[i]);
  // np->cwd = idup(curproc->cwd);

  // safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  // pid = np->pid;

  // acquire(&ptable.lock);

  // np->state = RUNNABLE;

  // release(&ptable.lock);

  // return pid;
  int i, pid;
  struct proc *p;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
	
  acquire(&ptable.lock);
	
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
		if((p->pid == curproc->pid) && (p->tid != curproc->tid)) {
			deallocuvm(np->pgdir, p->addr + 2 * PGSIZE, p->addr);
			np->tcircle[np->rear] = p->addr;
			np->rear = (np->rear + 1) % NPROC;
		}
	}
	
  release(&ptable.lock);

  np->addr = curproc->addr;
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
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

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  // struct proc *curproc = myproc();
  // int fd;

  // curproc->retval = retval;

  // if(curproc == initproc)
  //   panic("init exiting");

  // // Close all open files.
  // for(fd = 0; fd < NOFILE; fd++){
  //   if(curproc->ofile[fd]){
  //     fileclose(curproc->ofile[fd]);
  //     curproc->ofile[fd] = 0;
  //   }
  // }

  // begin_op();
  // iput(curproc->cwd);
  // end_op();
  // curproc->cwd = 0;

  // acquire(&ptable.lock);

  // // Parent might be sleeping in wait().
  // wakeup1(curproc->parent);

  // // Jump into the scheduler, never to return.
  // curproc->state = ZOMBIE;
  // sched();
  // panic("zombie exit");
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");
	
  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  acquire(&ptable.lock);
	
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
		if(curproc->pid == p->pid && curproc != p) {
			kfree(p->kstack);
			p->kstack = 0;
			p->pid = 0;
			p->parent = 0;
			p->killed = 0;
      p->name[0] = 0;
      p->state = UNUSED;
		}
	}
	
  release(&ptable.lock);

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

int
qNumber(void)
{
	struct proc *p;
	int count = 0;
	int highq = 5;
	
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		if(p->pid > 2 && p->state == RUNNABLE && (p->qlev) < highq && p->needToBoost == 0){
			highq = p->qlev;
			count++;
		}
	}
	if(count == 0)
		return -1;
	return highq;
}

struct proc*
selectProcess(int qnum)
{
	struct proc *p;
	struct proc *highp = 0;
	int priority = 0;

	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		if(p->qlev == qnum && p->state == RUNNABLE && p->priority >= priority && p->needToBoost == 0){
				highp = p;
        priority = p->priority;
		}
	}
	return highp;
}

void
priorityBoost(void)
{
	struct proc *p;
	acquire(&ptable.lock);
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		if(p->pid > 0 && p->qlev != L0){
			if(p-> state == RUNNING || p->state == RUNNABLE || p->state == SLEEPING){
				p->qlev = L0;
				p->ticks = 0;
				p->state = RUNNABLE;
				p->needToBoost = 0;
			}
		}
	}
	release(&ptable.lock);
}

void
mlfqYield(int qLevel)
{
  myproc()->qlev = qLevel+1;
	acquire(&ptable.lock);
	myproc()->state = RUNNABLE;
	sched();
	release(&ptable.lock);
}

void
mlfqLowYield(void)
{
  myproc()->needToBoost = 1;
	acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);	
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct cpu *c;
  struct proc *p;
  c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);

#ifdef MULTILEVEL_SCHED
    int robin = 0;
    
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      if(p->pid % 2 == 0){
        robin = 1;
        break;
      }
    }

    if(robin == 1){ //round robin
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
          continue;
        
        if(p->pid % 2 == 1)
          continue;

        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;
        swtch(&(c->scheduler), p->context);
        switchkvm();
        c->proc = 0;
      }
    }
    else{ //FCFS
      struct proc * lowp = 0;

      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
          continue;

        if(lowp == 0)
          lowp = p;
        else if(p->pid < lowp->pid)
          lowp = p;
      }

      if(lowp != 0){
        p = lowp;
        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;
        swtch(&(c->scheduler), p->context);
        switchkvm();
        c->proc = 0;
      }
    }

#elif MLFQ_SCHED
    struct proc *select = 0;

		for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
			if(p->state != RUNNABLE)
				continue;

			if(p->pid <= 2)				
        select = p;
			else{
        int qnum = qNumber();
        
        if(qnum == -1)
          continue;
        
        select = selectProcess(qnum); 
          
        if(select->ticks > (4 * qnum + 2)){ 
          if(qnum != (MLFQ_K - 1)){
            select->ticks = 0;
            select->qlev = qnum+1;
          }
          else{
            select->needToBoost = 1;
            select->state = SLEEPING;
          }
          continue;
      }
    }

    c->proc = select;
    switchuvm(select);
    select->state = RUNNING;
    swtch(&(c->scheduler), select->context);
    switchkvm();
		c->proc = 0;
	  }

#else
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
#endif

    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
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

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

int
sys_yield(void)
{
  yield();
  return 0;
}

int
getlev(void)
{
	struct proc *p;
  int lev;
  p = myproc();
	lev = p->qlev;
	return lev;
}

int
sys_getlev(void)
{
	int lev;
	lev = getlev();
	return lev;	
}

// int
// setpriority(int pid, int priority)
// {
//   if(priority < 0 && priority > 10) 
//     return -2;

//   struct proc *child = 0;
//   struct proc *p;
//   acquire(&ptable.lock);

//   for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
//     if(p->pid == pid) {
//       child = p;
//       break;
//     }
//   }

//   if(child != 0) {
//     if(child->parent->pid == myproc()->pid) {
//       child->priority = priority;
//       return 0;
//     }
//   }

//   release(&ptable.lock);
//   return -1;
// }

int
setpriority(int pid, int priority)
{
  struct proc *curp;
  struct proc *p;
  int retVal = -1;

  curp = myproc();
  	 
  if(priority < 0 || priority > 10)
    return -2;
    
  acquire(&ptable.lock);
    
  for(p = ptable.proc; p< &ptable.proc[NPROC]; p++){
    if(pid > 0 && p->pid == pid && curp->pid == p->parent->pid){
      retVal = 0;
      break;
    }
  }
  
  if(retVal == 0)
    p->priority = priority;
  
  release(&ptable.lock);
  return retVal;
}

int
sys_setpriority(void)
{
    int pid, priority;
    if(argint(0, &pid) < 0 || argint(1, &priority) < 0)
	      return -1; 
    return setpriority(pid, priority);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
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
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int
thread_create(thread_t *thread, void *(*start_routine)(void *), void *arg)
{
	struct proc *np;
	struct proc *curproc = myproc();
  int i;
	uint sz, sp, ustack[2];
	pde_t *pgdir;


	if(curproc->master != curproc) {
		return -1;
	}

  // Allocate process.
	if((np = allocproc()) == 0) {
		return -1;
	}

	pgdir = curproc->pgdir;

	if(pgdir == 0) {
		np->state = UNUSED;
		return -1;
	}

	np->parent = curproc->parent;
	*np->tf = *curproc->tf;

	for(i = 0; i < NOFILE; i++)
		if(curproc->ofile[i])
			np->ofile[i] = filedup(curproc->ofile[i]);
	np->cwd = idup(curproc->cwd);

	safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  acquire(&ptable.lock);

  np->pid = curproc->pid;

  np->tid = nexttid++;

  np->master = curproc;

  np->tf->eax = 0;

  release(&ptable.lock);

  //

	if(curproc->front == curproc->rear) {
		sz = curproc->sz;	
	} else {
		sz = curproc->tcircle[curproc->front];
	}
	if((sz = allocuvm(pgdir, sz, sz + 2*PGSIZE)) == 0) {
		goto bad;
	}
	clearpteu(pgdir, (char*)(sz - 2*PGSIZE));
	sp = sz;

	ustack[0] = 0xffffffff;
	ustack[1] = (uint)arg;
	sp -= 8;
  np->addr = sz - 2*PGSIZE;
	if(copyout(pgdir, sp, ustack, 2 * 4) < 0)
		goto bad;
	
	if(curproc->front == curproc->rear) {
		curproc->sz = sz;
	} else {
    curproc->sz = curproc->sz;
		curproc->front = (curproc->front + 1) % (NPROC+1);
	}

  np->pgdir = pgdir;
	np->sz = curproc->sz;
	np->tf->eip = (uint)start_routine;
	np->tf->esp = sp;

	*thread = np->tid;

	acquire(&ptable.lock);

	np->state = RUNNABLE;

	release(&ptable.lock);

	return 0;

bad :
	np->state = UNUSED;
	return -1;
}

int
sys_thread_create(void)
{
	int thread, start_routine, arg;

	if(argint(0, &thread) < 0 || argint(1, &start_routine) < 0 || argint(2, &arg) < 0)
		return -1;

	return thread_create((thread_t*)thread, (void*)start_routine, (void*)arg);
}

void
thread_exit(void *retval)
{
  int fd;
  struct proc *curproc = myproc();

	if(curproc->tid == 0) {
		return;
	}

	curproc->retval = retval;

	for(fd = 0; fd < NOFILE; fd++) {
		if(curproc->ofile[fd]) {
			fileclose(curproc->ofile[fd]);
			curproc->ofile[fd] = 0;
		}
	}
	
	begin_op();
	iput(curproc->cwd);
	end_op();
	curproc->cwd = 0;

	acquire(&ptable.lock);

	wakeup1(curproc->master);

	curproc->state = ZOMBIE;
	
	sched();
	panic("zombie exit");

}

void
sys_thread_exit(void)
{
	int retval;

	if(argint(0, &retval) < 0)
		return;

	thread_exit((void*)retval);
	return;
}

int
thread_join(thread_t thread, void **retval)
{
	struct proc *p;
	int havekids, addr;
  struct proc *curproc = myproc();

	if(curproc->tid != 0) {
		return -1;
	}

	acquire(&ptable.lock);
	for(;;){
    // Scan through table looking for exited children.
		havekids = 0;
		for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
			if(p->master != curproc || p->tid != thread)
				continue;
			havekids = 1;
			if(p->state == ZOMBIE){
        // Found one.
				kfree(p->kstack);
				p->kstack = 0;
				p->pid = 0;
				p->parent = 0;
				p->killed = 0;
        p->name[0] = 0;
        p->state = UNUSED;
				p->master = 0;
				p->tid = 0;
        addr = p->addr;
				p->addr = 0;
        *retval = p->retval;
				deallocuvm(p->pgdir, addr + 2 * PGSIZE, addr);

				release(&ptable.lock);

				curproc->tcircle[curproc->rear] = addr;
				curproc->rear = (curproc->rear + 1) % (NPROC+1);				

				return 0;
			}
		}

		if(!havekids || curproc->killed) {
			cprintf("error\n");
			release(&ptable.lock);
			return -1;
		}

		sleep(curproc, &ptable.lock);
	}
}

int
sys_thread_join(void)
{
	int thread, retval;

	if(argint(0, &thread) < 0 || argint(1, &retval) < 0)
		return -1;

	return thread_join((thread_t)thread, (void**)retval);
}

void
texit(int pid, int tid) {
	struct proc *p;
		
	acquire(&ptable.lock);
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
		if(pid == 0)
			continue;
		if(p->pid == pid && p->tid != tid) {
			kfree(p->kstack);
      p->kstack = 0;
			p->pid = 0;
			p->parent = 0;
			p->killed = 0;
      p->name[0] = 0;
      p->state = UNUSED;
		}
	}
	release(&ptable.lock);
}