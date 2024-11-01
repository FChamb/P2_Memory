/* SPDX-License-Identifier: MIT */

/* StACSOS - Kernel
 *
 * Copyright (c) University of St Andrews 2024
 * Tom Spink <tcs6@st-andrews.ac.uk>
 */
#include <stacsos/kernel/debug.h>
#include <stacsos/kernel/mem/page-allocator-buddy.h>
#include <stacsos/kernel/mem/page.h>
#include <stacsos/memops.h>

using namespace stacsos;
using namespace stacsos::kernel;
using namespace stacsos::kernel::mem;

void page_allocator_buddy::dump() const {
    dprintf("*** buddy page allocator - free list ***\n");

    for (int i = 0; i <= LastOrder; i++) {
        dprintf("[%02u] ", i);

        page *c = free_list_[i];
        while (c) {
            dprintf("%lx--%lx ", c->base_address(), (c->base_address() + ((1 << i) << PAGE_BITS)) - 1);
            c = c->next_free_;
        }

        dprintf("\n");
    }
}

void page_allocator_buddy::dump_free_list(int order) const {
    dprintf("Free list for order %d: ", order);
    page *c = free_list_[order];
    while (c) {
        dprintf("%llu ", c->pfn());
        c = c->next_free_;
    }
    dprintf("\n");
}

/**
 * Inserts a range of pages into the free lists, breaking them down into the
 * largest blocks that fit within the remaining page count. Each block is added
 * at the appropriate order level.
 *
 * @param range_start - Starting page in the range to insert
 * @param page_count - Number of pages to insert
 */
void page_allocator_buddy::insert_pages(page &range_start, u64 page_count) {
    u64 start_pfn = range_start.pfn();
    u64 end_pfn = start_pfn + page_count;

    while (start_pfn < end_pfn) {
        int order = LastOrder;

        // Find the largest possible order for the current block that does not exceed boundaries
        while (order > 0 && ((start_pfn + pages_per_block(order)) > end_pfn || !block_aligned(order, start_pfn))) {
            order--;
        }

        // If order becomes -1, it means no valid block size can fit.
        if (order < 0) {
            // In this case, just increment by 1 to handle unallocated pages
            start_pfn++;
            continue;
        }

        // Get the page corresponding to this start_pfn
        page &block_start = page::get_from_pfn(start_pfn);

        // Track the size of the block being added
        block_start.free_block_size_ = pages_per_block(order);

        // Insert the block into the appropriate free list
        insert_free_block(order, block_start);

        // Move the start pfn by the size of this block to continue with the next segment
        start_pfn += pages_per_block(order);
    }
}

/**
 * Removes a range of pages from the free lists, breaking them down into blocks
 * and removing each from the appropriate order level.
 *
 * @param range_start - Starting page in the range to remove
 * @param page_count - Number of pages to remove
 */
void page_allocator_buddy::remove_pages(page &range_start, u64 page_count) {
    u64 start_pfn = range_start.pfn();
    u64 end_pfn = start_pfn + page_count;

    for (int order = LastOrder - 1; start_pfn < end_pfn; ) {
        // Identify the largest order that fits
        while (order >= 0 && (start_pfn + pages_per_block(order)) > end_pfn) {
            order--;
        }

        assert(order >= 0);
        page &block_start = page::get_from_pfn(start_pfn);

        // Dump current free list state
        dump_free_list(order);

        // Debugging: Print the block being removed and its order
        dprintf("Removing block: start_pfn=%llu, order=%d\n", start_pfn, order);

        // Attempt to remove the block from the free list
        remove_free_block(order, block_start);

        start_pfn += pages_per_block(order);
    }
}

void page_allocator_buddy::insert_free_block(int order, page &block_start) {
    assert(order >= 0 && order <= LastOrder);
    assert(block_aligned(order, block_start.pfn()));

    page *target = &block_start;
    page **slot = &free_list_[order];

    // Insert the block in sorted order in the free list.
    while (*slot && *slot < target) {
        slot = &((*slot)->next_free_);
    }

    target->next_free_ = *slot;
    *slot = target;

    // Print the state of the free list after insertion for debugging
    dprintf("Inserted block: start_pfn=%llu, order=%d\n", target->pfn(), order);
}

