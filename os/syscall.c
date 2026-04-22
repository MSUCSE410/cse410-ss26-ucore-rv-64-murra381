#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	debugf("sys_read fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDIN)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	return copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!\n");
	return fork();
}

uint64 sys_exec(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	copyinstr(p->pagetable, name, va, 200);
	debugf("sys_exec %s\n", name);
	return exec(name);
}

static TaskStatus proc_to_task_status(enum procstate state)
{
	switch (state) {
	case UNUSED:
	case USED:
		return UnInit;
	case SLEEPING:
	case RUNNABLE:
		return Ready;
	case RUNNING:
		return Running;
	case ZOMBIE:
		return Exited;
	default:
		return UnInit;
	}
}

uint64 sys_task_info(uint64 va)
{
	struct proc *p = curr_proc();
	TaskInfo ti;
	uint64 now = get_cycle();
	uint64 elapsed_cycles = 0;

	if (p->born_cycle != 0 && now >= p->born_cycle) {
		elapsed_cycles = now - p->born_cycle;
	}

	ti.status = proc_to_task_status(p->state);
	ti.time = (int)(elapsed_cycles * 1000 / CPU_FREQ);
	for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
		ti.syscall_times[i] = p->syscall_times[i];
	}
	return copyout(p->pagetable, va, (char *)&ti, sizeof(ti));
}

static int page_mapped(pagetable_t pagetable, uint64 va)
{
	return walkaddr(pagetable, va) != 0;
}

uint64 sys_mmap(uint64 start, uint64 len, uint64 port)
{
	struct proc *p = curr_proc();
	uint64 size, end, perm;

	if (!PGALIGNED(start)) {
		return -1;
	}
	if ((port & ~0x7ULL) != 0 || (port & 0x7ULL) == 0) {
		return -1;
	}
	if (len == 0) {
		return 0;
	}

	size = PGROUNDUP(len);
	end = start + size;
	if (end < start || end > TRAPFRAME) {
		return -1;
	}
	for (uint64 va = start; va < end; va += PAGE_SIZE) {
		if (page_mapped(p->pagetable, va)) {
			return -1;
		}
	}

	perm = PTE_U | (port << 1);
	for (uint64 va = start; va < end; va += PAGE_SIZE) {
		void *page = kalloc();
		if (page == 0) {
			return -1;
		}
		memset(page, 0, PAGE_SIZE);
		if (mappages(p->pagetable, va, PAGE_SIZE, (uint64)page, perm) != 0) {
			kfree(page);
			return -1;
		}
	}

	if (end / PAGE_SIZE > p->max_page) {
		p->max_page = end / PAGE_SIZE;
	}
	return 0;
}

uint64 sys_munmap(uint64 start, uint64 len)
{
	struct proc *p = curr_proc();
	uint64 size, end;

	if (!PGALIGNED(start)) {
		return -1;
	}
	if (len == 0) {
		return 0;
	}

	size = PGROUNDUP(len);
	end = start + size;
	if (end < start || end > TRAPFRAME) {
		return -1;
	}
	for (uint64 va = start; va < end; va += PAGE_SIZE) {
		if (!page_mapped(p->pagetable, va)) {
			return -1;
		}
	}

	uvmunmap(p->pagetable, start, size / PAGE_SIZE, 1);
	return 0;
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_spawn(uint64 va)
{
	struct proc *p = curr_proc();
	struct proc *np;
	char name[MAX_STR_LEN];
	int id;

	if (copyinstr(p->pagetable, name, va, MAX_STR_LEN) < 0) {
		return -1;
	}
	id = get_id_by_name(name);
	if (id < 0) {
		return -1;
	}
	np = allocproc();
	if (np == NULL) {
		return -1;
	}
	np->parent = p;
	if (loader(id, np) != 0) {
		freeproc(np);
		return -1;
	}
	add_task(np);
	return np->pid;
}

uint64 sys_set_priority(long long prio)
{
	if (prio < 2) {
		return -1;
	}
	curr_proc()->priority = prio;
	return prio;
}


extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7;
	uint64 ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	if (id >= 0 && id < MAX_SYSCALL_NUM) {
		curr_proc()->syscall_times[id]++;
	}
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2]);
		break;
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	case SYS_setpriority:
		ret = sys_set_priority(args[0]);
		break;
	case SYS_task_info:
		ret = sys_task_info(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
