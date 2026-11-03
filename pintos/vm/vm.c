/* vm.c: Generic interface for virtual memory objects. */

#include <string.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "userprog/syscall.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "lib/kernel/hash.h"
#include "threads/vaddr.h"


static struct list frame_table;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
	list_init (&frame_table);
	
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
void kill_spt_entry (struct hash_elem *e, void *aux);
/* Returns a hash value for page p. */
unsigned
page_hash (const struct hash_elem *p_, void *aux UNUSED) {
  const struct spt_entry *p = hash_entry (p_, struct spt_entry, elem);
  return hash_bytes (&p->va, sizeof p->va);
}

/* Returns true if page a precedes page b. */
bool
page_less (const struct hash_elem *a_,
           const struct hash_elem *b_, void *aux UNUSED) {
  const struct spt_entry *a = hash_entry (a_, struct spt_entry, elem);
  const struct spt_entry *b = hash_entry (b_, struct spt_entry, elem);

  return a->va < b->va;
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
			uninit_new(new_page, upage, init, type, aux, anon_initializer);
			break;
		case VM_FILE:
			uninit_new(new_page, upage, init, type, aux, file_backed_initializer);
			break;
			
		default:
			free(new_page);						

			goto err;
			break;
		}

		new_page->writable = writable;

		if (!spt_insert_page(spt,new_page)) {
			free(new_page);
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
	look_up.va = pg_round_down(va); // 해시 테이블 인덱스 == SPT 인덱스. va로 해당 블록을 찾아감(0번째 1번째 등등...)

	struct hash_elem *finding_hash_elem = hash_find(&spt->SPT_hash, &look_up.elem);
	
	if (finding_hash_elem != NULL) {
		finding_page = hash_entry(finding_hash_elem, struct spt_entry, elem)->page;
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

	if (hash_insert (&spt->SPT_hash, &entry->elem) != NULL) {
		free(entry);
		return false;
	}
	
	return true;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	
	struct spt_entry look_up;
	look_up.va = page->va;
	
	struct hash_elem *e = hash_delete(&spt->SPT_hash, hash_find(&spt->SPT_hash, &look_up.elem));
	
	if (e != NULL) {
		struct spt_entry *find_spt_entry = hash_entry(e, struct spt_entry, elem);
		free(find_spt_entry);
	}
	
	vm_dealloc_page (page);
	
	// return true;
}

static struct list_elem *
clock_next (struct list_elem *e) {
	if (e->next == list_end (&frame_table))
		return list_begin (&frame_table);
	else
		return e->next;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct list_elem *find_elem = list_begin(&frame_table);
	struct frame *find_frame;
	 /* TODO: The policy for eviction is up to you. */
	
	while (true) {
		find_frame = list_entry(find_elem, struct frame, frame_elem);

		/* 아직 page가 연결되지 않은 frame(claim 도중)은 건너뜀 */
		if (find_frame->page == NULL) {
			find_elem = clock_next (find_elem);
			continue;
		}

		uint64_t *pml4 = find_frame->my_thread->pml4;
		
		if(pml4_is_accessed(pml4,find_frame->page->va)) {
			pml4_set_accessed(pml4,find_frame->page->va,false);
			find_elem = clock_next(find_elem);
		}else{
			return find_frame;
		}
	}
}

// static struct frame *
// vm_get_victim (void) {
// 	struct frame *victim = NULL;
// 	struct list_elem *find_elem = list_begin(&frame_table);
// 	for (size_t i = 0; i < 2 * list_size(&frame_table); i++) {
// 		struct frame *f = list_entry(find_elem, struct frame, frame_elem);
// 		clock_next(find_elem);

// 		uint64_t *pml4 = f->my_thread->pml4;
// 		if (pml4_is_accessed(pml4, f->page->va)) {
// 			pml4_set_accessed(pml4, f->page->va, false);
// 		} else {
// 			victim = f;
// 			break;
// 		}
// 		find_elem = clock_next(find_elem);
// 	}
// 	return NULL
// }

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */
	
	if(victim == NULL) {
		return NULL;
	}

	struct page *victim_page = victim->page;
	pml4_clear_page (victim->my_thread->pml4, victim->page->va);

	if (!swap_out (victim_page)) {
		return NULL;
	}

	victim_page->frame = NULL;
	victim->page = NULL;
	
	return victim;	
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = malloc(sizeof(struct frame));
	
	ASSERT(frame != NULL);

	frame->kva = palloc_get_page(PAL_USER);
	if (frame->kva == NULL) { // frame에서 가용 가능한 페이지가 없다면
		free(frame);
		frame = vm_evict_frame();
		if (frame == NULL){
			return NULL;
		}
	} else {
		list_push_back (&frame_table, &frame->frame_elem);
	}
	
	frame->page = NULL; // 페이지 멤버 초기화
	frame->my_thread = thread_current();

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
	vm_alloc_page(VM_ANON, pg_round_down(addr), true);
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED, bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	
	if (addr == NULL || !is_user_vaddr(addr)){
		return false;
	}
	
	if (!not_present) {
		return false;
	}


	struct page *page = spt_find_page(spt,addr);
	if (page != NULL) {
		if (write && !page->writable) {
			return false;
		}

		return vm_do_claim_page (page);
	}

	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	if(page == NULL){
		
		// 조건 1 user_addr,
		// 조건 2 1<<20 으로 
		// a >= rsp - 8 && a >= stakc의 1MB크기 && 

		uintptr_t rsp = user ? f->rsp : thread_current ()->user_rsp;

		uintptr_t fault_addr = (uintptr_t) addr;
		
		if (fault_addr >= rsp - 8 && fault_addr >= USER_STACK - ONE_MB && fault_addr < USER_STACK) {
			
			vm_stack_growth(addr);
			return vm_claim_page(addr);
		}
		
		return false;
	}

	return false;
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
	
	if (!pml4_set_page (thread_current ()->pml4, page->va, frame->kva, page->writable)) {
		page->frame = NULL;
		frame->page = NULL;
		list_remove (&frame->frame_elem);
		palloc_free_page (frame->kva);
		free (frame);
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
			 
			struct hash_iterator i;
			
			hash_first(&i,&src->SPT_hash);
			
			while(hash_next(&i)){
				
				struct spt_entry *src_entry = hash_entry(hash_cur(&i),struct spt_entry,elem);
				struct page *src_page = src_entry->page;

				enum vm_type type = src_page->operations->type;
				void *va = src_page->va;
				bool writable = src_page->writable;
				
				/* 부모 spt_entry를 자식 spt_entry로 복사 */
				// struct spt_entry *dst_entry = malloc(sizeof(struct spt_entry));

				// if (dst_entry == NULL)
				// 	return NULL;
				
				// dst_entry = memcpy(dst_entry, src_entry, sizeof(struct spt_entry));

				if (type == VM_UNINIT) {
					
					enum vm_type origin_type = src_page->uninit.type;
					vm_initializer *init = src_page->uninit.init;
					
					struct vm_entry *parent_info = src_page->uninit.aux;
					struct vm_entry *child_info = malloc(sizeof(struct vm_entry));
					
					if (child_info == NULL) {
						return false;
					}

					memcpy(child_info, parent_info, sizeof(struct vm_entry));

					if (!vm_alloc_page_with_initializer(origin_type, va, writable, init, child_info)) {
						//free(dst_entry);
						free(child_info);
						return false;
					}
				
				} else if (type == VM_FILE || type == VM_ANON) {
					/* 자식 페이지 만들고 frame 붙이기 */
					if (!vm_alloc_page (type, va, writable))
						return false;
					struct page *dst_page = spt_find_page (dst, va);
					if (!vm_do_claim_page (dst_page))
						return false;

					/* file 페이지는 파일 정보도 복사 (aux가 없어서 initializer가 못 채움) */
					if (type == VM_FILE)
						dst_page->file = src_page->file;

					/* 내용 복사: 위의 claim이 evict를 일으켰을 수 있으니 복사 직전에 부모 상태 확인 */
					if (src_page->frame != NULL) {
						memcpy (dst_page->frame->kva, src_page->frame->kva, PGSIZE);
					} else if (type == VM_ANON) {
						/* 부모는 swap 디스크에 있음: 읽기만 하고 부모 슬롯은 그대로 둠 */
						anon_copy_slot (src_page, dst_page->frame->kva);
					} else {
						struct file_page *fp = &src_page->file;                        /* 부모는 파일로 나가 있음 */
						bool held = lock_held_by_current_thread (&file_lock);
						if (!held) lock_acquire (&file_lock);
						file_read_at (fp->file, dst_page->frame->kva, fp->read_bytes, fp->offset);
						if (!held) lock_release (&file_lock);
						memset (dst_page->frame->kva + fp->read_bytes, 0, fp->zero_bytes);
					}
				}
								
			}
			return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	
	hash_clear(&spt->SPT_hash, kill_spt_entry);
	
	 // typedef void hash_action_func (struct hash_elem *e, void *aux);
}

void 
kill_spt_entry (struct hash_elem *e, void *aux) {	
	struct spt_entry *entry = hash_entry (e, struct spt_entry, elem);

	if (entry != NULL && entry->page != NULL) {
		vm_dealloc_page(entry->page);
	}

	free (entry);
}