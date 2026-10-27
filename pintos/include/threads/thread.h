#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

struct file; // filesys 헤더를 가져오지 않도록 하기 위함
#define FD_MAX 128 // 프로세스당 fd 최대 개수 (0, 1 예약 → 2~127)

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */
struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* 바꾸는 priority */
	int nice;
	int recent_cpu;

	int original_priority;					/*내가 원체 create에서 받은 우선순위가 아니라 양도 받은 놈 */

	int exit_status; //종료 상태

	int64_t wake_ticks;					/*wake tick*/

	struct lock *waiting_lock;					/*지금 기다리고 있는 락 위치*/
	/*
	lock
		lock 안에 문잠구고 들어가있는 thread
		semaphore
			1
			list 정렬이 된 상태
	*/
	struct list donors_list;
	/*
	list를 순회하면서 얘네들의 우선순위를 볼 수 있다
	이것들을 max 해주면 내 바뀐 우선순위를 가질 수 있다.
	*/
	struct list_elem donation_elem;
	/*
	내가 1짱이야 cpu 쓸거야 근데 lock에 접근해봤더니 이미 잠겼어
	lock을 가지고 있는 놈에게 이 
	*/

	struct list_elem all_list_elem;     /* all_list 선언 */


	/* Shared between thread.c and synch.c. */
	struct list_elem elem;              /* List element. */
	
	struct thread *parent_process;
	/* 부모 프로세스를 파악 할 수 있도록 포인터 생성*/

	struct list_elem child_elem;
	/* child_elem 리스트를 위해 선언*/

	struct list child_list;
	/* child 리스트 선언*/

	struct semaphore parent_sema;
	/* 부모 세마포어 선언*/
	
	struct semaphore child_sema;
	/* 자식 세마포어 선언*/
	
	struct semaphore fork_sema;
	/* 포크 세마포어 선언*/
	
	struct intr_frame *fork_intr;
	// 포크를 위해 if를 부모 스레드에 저장하고 자식이 꺼내씀

	struct file *running_file;
	// 현재 실행중인 파일

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */
	struct file **fd_table; // fd -> 열린 파일
	int next_fd; // 다음에 배정할 fd 번호
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	unsigned magic;                     /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs; // <-- 이 친구를 사용해서 껐다가 켜!

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_wake (int64_t ticks);
void thread_sleep (int64_t ticks);
void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_check_preemption(void);
void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

bool thread_priority_greater(const struct list_elem *a, const struct list_elem *b, void *aux);

void recursive_donation(struct thread *receiver, int priority);
void push_donation(struct thread *donor, struct thread *receiver);
void refresh_priority(void);

void thread_calc_recent_cpu_running(void);
void thread_calc_recent_cpu_all(void);
void thread_calc_priority_all(void);
void thread_calc_load_avg(void);

#endif /* threads/thread.h */
