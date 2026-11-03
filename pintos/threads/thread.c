#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210


/*

== 매크로 함수 변수 이름 ==
fp => fixed point | 고정 소수점 값?
int => integer
to => to
zero => zero
nearest => round to nearest | 가장 가까운 정수로 반올림
add => addition
sub => subtraction
mul => multiplication
div => division | 나눗셈이에용
F => Fraction scale

*/
/* ========== 연산 메크로? ==========
   x, y : 고정소수점 값,  n : 정수 */
#define F (1 << 14)

/* 변환 */
static inline int
int_to_fp (int n) {
	return n * F;
}

static inline int
fp_to_int_zero (int x) {
	return x / F;
}


static inline int
fp_to_int_nearest (int x) {
	return x >= 0 ? (x + F / 2) / F : (x - F / 2) / F;
}

/* 덧셈 / 뺄셈 */
static inline int
fp_add (int x, int y) {
	return x + y;
}

static inline int
fp_sub (int x, int y) {
	return x - y;
}

static inline int
fp_add_int (int x, int n) {
	return x + n * F;
}

static inline int
fp_sub_int (int x, int n) {
	return x - n * F;
}

/* 곱셈 / 나눗셈 */
static inline int
fp_mul (int x, int y) {
	return (int) ((int64_t) x * y / F);
}

static inline int
fp_mul_int (int x, int n) {
	return x * n;
}

static inline int
fp_div (int x, int y) {
	return (int) ((int64_t) x * F / y);
}

static inline int
fp_div_int (int x, int n) {
	return x / n;
}
/* ================================================= */


/* 파일 내부 전역 선언을 해줘요 */
static int load_avg;

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Thread sleep list*/
static struct list sleep_list;

/* 살아있는 전체 스레드 목록 */
static struct list all_list;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

static bool wakeup_tick_less (const struct list_elem *a, const struct list_elem *b, void *aux);
static bool donor_priority_less (const struct list_elem *a, const struct list_elem *b, void *aux);
static int load_avg;
/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);
	


	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);
	list_init (&sleep_list);
	list_init (&all_list); // <-- 살아있는 전체 스레드 목록 초기화

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();

	/* we need to use this init values when mlfqs used*/
	load_avg = 0;
	initial_thread->nice = 0;
	initial_thread->recent_cpu = 0;
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}


/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) { // <-- MLFQS 계산이 들어가야 함
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	if(thread_mlfqs){
	}
	
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();
	
	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	t->parent_process = thread_current(); // 현재 스레드(부모)를 넣어준다.
	list_push_back(&thread_current()->child_list,&t->child_elem); // 부모안에 child_list에 t->child_elem을 넣어준다.

	/* Add to run queue. */
	thread_unblock (t);
	thread_check_preemption();
	return tid;
}


/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
// sleep_list 정렬용 비교 함수. A의 깨어날 시각이 B보다 빠르면 true를 반환한다.
static bool
wakeup_tick_less (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	const struct thread *ta = list_entry (a, struct thread, elem);
	const struct thread *tb = list_entry (b, struct thread, elem);

	return ta->wake_ticks < tb->wake_ticks;
}

bool
thread_priority_greater(const struct list_elem *a, const struct list_elem *b, void *aux){
	int64_t at = list_entry(a, struct thread, elem)->priority;
	int64_t bt = list_entry(b, struct thread, elem)->priority;
	return at > bt;
}

void
thread_sleep(int64_t ticks){
	enum intr_level old_level = intr_disable();
	struct thread *cur = thread_current();

	cur->wake_ticks = ticks;

	list_insert_ordered(&sleep_list, &cur->elem, wakeup_tick_less, NULL);

	thread_block();
	intr_set_level(old_level);
}

void
thread_wake(int64_t ticks){ // <- sleep_list에서 깨울 스레드를 unblock
	while (!list_empty(&sleep_list)) //넘은거 처리
	{
		struct list_elem *e = list_front(&sleep_list);
		struct thread *t = list_entry(e, struct thread, elem);
		if(t->wake_ticks > ticks) break;
		list_remove(list_front(&sleep_list));
		thread_unblock(t);
	}
}

