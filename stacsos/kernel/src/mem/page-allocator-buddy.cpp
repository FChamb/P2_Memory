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
        // Determine the largest block size that fits within the range
        int order = 0;
        u64 max_block_size = 1;
        while ((start_pfn + max_block_size <= end_pfn) && block_aligned(order, start_pfn) && order < LastOrder) {
            order++;
            max_block_size <<= 1;
        }
        order--;
        max_block_size >>= 1;

        // Retrieve the block starting page
        page &block_start = page::get_from_pfn(start_pfn);
        block_start.free_block_size_ = max_block_size;

        // Insert the block into the appropriate free list
        insert_free_block(order, block_start);

        // Move start_pfn forward by the size of the inserted block
        start_pfn += max_block_size;
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

    while (start_pfn < end_pfn) {
        // Get the current page to remove from free list
        page &block_start = page::get_from_pfn(start_pfn);

        // Determine block order based on `free_block_size_`
        int order = 0;
        u64 block_size = block_start.free_block_size_;
        if (block_size > 0) {
            // Calculate the order if `free_block_size_` is set
            while ((1ULL << order) < block_size) {
                order++;
            }
        } else {
            // Fallback to alignment-based order calculation if `free_block_size_` is zero
            while (order <= LastOrder && !block_aligned(order, start_pfn)) {
                order++;
            }
        }

        // Ensure order is within bounds
        assert(order <= LastOrder);

        // Remove block from the free list at determined order
        remove_free_block(order, block_start);

        // Move to the next block based on the current block's size
        start_pfn += pages_per_block(order);
    }
}

void page_allocator_buddy::insert_free_block(int order, page &block_start) {
    // assert order in range
    assert(order >= 0 && order <= LastOrder);

    // assert block_start aligned to order
    assert(block_aligned(order, block_start.pfn()));

    page *target = &block_start;
    page **slot = &free_list_[order];
    while (*slot && *slot < target) {
        slot = &((*slot)->next_free_);
    }

    assert(*slot != target);

    target->next_free_ = *slot;
    *slot = target;
}

void page_allocator_buddy::remove_free_block(int order, page &block_start) {
    // assert order in range
    assert(order >= 0 && order <= LastOrder);

    // assert block_start aligned to order
    assert(block_aligned(order, block_start.pfn()));

    page *target = &block_start;
    page **candidate_slot = &free_list_[order];
    while (*candidate_slot && *candidate_slot != target) {
        candidate_slot = &((*candidate_slot)->next_free_);
    }

    // assert candidate block exists
    assert(*candidate_slot == target);

    *candidate_slot = target->next_free_;
    target->next_free_ = nullptr;
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

/**
 * Frees a block of pages and attempts to merge with buddies to form larger blocks.
 *
 * @param block_start - Starting page of the block to free
 * @param order - Order of the block to free
 */
void page_allocator_buddy::free_pages(page &block_start, int order) {
    // Ensure order is within range
    assert(order >= 0 && order <= LastOrder);

    block_start.state_ = page_state::free;
    insert_free_block(order, block_start);
    total_free_ += pages_per_block(order);

    // Attempt to merge with buddies
    while (order < LastOrder) {
        u64 buddy_pfn = block_start.pfn() ^ pages_per_block(order);
        page &buddy = page::get_from_pfn(buddy_pfn);

        // Check if buddy is free and aligned
        if (buddy.state_ != page_state::free || !block_aligned(order, buddy.pfn())) {
            break;
        }

        // Merge the block with its buddy and move to the next order
        merge_buddies(order, block_start);
        order++;
    }
}