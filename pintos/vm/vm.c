/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "vm/hash.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Returns a hash value for page p. */
unsigned
page_hash (const struct hash_elem *p_, void *aux UNUSED) {
  const struct page *p = hash_entry (p_, struct page, hash_elem);
  return hash_bytes (&p->addr, sizeof p->addr);
}

/* Returns true if page a precedes page b. */
bool
page_less (const struct hash_elem *a_,
           const struct hash_elem *b_, void *aux UNUSED) {
  const struct page *a = hash_entry (a_, struct page, hash_elem);
  const struct page *b = hash_entry (b_, struct page, hash_elem);

  return a->addr < b->addr;
}

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable, vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		
		struct page *new_page = malloc(sizeof(struct page));
		
		if (new_page == NULL) {
			return false;
		}

		// VM_TYPE에 따른 초기화를 진행
		/*
		VM_ANON = 1
		VM_FILE = 2
		*/
		switch (VM_TYPE(type))
		{
		case VM_ANON:
			uninit_new(page, upage, init, type, aux, anon_initializer);
			break;
		case VM_FILE:
			uninit_new(page, upage, init, type, aux, file_backed_initializer);
			break;
			
		default:
			free(page);						

			goto err;
			break;
		}

		page->writable = writable;

		if (!spt_insert_page(spt,new_page)) {
			free(page);
			goto err;
		}

		return true;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	// struct page *page = NULL;
	/* TODO: Fill this function. */
	struct page *finding_page = NULL;
	
	struct spt_entry look_up;
	lookup.va = pg_round_down(va); // 해시 테이블 인덱스 == SPT 인덱스. va로 해당 블록을 찾아감(0번째 1번째 등등...)

	struct hash_elem *finding_hash_elem = hash_find(&spt->SPT_hash, &look_up.elem);
	
	if (finding_hash_elem != NULL) {
		finding_page = hash_entry(finding_hash_elem, struct spt_entry, hash_elem)->page;
	}

	return finding_page;
}


/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	/* TODO: Fill this function. */
	
	/* 예외 처리 */
	if (spt == NULL || page == NULL) {
		return false;
	}
	
	// 메모리 할당
	struct spt_entry *entry = malloc(sizeof(struct spt_entry));

	if (entry == NULL) {
		return false;
	}
	
	entry->va = page->va; // 가상 주소
	entry->page = page; // 페이지 참조

	if (hash_insert (struct hash *hash, struct hash elem *element) != NULL) {
		free(entry);
		return false;
	}
	
	return true;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = malloc(sizeof(struct frame));
	
	ASSERT(frame != NULL);

	frame->kva = palloc_get_page(PALLOC_USER);
	
	if (frame->kva == NULL) { // frame에서 가용 가능한 페이지가 없다면

		free(frame);
		PANIC("todo");
	}
	frame->page = NULL; // 페이지 멤버 초기화
	/* TODO: Fill this function. */
	/*  user pool by calling palloc_get_page (PALLOC_USER)  */
	/* allocates a frame(kva), initialize its members(page), and returns it */
	/* fail -> PANIC ("todo") */


	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = spt_find_page(&thread_current()->spt, va);
	/* TODO: Fill this function */

	/* 인자로 주어진 va에 페이지를 할당하고, 해당 페이지에 프레임을 할당한다.
	우선 한 페이지를 얻어야 하고 그 이후에 해당 페이지를 인자로 갖는 vm_do_claim_page 함수를 호출한다.
	*/
	if (page == NULL) {
		return false;
	}
	
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	if (frame == NULL) {
		return false;
	}

	/* Set links */
	frame->page = page;
	page->frame = frame;
	
	if (!pml4_set_page(thread_current ()->pml4, page->va, frame->kva, page->rw)) {
		vm_dealloc_page(page); /* 문제 발생 시 추후 수정 (페이지를 물리 메모리와 가상 공간에서 아예 삭제)*/
		return false;
	}
	
	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	/* 
	인자로 주어진 page에 물리 메모리 프레임을 할당한다.
	먼저, vm_get_frame 함수를 호출함으로써 프레임 하나를 가져온다.
	그런 다음 MMU를 설정한다.
	→ 가상 주소와 물리 주소를 매핑한 정보를 페이지 테이블에 추가해야 한다는 것을 의미
  	*/

	/* set up the MMU. */
	/* add the mapping from the virtual address to the physical address in the page table. */
	
	return swap_in (page, frame->kva); /* return page->operations->swap_in(page, frame->kva); */
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	if(!hash_init(&spt->SPT_hash, page_hash, page_less, NULL)) {
		return;
	}
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}
