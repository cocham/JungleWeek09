/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include <bitmap.h>
#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/vaddr.h"

#define SECTORS_PER_PAGE (PGSIZE / DISK_SECTOR_SIZE)

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static struct bitmap *swap_table;

static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1, 1);
	swap_table = bitmap_create(disk_size(swap_disk) / SECTORS_PER_PAGE);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;
	page->anon.swap_slot = BITMAP_ERROR;
	struct anon_page *anon_page = &page->anon;
	memset (kva, 0, PGSIZE);
	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	size_t slot = anon_page->swap_slot;
	
	if (slot == BITMAP_ERROR) {
		return false;
	}
	
	for (int i = 0; i < SECTORS_PER_PAGE; i++) {
		disk_read(swap_disk, slot * SECTORS_PER_PAGE + i, kva + i * DISK_SECTOR_SIZE);
	}

	bitmap_reset(swap_table,slot);

	anon_page->swap_slot = BITMAP_ERROR;
	
	return true;
}

/* Swap out the page by writing contents to the swap disk. */
// pml4_clear_page(thread_current()->pml4, page->va); // 페이지 테이블에서 가상 주소 제거
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	struct frame *frame = page->frame;
	size_t slot = bitmap_scan_and_flip (swap_table,0,1,false);
	
	if (slot == BITMAP_ERROR) {
		PANIC("No available swap slot");
	}

	anon_page->swap_slot = slot;
	
	for (int i = 0; i < SECTORS_PER_PAGE; i++) {
		disk_write(swap_disk, slot * SECTORS_PER_PAGE + i, frame->kva + i * DISK_SECTOR_SIZE);
	}

	page->frame = NULL;
	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	
	if (anon_page->swap_slot != BITMAP_ERROR) {
		bitmap_reset (swap_table, anon_page->swap_slot);
	}

	if (page->frame != NULL) {
		pml4_clear_page(thread_current()->pml4, page->va);
		palloc_free_page(page->frame->kva);
		list_remove(&page->frame->frame_elem);
		page->frame = NULL;
		free(page->frame);
	}

}
