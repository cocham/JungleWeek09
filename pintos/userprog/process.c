#include "userprog/process.h"

#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);
static void argument_stack (char **argv, int argc, struct intr_frame *if_);
char* arg_pashing(char *s, char **save_ptr);
struct thread *find_child(struct thread *parent, tid_t child_tid);

/* General process initializer for initd and other process. */
static bool // <- 메모리 부족을 감지하기 위해서 반환값을 bool로 변환
process_init (void) {
	struct thread *current = thread_current ();

	// fd를 초기화
	// palloc_get_page -> 커널 메모리를 페이지 단위로 받음
	current->fd_table = palloc_get_page(PAL_ZERO); // 테이블 생성 + 모든 칸 NULL
	if (current->fd_table == NULL) {
		return false;
	}
	
	current->next_fd = 2; // 0, 1은 다른 애들이 차지, 2부터 배정 시작 (3 부터도 가능)
	return true;
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;
	char *file_name_ptr;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (arg_pashing(file_name, &file_name_ptr), PRI_DEFAULT, initd, fn_copy);
	

	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

// open하고 NULL값 찾아서 값 넣고 index 반환
int
fd_alloc(struct file *file) {
	struct thread *curr = thread_current(); // <- 현재의 쓰레드
	struct file **fdt = curr->fd_table;

	for(int fd = curr->next_fd; fd < FD_MAX; fd++) {
		if(fdt[fd] == NULL) {
			fdt[fd] = file;
			curr->next_fd = fd + 1;
			return fd;
		}
	}
	return -1;
}

// 반환된 index값으로 부터 포인터를 찾고 그걸 반환
struct file *
fd_get(int fd) {
	if(fd<2 || fd>=FD_MAX) {
		return NULL;
	}
	return thread_current()->fd_table[fd];
}

// index 값을 NBULL값으로 변환
void
fd_close (int fd) {
	struct thread *curr = thread_current();
	struct file *file = fd_get(fd);

	if (file == NULL) {
		return ;
	}

	curr->fd_table[fd] = NULL;
	file_close (file);

	if (fd < curr->next_fd) {
		curr->next_fd = fd;
	}
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	if (!process_init ()) {
		PANIC("initd의 초기화를 실패 해버렸습니다.\n"); // 메모리가 부족해서 할당에 실패하면 fd_table이 NULL인 채로 프로세스가 실행되는 문제가 발생!!
	}

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	struct thread *cur = thread_current();
	cur->fork_intr = if_; // 성공 실패 여부를 둘다 판단함

	/* Clone current thread to new thread.*/
	tid_t fork_t = thread_create(name, PRI_DEFAULT, __do_fork, cur);
	if(fork_t == TID_ERROR){ // thread_creat 판단
		return TID_ERROR;
	}

	sema_down(&cur->fork_sema);
	if (cur->fork_intr == NULL){ // fork 성공 여부 판단
		return TID_ERROR;
	}

	return fork_t;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;
	/* 1. TODO: If the parent_page is kernel page, then return immediately. */

	if (is_kernel_vaddr(va)){
		return true;
	}
	
	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = (uint8_t *) palloc_get_page(PAL_USER);
	if (newpage == NULL){
		return false;
	}
	
	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */

	uint8_t *new_p = newpage;
	uint8_t *parent_p = parent_page;
	for (unsigned i = 0; i < PGSIZE; i++){
		new_p[i] = parent_p[i];
	}

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	writable = is_writable(pte);
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		palloc_free_page(newpage);
		return false;
	}

	return true;
	
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if = parent->fork_intr;
	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
	if_.R.rax = 0; // 자식의 fork 반환값은 0

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	if (!process_init ()) {
		goto error; // 메모리가 부족해서 할당에 실패하면 fd_table이 NULL인 채로 프로세스가 실행되는 문제가 발생!!
	}

	// 부모의 테이블을 자식으로 값을 복사해서 옮김
	// 주소를 그대로 옮기면 안됩니다;;
	for (int i = 2; i < FD_MAX; i++){
		struct file *f = parent->fd_table[i];
		if(f==NULL){
			continue;
		}

		lock_acquire(&file_lock);
		struct file *file_val = file_duplicate(f);
		lock_release(&file_lock);

		if(file_val == NULL){
			goto error;
		}

		current->fd_table[i] = file_val;
	}

	// 부모의 진행파일도 옮김
	if (parent->running_file != NULL){
		// 주소를 그대로 옮기면 안됩니다;;
		lock_acquire(&file_lock);
		struct file *running_f = file_duplicate(parent->running_file);
		lock_release(&file_lock);

		if (running_f == NULL){
			goto error;
		}

		current->running_file = running_f;
	}

	/* Finally, switch to the newly created process. */
	if (succ){
		sema_up(&parent->fork_sema);
		do_iret (&if_);
	}
error:
	list_remove(&current->child_elem);  // 부모의 child_list에서 빠짐 → wait 대상 아님
	sema_up(&current->child_sema);      // 기다려 줄 부모가 없으니 미리 통과권 발급
	parent->fork_intr = NULL;
	sema_up(&parent->fork_sema);
	thread_exit();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();

	/* And then load the binary */
	success = load (file_name, &_if);

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */

/* 자식 찾기 프로젝트 */
struct thread
*find_child(struct thread *parent, tid_t child_tid){
	struct list_elem *e;
	struct thread *child;

	if (!list_empty(&parent->child_list)){   // 리스트가 비어있는지 확인.
		
		e = list_front(&parent->child_list); // 맨 앞의 리스트 한개를 가져온다. 
		
		// 리스트 마지막까지 검사해서 인자로 받은 tid와 일치하는 자식 프로세스를 찾는다.
		while(e != list_end(&parent->child_list)){ 
			child = list_entry(e,struct thread,child_elem);

			if(child->tid == child_tid){
				// thread*로 리턴
				return child; 		
			}

			e = list_next(e);
		}
	}

	return NULL; // 찾지 못했거나 리스트가 비어있으면 NULL반환
}

int
process_wait (tid_t child_tid UNUSED) {
	struct thread *parent = thread_current();			 // 부모 프로세스라고 가정하고 진행
	struct thread *child = find_child(parent,child_tid); // 자식 프로세스를 찾고 child에 할당
	int child_status; 									 // 자식 프로세스의 종료숫자를 저장할 int 선언
	
	if(child == NULL){	// 리스트에서 찾지 못하면 -1을 리턴
		return -1;
	}

	sema_down(&child->parent_sema);    // 자식 구조체의 세마를 이용해서 parent_sema 즉 현재 부모 프로세스를 잠재운다.

	child_status = child->exit_status; // 자식의 exit_status를 저장한다.
	list_remove(&child->child_elem);   // 부모의 child 리스트에서 remove으로 삭제한다.
	
	sema_up(&child->child_sema);       // 자식을 깨워서 진행시킨다.
	
	
	return child_status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current (); // 자식 프로세스라고 가정하고 진행
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */
	
	// 유저 프로세스(pml4 있음)만 종료 메시지 출력, 커널 스레드는 출력 안 함
	if(curr->pml4 != NULL){
		printf("%s: exit(%d)\n", curr->name, curr->exit_status);
	}
	
	// 현재 스레드의 진행 파일을 close 해줘야 함
	if (curr->running_file != NULL){
		lock_acquire(&file_lock);
		file_close(curr->running_file);
		lock_release(&file_lock);
		curr->running_file = NULL;
	}

	if (curr->fd_table != NULL){
		lock_acquire(&file_lock);
		for (int i = 2; i < FD_MAX; i++){
			if (curr->fd_table[i] != NULL){
				file_close(curr->fd_table[i]);
				curr->fd_table[i] = NULL;
			}
		}
		lock_release(&file_lock);
		palloc_free_page(curr->fd_table);
		curr->fd_table = NULL;
	}

	sema_up(&curr->parent_sema);  // 부모 프로세스를 깨운다.
	
	sema_down(&curr->child_sema); // 자식 즉 본인을 잠재운다.
	
	/* child_list가 안비었을 경우 list_front()를 통해 맨앞을 받은 뒤 마지막까지 반복해서 child_list를 전부 깨운다.
	   즉 부모 프로세스가 죽는다는 가정하에 실행 */
	if(!list_empty(&curr->child_list)){	 
		struct list_elem *childs = list_front(&curr->child_list);
		struct list_elem *e;
		while(childs != list_end(&curr->child_list)){
			e = list_next(childs); // 세마 업을 진행하면 실행 후 사라질 수 도 있기 때문에 미리 next값을 저장하고 진행
			sema_up(&list_entry(childs,struct thread, child_elem)->child_sema); 
			childs = e;
		}
	}
	process_cleanup ();
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	char *token, *save_ptr;
	char *token_list[64];
	int count = 0;
	if(token = arg_pashing(file_name,&save_ptr)){
		token_list[count] = token;
	}
	while(token != NULL){
		token = arg_pashing(NULL,&save_ptr);
		count++;
		token_list[count] = token;
	}

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	file = filesys_open (token_list[0]);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	
	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */
	
	argument_stack(&token_list, count, if_);

	success = true;

	// 실행 중인 파일을 붙잡기 - 프로세스가 끝날 때까지 이 파일에 쓰기 금지
	lock_acquire(&file_lock);
	// 기존 파일을 지워줘야 함
	if (t->running_file != NULL){
		file_close(t->running_file);
	}
	file_deny_write(file); // inode의 쓰기 금지 횟수 +1 (0이 될 때까지 write는 0바이트)
	t->running_file = file;
	lock_release(&file_lock);

done:
	/* We arrive here whether the load is successful or not. */

	// 성공: 실행 파일은 running_file로 보관 중이니 닫지 않음
	// 실패: 이 프로세스는 실행되지 않으니 열었던 파일을 닫음
	if (!success) {
		lock_acquire (&file_lock);
		file_close (file);
		lock_release (&file_lock);
	}

	return success;
}

// file_name을 공백 기준으로 분할해 token을 나눠주는 함수
char* 
arg_pashing(char *s, char **save_ptr){
	
	char *token; /*저장할 토큰 주소 설정*/

	/* char *s가 분할할 문자열 주소가 아닌 NULL을 받을 경우 예전에 저장한 주소로 이동*/
	if(s == NULL){ 
		s = *save_ptr;
	}

	/* 빈칸이 앞에 있을 경우 +를 증가시켜 위치를 이동 */
	while(*s == ' '&& *s != '\0'){
		s++;
	}

	/* 만일 앞에 빈칸을 없애는 중에 문자열의 끝에 도달할 경우 NULL을 반환 */
	if(*s == '\0'){
		*save_ptr = s;
		return NULL;
	}

	/* 토큰 시작 주소를 s로 설정 이후 빈칸을 만날때 까지 ++로 앞으로 이동 */
	token = s;
	while(*s != ' ' && *s != '\0'){
		s++;
	}
	
	/* 빈칸을 '\0'으로 변경해준뒤 s + 1로 save_ptr를 설정 */
	if(*s!='\0'){
		*s = '\0';
		*save_ptr = s + 1;
	}else{
		*save_ptr = s;
	}

	return token;
}

// argument_stack() arg를 받아와서 stack에 넣어주는 함수
static void
argument_stack(char **argv/*파싱된 문자열 받아서 온다.*/, int argc/*단어의 계수*/, struct intr_frame *if_/*스냅샷을 찍어서 보관*/){
	uint8_t *rsp = (uint8_t *) if_->rsp; // -> 이미 if_->rsp는 USER_STACK으로 초기화 되어있음 따라서 여기가 User_stack의 최상단 주소가 됨.

	/* 인자 문자열을 마지막 것부터 복사하고, argv[i]를 사용자 스택 위의 주소로 바꿔 둔다. */
	for (int i = argc - 1; i >= 0; i--) {
		size_t len = strlen (argv[i]) + 1;
		rsp -= len;
		memcpy (rsp, argv[i], len); // <- 커널 커널 스택의 메모리의 스택을 유저 스택으로 복사함
		argv[i] = (char *) rsp;  // <- 복사한 메모리의 위치를 기록
	}

	/* 8바이트 경계로 정렬한다. */
	while ((uintptr_t) rsp % 8 != 0) {
		rsp--;
		*rsp = 0;
	}

	/* 스택의 끝을 표기 */
	rsp -= sizeof (char *); // rsp -= 8;
	*(char **) rsp = NULL;

	/* 위에서 유저스택에 복사한 메모리의 주소를 가지고 온다 */
	for (int i = argc - 1; i >= 0; i--) {
		rsp -= sizeof (char *);
		*(char **) rsp = argv[i];
	}

	/* main(argc, argv)의 인자 레지스터 */
	if_->R.rdi = argc;
	if_->R.rsi = (uint64_t) rsp;
	
	/* 6. 가짜 반환 주소 */
	rsp -= sizeof (void *);
	*(void **) rsp = NULL;

	if_->rsp = (uint64_t) rsp;
}	

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
