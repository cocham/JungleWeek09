#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "threads/palloc.h"
#include <hash.h>

enum vm_type {
	/* page not initialized */
	VM_UNINIT = 0,
	/* page not related to the file, aka anonymous page */
	VM_ANON = 1,
	/* page that realated to the file */
	VM_FILE = 2,
	/* page that hold the page cache, for project 4 */
	VM_PAGE_CACHE = 3,

	/* Bit flags to store state */

	/* Auxillary bit flag marker for store information. You can add more
	 * markers, until the value is fit in the int. */
	VM_MARKER_0 = (1 << 3),
	VM_MARKER_1 = (1 << 4),

	/* DO NOT EXCEED THIS VALUE. */
	VM_MARKER_END = (1 << 31),
};

#include "vm/uninit.h"
#include "vm/anon.h"
#include "vm/file.h"
#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

struct page_operations;
struct thread;

#define VM_TYPE(type) ((type) & 7)

/* The representation of "page".
 * This is kind of "parent class", which has four "child class"es, which are
 * uninit_page, file_page, anon_page, and page cache (project4).
 * DO NOT REMOVE/MODIFY PREDEFINED MEMBER OF THIS STRUCTURE. */
struct page {
	const struct page_operations *operations;
	void *va;              /* Address in terms of user space */
	struct frame *frame;   /* Back reference for frame */
	bool writable;
	
	/* Your implementation */

	/* Per-type data are binded into the union.
	 * Each function automatically detects the current union */
	union {
		struct uninit_page uninit;
		struct anon_page anon;
		struct file_page file;
#ifdef EFILESYS
		struct page_cache page_cache;
#endif
	};
};

/* The representation of "frame" */
struct frame {
	void *kva;
	struct page *page;

	/*
	01. 물리 메모리 내 사용 가능한 프레임(페이지의 저장 공간)을 추적.
	02. 프레임 할당 및 해제 기능 구현.
	03. 프레임 테이블에 동기화(synchronization) 추가.
	*/
};


/* The function table for page operations.
 * This is one way of implementing "interface" in C.
 * Put the table of "method" into the struct's member, and
 * call it whenever you needed. */
struct page_operations {
	bool (*swap_in) (struct page *, void *);
	bool (*swap_out) (struct page *);
	void (*destroy) (struct page *);
	enum vm_type type;
};

#define swap_in(page, v) (page)->operations->swap_in ((page), v)
#define swap_out(page) (page)->operations->swap_out (page)
#define destroy(page) \
	if ((page)->operations->destroy) (page)->operations->destroy (page)

/* Representation of current process's memory space.
 * We don't want to force you to obey any specific design for this struct.
 * All designs up to you for this. */
struct supplemental_page_table {
	/*
	[역할]
	01. 페이지 폴트 시 커널이 추가 페이지 테이블에서 오류가 발생한 가상 페이지를 조회하여 어떤 데이터가 있어야 하는지 확인
	02. 프로세스가 종료될 때 커널이 SPT를 참조하여 어떤 리소스를 free시킬 것인지 결정

	할당받은 페이지에 대한 정보(unint, anonymouse, file-backed)를 가지고 있음
	*/

	/* 자료 구조 선택 
	배열, 리스트, 비트맵, 해시 테이블(내장 함수 제공)
	*/

	/* 필드 타입 선택
	- 세그먼트 (연속된 페이지 그룹_실행 파일이나 메모리 매핑 파일이 포함된 메모리 영역) 
	- 페이지
	*/

	/* 해시로 구현하는 경우
	필요한 값: VA, page, hash_elem
	먼저 SPT에 해시 구조체를 만들어주어야 한다. 
	SPT 자체는 해시 구조체가 아니기 때문이다.
	그러면 SPT 내부의 해시 구조체에 들어갈 엔트리도 만들어줘야 한다. 
	page 구조체 자체에 hash_elem을 추가해서 엔트리로 사용할 수도 있지만, 
	그렇게 하지 않고 wrapper 엔트리 생성
	*/
	struct hash SPT_hash;

};

struct spt_entry {
	void *va; /* 키 (가상주소) */
	struct page *page; /* 값 (페이지) */
	struct hash_elem elem;
};

struct vm_entry {
	int VM_BIN;
	uint32_t zero_bytes;
	uint32_t read_bytes;
	off_t offset;
	struct file file; //이렇게 하면 복사됨. file포인터로 수정?
};


#include "threads/thread.h"
void supplemental_page_table_init (struct supplemental_page_table *spt);
bool supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src);
void supplemental_page_table_kill (struct supplemental_page_table *spt);
struct page *spt_find_page (struct supplemental_page_table *spt,
		void *va);
bool spt_insert_page (struct supplemental_page_table *spt, struct page *page);
void spt_remove_page (struct supplemental_page_table *spt, struct page *page);

void vm_init (void);
bool vm_try_handle_fault (struct intr_frame *f, void *addr, bool user,
		bool write, bool not_present);

#define vm_alloc_page(type, upage, writable) \
	vm_alloc_page_with_initializer ((type), (upage), (writable), NULL, NULL)
bool vm_alloc_page_with_initializer (enum vm_type type, void *upage,
		bool writable, vm_initializer *init, void *aux);
void vm_dealloc_page (struct page *page);
bool vm_claim_page (void *va);
enum vm_type page_get_type (struct page *page);

#endif  /* VM_VM_H */
