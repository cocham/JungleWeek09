#include "userprog/syscall.h"
#include <string.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/synch.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/vaddr.h"     // is_user_vaddr
#include "threads/mmu.h"       // pml4_get_page
#include "filesys/filesys.h"   // filesys_open
#include "filesys/file.h"      // file_read, file_write, file_close
#include "devices/input.h"     // input_getc
#include "userprog/process.h"
#include "threads/palloc.h"
#include "threads/init.h" 

typedef int pid_t;
struct lock file_lock;


void sys_exit(int status); // 일부 구현
pid_t sys_fork (const char *thread_name, struct intr_frame *f); 
int sys_exec (const char *file);
int sys_wait (pid_t pid); // 민성 ^&^
bool create (const char *file, unsigned initial_size); // 구현/테스트x
bool remove (const char *file); // 구현/테스트x
int sys_open(const char *file); // 구현/테스트X
int filesize (int fd); // 구현/테스트x
int sys_read(int fd, void *buffer, unsigned size); // 구현/테스트X
int sys_write(int fd, const void *buffer, unsigned size); // 구현/테스트X
void seek (int fd, unsigned position); // 미구현
unsigned tell (int fd); // 미구현
void sys_close (int fd); // 구현/테스트X

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	lock_init(&file_lock);

	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

// 포인터가 존재하는지 확인하는 유틸 함수
static void 
check_address(const void *ptr){
	if(ptr == NULL || !is_user_vaddr(ptr) || !pml4_get_page(thread_current()->pml4, ptr)){
		sys_exit(-1);
	}
}

static void
check_buffer(const void *buf, unsigned size){
	if (size == 0){
		return;
	}

	const uint8_t *start = buf; // 버퍼의 시작
	const uint8_t *end = start + size - 1; //주소의 끝

	// 주소의 끝까지 페이지를 이동하면서 구경
	for(const uint8_t *page = pg_round_down(start); page <= end; page += PGSIZE){
		check_address(page);
	}
}