void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	list_insert_ordered (&ready_list, &t->elem, thread_priority_greater, NULL);
	t->status = THREAD_READY;

	intr_set_level (old_level);

}

void
thread_check_preemption(void){
	enum intr_level old_level = intr_disable();
	//ready 빈 상황
	if(list_empty(&ready_list)){
		intr_set_level(old_level);
		return;
	} 
	//정렬된 ready list 이므로 priority 최대값만 cur thread 랑 비교
	struct thread *top = list_entry(list_front(&ready_list), struct thread, elem);
	bool preempt = top -> priority > thread_current()->priority;

	intr_set_level(old_level);
	//우선순위가 cur thread가 높은 경우 pass
	if(!preempt) return;
	//우선순위가 cur thread가 낮은 경우 처리
	if (intr_context()) {
		intr_yield_on_return();
	} else {
	thread_yield();
	}
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	list_remove (&thread_current ()->all_list_elem); // <-- exit 하면 리스트 삭제
	do_schedule (THREAD_DYING); // 다음 스레드가 메모리 해제를 해줌
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_insert_ordered (&ready_list, &curr->elem, thread_priority_greater, NULL);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) {
	
	struct thread *curr = thread_current();
	curr->original_priority = new_priority; // 원래 값만 바꾼다
	if(thread_mlfqs){
		curr->priority = new_priority;
	}else{
		refresh_priority();
		thread_check_preemption();
	}

}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

// ========== MLFQS ==========

/*
priority -> 4틱 마다 모든 스레드를 죠쟈ㅕ!
priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)

recent_cpu -> 1초 마다 모든 스레드를 죠져고, 매 틱마다 실행 중인 스레드를 죠져
recent_cpu = (2 * load_avg) / (2 * load_avg + 1) * recent_cpu + nice

load_avg (1초 마다, 시스템 전역 변수 1개를 조져)
load_avg = (59/60) * load_avg + (1/60) * ready_threads
*/

/* load_avg = (59/60) * load_avg + (1/60) * ready_threads */
static inline int
mlfqs_calc_load_avg (int old_load_avg, int ready_threads) {
	return fp_add (fp_mul (fp_div (int_to_fp (59), int_to_fp (60)), old_load_avg),
	               fp_mul_int (fp_div (int_to_fp (1), int_to_fp (60)), ready_threads));
}

/* recent_cpu = (2*load_avg)/(2*load_avg + 1) * recent_cpu + nice */
static inline int
mlfqs_calc_recent_cpu (int recent_cpu, int cur_load_avg, int nice) {
	int coef = fp_div (fp_mul_int (cur_load_avg, 2),
	                   fp_add_int (fp_mul_int (cur_load_avg, 2), 1));
	return fp_add_int (fp_mul (coef, recent_cpu), nice);
}

/* recent_cpu + 1 */
static inline int
mlfqs_calc_increment_recent_cpu (int recent_cpu) {
	return fp_add_int (recent_cpu, 1);
}

/* priority = PRI_MAX - (recent_cpu / 4) - (nice * 2), PRI_MIN~PRI_MAX로 clamp */
static inline int
mlfqs_calc_priority (int recent_cpu, int nice) {
	int priority = fp_to_int_zero (
		fp_sub_int (fp_sub (int_to_fp (PRI_MAX), fp_div_int (recent_cpu, 4)),
		            nice * 2));

	if (priority > PRI_MAX)
		priority = PRI_MAX;
	if (priority < PRI_MIN)
		priority = PRI_MIN;
	return priority;
}


/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice) {
	enum intr_level old_level = intr_disable (); // <-- 인터럽트 끄고
	struct thread *cur = thread_current (); // <-- 현재 스레드 가져오기

	cur->nice = nice; // 나이스를 저장해 줍니다.
	cur->priority = mlfqs_calc_priority (cur->recent_cpu, cur->nice);
	// ^^^priority를 다시 계산해 주도록 합니다.^^^

	intr_set_level (old_level); // 인터럽트 복구
	thread_check_preemption (); // 더 높은 ready 스레드가 있으면 선점시키기
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	// return 0;
	return thread_current()->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return fp_to_int_nearest (fp_mul_int (load_avg, 100));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	struct thread *curr = thread_current();
	return fp_to_int_nearest(fp_mul_int(curr->recent_cpu, 100));
}
// ============================

