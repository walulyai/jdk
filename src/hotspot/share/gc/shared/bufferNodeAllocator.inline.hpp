/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_GC_SHARED_BUFFERNODEALLOCATOR_INLINE_HPP
#define SHARE_GC_SHARED_BUFFERNODEALLOCATOR_INLINE_HPP

#include "gc/shared/bufferNodeAllocator.hpp"

#include "logging/log.hpp"
#include "runtime/atomic.hpp"
#include "utilities/globalCounter.inline.hpp"

template <class BufferNode, bool E>
BufferNodeAllocatorBase<BufferNode, E>::BufferNodeAllocatorBase(const char* name, size_t buffer_size) :
  _buffer_size(buffer_size),
  _pending_list(),
  _free_list(),
  _pending_count(0),
  _free_count(0),
  _transfer_lock(false)
{
  strncpy(_name, name, sizeof(_name) - 1);
  _name[sizeof(_name) - 1] = '\0';
}

template <class BufferNode>
BufferNodeAllocatorBase<BufferNode, false>::BufferNodeAllocatorBase(size_t buffer_size) :
  _buffer_size(buffer_size),
  _pending_list(),
  _free_list(),
  _pending_count(0),
  _free_count(0),
  _transfer_lock(false)
{}

template <class BufferNode, class Arena, bool E>
void BufferNodeAllocator<BufferNode, Arena, E>::delete_list(BufferNode* list) {
  while (list != NULL) {
    BufferNode* next = list->next();
    DEBUG_ONLY(list->set_next(NULL);)
    _arena.deallocate(list);
    list = next;
  }
}

template <class BufferNode, class Arena, bool E>
BufferNodeAllocator<BufferNode, Arena, E>::~BufferNodeAllocator() {
  delete_list(_free_list.pop_all());
  delete_list(_pending_list.pop_all());
  _arena.drop_all();
}

template <class BufferNode, class Arena, bool E>
void BufferNodeAllocator<BufferNode, Arena, E>::reset() {
  _free_list.pop_all();
  _pending_list.pop_all();
  _pending_count = 0;
  _free_count = 0;
  _arena.drop_all();
}


template <class BufferNode, class Arena, bool E>
size_t BufferNodeAllocator<BufferNode, Arena, E>::free_count() const {
  return Atomic::load(&_free_count);
}

template <class BufferNode, class Arena, bool E>
size_t BufferNodeAllocator<BufferNode, Arena, E>::pending_count() const {
  return Atomic::load(&_pending_count);
}

template <class BufferNode, class Arena, bool E>
BufferNode* BufferNodeAllocator<BufferNode, Arena, E>::allocate() {
  BufferNode* node = NULL;
  if (free_count() > 0) {
    // Protect against ABA; see release().
    GlobalCounter::CriticalSection cs(Thread::current());
    node = _free_list.pop();
  }
  if (node == NULL) {
    // node = _arena.allocate(_buffer_size);
    node = _arena.allocate();
  } else {
    // Decrement count after getting buffer from free list.  This, along
    // with incrementing count before adding to free list, ensures count
    // never underflows.
    size_t count = Atomic::sub(&_free_count, 1u);
    assert((count + 1) != 0, "_free_count underflow");
  }
  return node;
}

// To solve the ABA problem for lock-free stack pop, allocate does the
// pop inside a critical section, and release synchronizes on the
// critical sections before adding to the _free_list.  But we don't
// want to make every release have to do a synchronize.  Instead, we
// initially place released nodes on the _pending_list, and transfer
// them to the _free_list in batches.  Only one transfer at a time is
// permitted, with a lock bit to control access to that phase.  A
// transfer takes all the nodes from the _pending_list, synchronizes on
// the _free_list pops, and then adds the former pending nodes to the
// _free_list.  While that's happening, other threads might be adding
// other nodes to the _pending_list, to be dealt with by some later
// transfer.
template <class BufferNode, class Arena, bool E>
void BufferNodeAllocator<BufferNode, Arena, E>::release(BufferNode* node) {
  assert(node != NULL, "precondition");
  assert(node->next() == NULL, "precondition");

  // Desired minimum transfer batch size.  There is relatively little
  // importance to the specific number.  It shouldn't be too big, else
  // we're wasting space when the release rate is low.  If the release
  // rate is high, we might accumulate more than this before being
  // able to start a new transfer, but that's okay.  Also note that
  // the allocation rate and the release rate are going to be fairly
  // similar, due to how the buffers are used.
  const size_t trigger_transfer = 10;

  // Add to pending list. Update count first so no underflow in transfer.
  size_t pending_count = Atomic::add(&_pending_count, 1u);
  _pending_list.push(*node);
  if (pending_count > trigger_transfer) {
    try_transfer_pending();
  }
}

// Try to transfer nodes from _pending_list to _free_list, with a
// synchronization delay for any in-progress pops from the _free_list,
// to solve ABA there.  Return true if performed a (possibly empty)
// transfer, false if blocked from doing so by some other thread's
// in-progress transfer.
template <class BufferNode, class Arena, bool E>
bool BufferNodeAllocator<BufferNode, Arena, E>::try_transfer_pending() {
  // Attempt to claim the lock.
  if (Atomic::load(&_transfer_lock) || // Skip CAS if likely to fail.
      Atomic::cmpxchg(&_transfer_lock, false, true)) {
    return false;
  }
  // Have the lock; perform the transfer.

  // Claim all the pending nodes.
  BufferNode* first = _pending_list.pop_all();
  if (first != NULL) {
    // Prepare to add the claimed nodes, and update _pending_count.
    BufferNode* last = first;
    size_t count = 1;
    for (BufferNode* next = first->next(); next != NULL; next = next->next()) {
      last = next;
      ++count;
    }
    Atomic::sub(&_pending_count, count);

    // Wait for any in-progress pops, to avoid ABA for them.
    GlobalCounter::write_synchronize();

    // Add synchronized nodes to _free_list.
    // Update count first so no underflow in allocate().
    Atomic::add(&_free_count, count);
    _free_list.prepend(*first, *last);
    log_trace(gc, ptrqueue, freelist)
             ("Transferred %s pending to free: " SIZE_FORMAT, this->name(), count);
  }
  Atomic::release_store(&_transfer_lock, false);
  return true;
}

template <class BufferNode, class Arena, bool E>
size_t BufferNodeAllocator<BufferNode, Arena, E>::reduce_free_list(size_t remove_goal) {
  try_transfer_pending();
  size_t removed = 0;
  for ( ; removed < remove_goal; ++removed) {
    BufferNode* node = _free_list.pop();
    if (node == NULL) break;
    _arena->deallocate(node);
  }
  size_t new_count = Atomic::sub(&_free_count, removed);
  log_debug(gc, ptrqueue, freelist)
           ("Reduced %s free list by " SIZE_FORMAT " to " SIZE_FORMAT,
            this->name(), removed, new_count);
  return removed;
}

#endif // SHARE_GC_SHARED_BUFFERNODEALLOCATOR_INLINE_HPP