void page_allocator_buddy::remove_free_block(int order, page &block_start) {
    assert(order >= 0 && order <= LastOrder);
    assert(block_aligned(order, block_start.pfn()));

    page *target = &block_start;
    page **candidate_slot = &free_list_[order];

    // Traverse the list to find the target
    while (*candidate_slot && *candidate_slot != target) {
        candidate_slot = &((*candidate_slot)->next_free_);
    }

    // Ensure we found the block
    if (*candidate_slot != target) {
        dprintf("Error: Block to remove not found in free list for order %d. Block start PFN: %llu\n", order, block_start.pfn());
        // Instead of panic, log and return if needed
        return;
    }

    // Remove the block from the list
    *candidate_slot = target->next_free_;
    target->next_free_ = nullptr;  // Clear next_free_ of the removed block
}

/**
 * Removes a free block of a specific order from the free list.
 *
 * @param order - Order of the block to remove
 * @param block_start - The starting page of the block to remove
 */
void page_allocator_buddy::split_block(int order, page &block_start) {
    // Ensure the order is valid
    assert(order > 0 && order <= LastOrder);

    // Remove the block from the current free list
    remove_free_block(order, block_start);

    // Split into two buddies of the next lower order
    int lower_order = order - 1;
    u64 block_size = pages_per_block(lower_order);

    page &buddy1 = block_start;
    page &buddy2 = page::get_from_pfn(buddy1.pfn() + block_size);

    insert_free_block(lower_order, buddy1);
    insert_free_block(lower_order, buddy2);

}

/**
 * Merges a free block with its buddy to form a larger block at the next higher order.
 *
 * @param order - Current order of the block and its buddy
 * @param block_start - The starting page of the block to merge
 */
void page_allocator_buddy::merge_buddies(int order, page &block_start) {
    // Ensure order is within range
    assert(order >= 0 && order < LastOrder);

    u64 block_size = pages_per_block(order);
    u64 buddy_pfn = block_start.pfn() ^ block_size;
    page &buddy = page::get_from_pfn(buddy_pfn);

    // Ensure buddy is free and aligned to order
    if (buddy.state_ == page_state::free && block_aligned(order, buddy.pfn())) {
        // Remove both blocks from the free list
        remove_free_block(order, block_start);
        remove_free_block(order, buddy);

        // Merge into a higher-order block
        page &merged_block = (buddy.pfn() < block_start.pfn()) ? buddy : block_start;
        insert_free_block(order + 1, merged_block);
    }
}

/**
 * Allocates pages by finding a free block of the requested order.
 * If no blocks of that order are available, higher order blocks are split.
 *
 * @param order - Order of pages to allocate
 * @param flags - Allocation flags (optional)
 * @return - Pointer to the allocated block, or nullptr if no block available
 */
page *page_allocator_buddy::allocate_pages(int order, page_allocation_flags flags) {
    // Ensure requested order is within range
    if (order < 0 || order > LastOrder) {
        return nullptr;
    }

    // Find the smallest available block at or above the requested order
    int current_order = order;
    while (current_order <= LastOrder && !free_list_[current_order]) {
        current_order++;
    }

    // No available blocks
    if (current_order > LastOrder) {
        return nullptr;
    }

    // Continuously split blocks until reaching the requested order
    while (current_order > order) {
        page &block = *free_list_[current_order];
        split_block(current_order, block);
        current_order--;
    }

    // Allocate the block at the desired order
    page *allocated_block = free_list_[order];
    remove_free_block(order, *allocated_block);
    allocated_block->state_ = page_state::allocated;
    total_free_ -= pages_per_block(order);
    return allocated_block;
}

void page_allocator_buddy::free_pages(page &block_start, int order) {
    // Ensure order is within range
    assert(order >= 0 && order <= LastOrder);

    // Check if the block is already free
    if (block_start.state_ != page_state::allocated) {
        dprintf("Warning: Attempting to free an already free block: PFN=%llu\n", block_start.pfn());
        return;  // Optionally handle this case, or panic if desired
    }

    block_start.state_ = page_state::free;
    insert_free_block(order, block_start);
    total_free_ += pages_per_block(order);

    // Attempt to merge with buddies
    while (order < LastOrder) {
        u64 buddy_pfn = block_start.pfn() ^ pages_per_block(order);
        page &buddy = page::get_from_pfn(buddy_pfn);

        if (buddy.state_ != page_state::free || !block_aligned(order, buddy.pfn())) {
            break;  // Stop merging if buddy is not free or misaligned
        }

        merge_buddies(order, block_start);
        order++;
    }
}