// 문자열이 걸친 모드 페이지가 유효한지 안한지 확인을 진행 (NULL문자 '\0' 까지)
static void
check_string (const char *str) {
	check_address(str);
	for (const char *p = str; *p != '\0'; p++) {
		if (pg_ofs (p+1) == 0) {
			check_address(p+1);
		}
	}
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {

	// rax = 시스템 콜 번호
	// rip; 돌아갈 주소
    // cs; 돌아갈 모드
    // eflags;
    // rsp; 유저 스택 위치

	// gp_register
	// 인수는 %rdi, %rsi, %rdx, %r10, %r8, %r9 순서로 전달
	// rcx는 다른 곳에 쓰임

	// 반환 값은 RAX 레지스터에 저장하는 것이 관례입니다. 
	// 값을 반환하는 시스템 호출은 해당 rax 레지스터의 멤버를 수정하여 값을 반환할 수 있습니다
	// struct intr_frame

	// 입구 코드 (syscall-entry.S)

	// 유저 rsp를 잠깐 보관하고, TSS에서 커널 스택 위치를 꺼내 rsp를 바꿉니다.
	// 커널 스택에 차례로 저장합니다. 이 묶음이 intr_frame입니다.
	// 돌아갈 정보: rip(rcx에 있던 값), cs, rflags(r11에 있던 값), 유저 rsp, ss
	// 일반 레지스터 전부 → R
	// 그 묶음의 주소를 인자로 syscall_handler(f)를 부릅니다.

	// syscall-entry.S에서 호출함
	// movabs $syscall_handler, %r12

	// TODO: Your implementation goes here.
	switch(f->R.rax){
		case SYS_HALT: // 민구
			power_off ();
			break;
		case SYS_EXIT:
			sys_exit((int) f->R.rdi); // 종료 호출
			break;
		case SYS_FORK:
			f->R.rax = sys_fork((const char *)f->R.rdi, f);
			break;
		case SYS_EXEC:
			f->R.rax = sys_exec((const char *) f->R.rdi);
			break;
		case SYS_WAIT: // 민성
			f->R.rax = sys_wait((int) f->R.rdi);
			break;
		case SYS_CREATE: // 민성
			f->R.rax = create((char *) f->R.rdi,(unsigned) f->R.rsi);
			break;
		case SYS_REMOVE: // 민성
			f->R.rax = remove((char *) f->R.rdi);
			break;
		case SYS_OPEN: // 민구
			f->R.rax = sys_open((char *) f->R.rdi);
			break;
		case SYS_FILESIZE: // 민성
			f->R.rax = filesize((int) f->R.rdi);
			break;
		case SYS_READ: // 민구
			f->R.rax = sys_read((int) f->R.rdi, (void *) f->R.rsi, (unsigned) f->R.rdx);
			break;
		case SYS_WRITE: // 민구
			f->R.rax = sys_write ((int) f->R.rdi, (const void *) f->R.rsi, (unsigned) f->R.rdx);
			break;
		case SYS_SEEK: // 민성
			seek((int) f->R.rdi, (int) f->R.rsi);
			break;
		case SYS_TELL: // 민성
			f->R.rax = tell((int) f->R.rdi);
			break;
		case SYS_CLOSE:
			sys_close ((int) f->R.rdi);
			break;
		// Project Part 3 - 4
		// case SYS_MMAP:
		// 	break;
		// case SYS_MUNMAP:
		// 	break;
		// case SYS_CHDIR:
		// 	break;
		// case SYS_MKDIR:
		// 	break;
		// case SYS_READDIR:
		// 	break;
		// case SYS_ISDIR:
		// 	break;
		// case SYS_INUMBER:
		// 	break;
		// case SYS_SYMLINK:
		// 	break;
		// case SYS_DUP2:
		// 	break;
		// case SYS_MOUNT:
		// 	break;
		// case SYS_UMOUNT:
		// 	break;
		default:
			sys_exit(-1); // 시스템콜 오류
			break;

	}
}

// void 
// halt(void){
// 	power_off();
// }
unsigned 
tell (int fd){
	struct file *file = fd_get(fd);

	if(file == NULL){ 
		return -1;
	}

	return file_tell(file);
}

int
sys_exec (const char *file) {
	check_string(file); // 사용자 문자열 검증

	char *cmd_copy = palloc_get_page (0); // 커널 페이지에 복사
	if (cmd_copy == NULL) {
		sys_exit (-1);
	}
	strlcpy(cmd_copy, file, PGSIZE);

	if (process_exec (cmd_copy) == -1) { // <- 프로그램을 교체를 성공하면 다시 돌아오지 않음 (구 작업을 영원히 잃어버림)
		sys_exit(-1);
	}

	NOT_REACHED(); // <- 커널 작동 중지 PANIC("executed an unreachable statement")과 같음
}

// int read(int fd, void *buffer, unsigned size){
void seek (int fd, unsigned position){
	struct file *file = fd_get(fd);
	if(file == NULL){ 
		return ;
	}
	file_seek(file,position);
}

int 
filesize (int fd){
	struct file *file = fd_get(fd);

	if(file == NULL){ 
		return -1;
	}

	return file_length(file);
} 

bool
create(const char *file, unsigned initial_size){
	bool result;
	check_address(file);
	lock_acquire(&file_lock);
	
	result = filesys_create(file,initial_size);
	
	lock_release(&file_lock);
	return result;
}

bool
remove(const char *file){
	bool result;
	check_address(file);
	lock_acquire(&file_lock);
	
	result = filesys_remove(file);
	
	lock_release(&file_lock);
	return result;
}

int 
sys_wait (pid_t pid){
	return process_wait(pid);
}

pid_t 
sys_fork (const char *thread_name, struct intr_frame *f){
	check_address(thread_name);
	return process_fork(thread_name, f);
}

int 
sys_open(const char *file_name){ 
	// 파일의 주소를 fd에 넣고 넣은 인덱스를 반환
	// 인덱스를 기억하는 건 유저 프로그램의 책임
	int fd;

	check_address(file_name); // 포인터가 실제로 존재하는지 확인

	lock_acquire(&file_lock);

	struct file *cur_file = filesys_open(file_name);
	
	lock_release(&file_lock);

	if (cur_file == NULL){
		return -1;
	}

	// fd 인덱스 찾아서 넣고 가져오기
	fd = fd_alloc(cur_file); // 아직 구현안함

	if(fd == -1){
		// 파일을 닫아줘야 함
		lock_acquire(&file_lock);
		file_close(cur_file); // 파일 오픈으로 연걸 닫아야 함
		lock_release(&file_lock);

		return -1;
	}

	return fd;
}

int 
sys_read(int fd, void *buffer, unsigned size){
	check_buffer(buffer, size); // 버퍼가 정상인지 검사
	int result = -1;

	// 키보드
	if(fd == 0){
		uint8_t *buf = buffer; // void 말고 8 포인터로 형변환
		for (unsigned i = 0; i < size; i++)
		{
			buf[i] = input_getc();
		}
		result = size;
	}
	else if (fd == 1){
		return -1;
	}
	else{
		struct file *file = fd_get(fd);

		if(file == NULL){ // 널검사
			return -1;
		}

		lock_acquire(&file_lock);
		result = file_read(file, buffer, size); // 읽은 값 반환
		lock_release(&file_lock);
	}
	return result;
}

int 
sys_write (int fd, const void *buffer, unsigned size){
	int result = -1;
	check_buffer(buffer, size);

	if(fd == 0){ // 사용 불가
		return -1;
	}
	else if (fd == 1) { // 버퍼 내용을 화면에 출력
		putbuf (buffer, size);
		return size;
	}
	else{ // fd 매핑해서 읽어오기
		struct file *file = fd_get(fd);

		if(file == NULL){ // 널검사
			return -1;
		}

		lock_acquire(&file_lock);
		result = file_write(file, buffer, size); // 쓴 값 반환
		lock_release(&file_lock);
	}
	return result;
}

void 
sys_close (int fd){
	// 음수나 예외 처리
	// 모든 경우를 처리할 수는 없긴 함
	if (fd < 2){ 
		return;
	}

	lock_acquire(&file_lock);
	fd_close(fd); // 여기서 락을 잡았으니, fd_close에서 락을 잡으면 안됌
	lock_release(&file_lock);
}

// 완성
void
sys_exit(int status){
	thread_current()->exit_status = status;
	thread_exit();
}
