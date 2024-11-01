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
    // Calculate the starting page frame number
    u64 start_pfn = range_start.pfn();
    u64 end_pfn = start_pfn + page_count;

    // Traverse through all pages in the range to insert
    while (start_pfn < end_pfn) {
        int order = LastOrder;

        // Find the largest possible order for the current block that does not exceed boundaries
        while (order > 0 && ((start_pfn + pages_per_block(order)) > end_pfn || !block_aligned(order, start_pfn))) {
            order--;
        }

        // Get the page corresponding to this start_pfn
        page &block_start = page::get_from_pfn(start_pfn);

        // Set the free block size to current order (for later reference)
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
    while (page_count > 0) {
        // Find the order for the current starting page
        int order = find_order(range_start);

        if (order == -1) {
            // If no order found, exit (block not in free list)
            return;
        }

        // Calculate the block size for the found order
        u64 block_size = pages_per_block(order);

        // Remove the block from the free list.
        remove_free_block(order, range_start);

        // Calculate the next page in the range to continue removing.
        u64 next_block_pfn = range_start.pfn() + block_size;
        range_start = page::get_from_pfn(next_block_pfn);

        // Reduce the page count by the size of the removed block.
        page_count -= block_size;

        // Update the total count of free pages.
        total_free_ -= block_size;
    }
}

/**
 * Finds the order of the block containing the given range start.
 *
 * @param range_start - The starting page for which to find the order
 * @return - The order of the block if found, otherwise -1
 */
int page_allocator_buddy::find_order(page &range_start) const {
    for (int order = 0; order <= LastOrder; ++order) {
        page *current = free_list_[order];
        while (current) {
            if (current->base_address() == range_start.base_address()) {
                return order; // Found the order containing range_start
            }
            current = current->next_free_;
        }
    }
    return -1; // Not found
}

/**
 * Finds the index in the free list of a specific order where the range start is located.
 *
 * @param order - The order of the block to search in
 * @param range_start - The starting page to find
 * @return - The index of the free block if found, otherwise -1
 */
int page_allocator_buddy::find_index(int order, page &range_start) const {
    page *current = free_list_[order];
    int index = 0;

    while (current) {
        if (current->base_address() == range_start.base_address()) {
            return index; // Found the block
        }
        current = current->next_free_;
        index++;
    }

    return -1; // Not found
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