void thread_calc_recent_cpu_running(void){
	struct thread *cur = thread_current();
	if(cur != idle_thread){
		cur->recent_cpu = fp_add_int(cur->recent_cpu,1);
	}
}

void thread_calc_load_avg(void){
	int ready_threads;

	ready_threads = list_size(&ready_list);
	
	if(thread_current() != idle_thread){
		ready_threads += 1;
	}

	load_avg = mlfqs_calc_load_avg (load_avg, ready_threads);
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	if(thread_mlfqs){
		// nice, recent_cpu를 초기화하고 그 값으로 prioriy 계산하는 것
	} else{
		t->priority = priority;
		t->original_priority = priority;
	}
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	
	t->exit_status = -1;
	t->waiting_lock = NULL; // 처음에 기다리는 lock이 없어서
	t->parent_process = NULL; // 부모 프로세스 NULL로 초기화
	
	sema_init(&t->parent_sema,0); // 부모의 세마포어 초기화
	sema_init(&t->child_sema,0);  // 자식의 세마포어 초기화 
	sema_init (&t->fork_sema,0); // 포크의 세마포어 초기화
	list_init (&t->donors_list);  // 나에게 기부한 스레드 목록 
	list_init (&t->child_list);   // 자식 스레드 목록!


	enum intr_level old_level = intr_disable();
	list_push_back (&all_list, &t->all_list_elem);
	intr_set_level (old_level);

	t->magic = THREAD_MAGIC; 
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}

/* 우선순위 전파 */
void 
recursive_donation(struct thread *receiver, int priority){
	if(receiver == NULL) return;
	if(receiver->priority < priority)
		//receiver의 priority가 크면 갱신
		receiver->priority = priority;
	if(receiver->waiting_lock != NULL){
		//receiver가 갱신되는 priority를 전파
		recursive_donation(receiver->waiting_lock->holder, receiver->priority);
	}
}
/* 기부 관계 */
void
push_donation(struct thread *donor, struct thread *receiver){
	if(donor->waiting_lock != NULL && donor->waiting_lock->holder == receiver){
		list_push_back(&receiver->donors_list, &donor->donation_elem);
	}
}

// 기부가 바뀐 뒤 유효 우선순위 재계산
void
refresh_priority(void){
	struct thread *curr = thread_current();

	// 원래 우선순위로 초기화
	curr->priority = curr->original_priority;

	// 남은 donor 중 최댓값과 비교
	if(!list_empty(&curr->donors_list)){
		struct list_elem *max_e = list_max(&curr->donors_list, donor_priority_less, NULL);
		struct thread *top = list_entry(max_e, struct thread, donation_elem);
		if(top->priority > curr->priority){
			curr->priority = top->priority;
		}
	}
}

static bool
donor_priority_less (const struct list_elem *a, const struct list_elem *b, void *aux){
	const struct thread *ta = list_entry(a, struct thread, donation_elem);
	const struct thread *tb = list_entry(b, struct thread, donation_elem);
	return ta->priority < tb->priority;
}

void
thread_calc_recent_cpu_all(void){
	for(struct list_elem *e = list_begin(&all_list);e != list_end(&all_list) ;e = list_next(e)){
		struct thread* t = list_entry(e, struct thread, all_list_elem);
		if(t == idle_thread) continue;
		t->recent_cpu = mlfqs_calc_recent_cpu(t->recent_cpu, load_avg, t->nice);
	}
}

void
thread_calc_priority_all(void){
	for(struct list_elem *e = list_begin(&all_list);e != list_end(&all_list) ;e = list_next(e)){
		struct thread* t = list_entry(e, struct thread, all_list_elem);
		if(t == idle_thread) continue;
		t->priority = mlfqs_calc_priority(t->recent_cpu, t->nice);
	}
	list_sort(&ready_list, thread_priority_greater, NULL);
	thread_check_preemption();
}