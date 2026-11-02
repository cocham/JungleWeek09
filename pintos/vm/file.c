/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/thread.h"
#include <round.h>
#include "userprog/process.h"
#include "threads/vaddr.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {

	struct vm_entry *aux = page->uninit.aux;
	 
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;

	// uint32_t zero_bytes = aux->zero_bytes;
	// uint32_t read_bytes = aux->read_bytes;
	// off_t offset = aux->offset;
	// struct file *file = aux->file; 
	// size_t page_cnt = aux->page_cnt;
	// void * addr = page->va;
	
	if (aux != NULL) {
		file_page->file = aux->file;
		file_page->offset = aux->offset;
		file_page->read_bytes = aux->read_bytes;
		file_page->zero_bytes = aux->zero_bytes;
		file_page->page_cnt = aux->page_cnt;
		file_page->addr = page->va;
	}

	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct thread *cur = thread_current();
	uint64_t *pml4 = cur->pml4;

	if (pml4_is_dirty(pml4, page->va)) {
		file_write_at(page->file.file, page->frame->kva, page->file.read_bytes, page->file.offset);
	};
	
	pml4_clear_page (cur->pml4, page->va);
	palloc_free_page(page->frame->kva);
	free(page->frame);
	page->frame = NULL;
	
	struct file_page *file_page UNUSED = &page->file;
}

/* Do the mmap */
/*
	* [do_mmap 흐름]
	*
	* 0. 세팅
	*    - 현재 스레드의 SPT 가져오기
	*    - 시작 주소 따로 보관 (루프에서 addr이 바뀌니까 리턴용)
	*    - 전체 페이지 수 = length를 PGSIZE 단위로 올림
	*
	* 1. 겹침 검사 (등록 전에 전체 범위 확인)
	*    - 매핑 범위의 페이지마다 spt_find_page
	*    - 하나라도 이미 있으면 NULL 리턴 (코드/데이터/스택/다른 mmap과 겹침)
	*
	* 2. 파일 다시 열기
	*    - file_reopen으로 매핑 전용 file 확보 (유저가 close해도 매핑 유지)
	*    - 실패하면 NULL 리턴
	*
	* 3. 총 읽을 양과 0으로 채울 양 계산
	*    - offset이 파일 끝을 넘으면 읽을 게 없음
	*    - 아니면 읽을 양 = min(파일에 남은 양, length)
	*    - 0 채울 양 = 페이지 수 × PGSIZE - 읽을 양
	*
	* 4. 페이지마다 등록 (load_segment와 같은 구조)
	*    - 이번 페이지 몫 계산: page_read_bytes, page_zero_bytes
	*    - vm_entry(aux) malloc 후 채우기
	*        file(reopen한 것), offset, read_bytes, zero_bytes,
	*        mmap_start(시작 주소), page_cnt(전체 페이지 수)
	*    - VM_FILE + lazy_load_file로 vm_alloc_page_with_initializer
	*    - 실패하면 aux 해제하고 NULL 리턴
	*    - 다음 페이지로: 남은 양 줄이고, offset += 읽은 양, addr += PGSIZE
	*
	* 5. 성공
	*    - 보관해 둔 시작 주소 리턴
	*/
void *
do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset) {

	/* 0. 세팅
	*    - 현재 스레드의 SPT 가져오기
	*    - 시작 주소 따로 보관 (루프에서 addr이 바뀌니까 리턴용)
	*    - 전체 페이지 수 = length를 PGSIZE 단위로 올림
	*/
	
	struct supplemental_page_table *spt = &thread_current()->spt;
	uintptr_t start = (uintptr_t) addr;
	size_t page_cnt = DIV_ROUND_UP(length, PGSIZE);

	/* 1. 겹침 검사 (등록 전에 전체 범위 확인)
	*    - 매핑 범위의 페이지마다 spt_find_page
	*    - 하나라도 이미 있으면 NULL 리턴 (코드/데이터/스택/다른 mmap과 겹침)
	*/
	for (size_t i = 0; i < page_cnt; i++) {
		if (spt_find_page (spt, addr + i * PGSIZE) != NULL) {
			return NULL;
		}
	}

	/*
	* 2. 파일 다시 열기
	*    - file_reopen으로 매핑 전용 file 확보 (유저가 close해도 매핑 유지)
	*    - 실패하면 NULL 리턴
	*/

	struct file *map_file = file_reopen (file);
	if (map_file == NULL) {
		return NULL;
	}
	

	/*
	* 3. 총 읽을 양과 0으로 채울 양 계산
	*    - offset이 파일 끝을 넘으면 읽을 게 없음
	*    - 아니면 읽을 양 = min(파일에 남은 양, length)
	*    - 0 채울 양 = 페이지 수 × PGSIZE - 읽을 양
	*/
	off_t file_len = file_length (map_file); // 파일 전체 크기
	uint32_t read_bytes = 0;
	if (offset < file_len) {
		read_bytes = file_len - offset;
		if (read_bytes > length) {
				read_bytes = length;
		}
	}
	// size_t read_bytes = offset < file_len 
	// 	? ((size_t) (file_len - offset) > length ? 
	// length : (size_t) (file_len - offset)) 
	// 	: 0;

	

	uint32_t zero_bytes = (page_cnt * PGSIZE) - read_bytes;

	while (read_bytes > 0 || zero_bytes > 0) {

		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

			
		struct vm_entry *aux = malloc(sizeof(struct vm_entry));
		if (aux == NULL) return NULL;

		aux->file = map_file;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;
		aux->offset = offset;
		aux->page_cnt = page_cnt;

		if (!vm_alloc_page_with_initializer (VM_FILE, addr, writable, lazy_load_segment, aux)) {
			if (aux->file) file_close(aux->file);
			free(aux);
			return NULL;
		}

		/* 다음 페이지로 이동 */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		offset += page_read_bytes;
		addr += PGSIZE;
	}

	return start;
	
}

/* Do the munmap */
void
do_munmap (void *addr) {

	uintptr_t start = (uintptr_t) addr;
	struct page *page;
	struct supplemental_page_table *spt = &thread_current()->spt;
	struct file *file;

	page = spt_find_page(spt,addr);
	
	enum vm_type vm = page->operations->type;
	int count ;

	if(VM_TYPE(vm) == VM_UNINIT){
		struct vm_entry *aux = page->uninit.aux;
		count = aux->page_cnt;
		file = aux->file;
	}else if (VM_TYPE(vm) == VM_FILE){
		count = page->file.page_cnt;
		file = page->file.file;
	}else{
		return NULL;
	}
	
	struct page *next;

	for(int j = 0; j < count; j++){
		next = spt_find_page (spt, addr + j * PGSIZE);
		spt_remove_page(spt, next);
	}
	
	
	file_close (file);
	

}
