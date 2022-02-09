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

#ifndef SHARE_GC_SHARED_NODEALLOCATOR_HPP
#define SHARE_GC_SHARED_NODEALLOCATOR_HPP

#include "gc/shared/bufferNodeList.inline.hpp"
#include "memory/padded.hpp"
#include "utilities/globalDefinitions.hpp"

// Allocation is based on a lock-free free list of nodes, linked through
// BufferNode::_next (see BufferNode::Stack).  To solve the ABA problem,
// popping a node from the free list is performed within a GlobalCounter
// critical section, and pushing nodes onto the free list is done after
// a GlobalCounter synchronization associated with the nodes to be pushed.
// This is documented behavior so that other parts of the node life-cycle
// can depend on and make use of it too.
template <class BufferNode, class Arena>
class NodeAllocator {
  friend class BufferNodeAllocatorTest;
  // Since we don't expect many instances, padding seems like a good tradeoff here.
#define DECLARE_PADDED_MEMBER(Id, Type, Name) \
  Type Name; DEFINE_PAD_MINUS_SIZE(Id, DEFAULT_CACHE_LINE_SIZE, sizeof(Type))

  class PendingList {
    BufferNode* _tail;
    DECLARE_PADDED_MEMBER(1, BufferNode* volatile, _head);
    DECLARE_PADDED_MEMBER(2, volatile size_t, _count);

    NONCOPYABLE(PendingList);

  public:
    PendingList();
    ~PendingList();

    // Add node to the list.  Returns the number of nodes in the list.
    // Thread-safe against concurrent add operations.
    size_t add(BufferNode* node);

    size_t count() const;

    // Return the nodes in the list, leaving the list empty.
    // Not thread-safe.
    BufferNodeList<BufferNode> take_all();
  };

  const size_t _buffer_size = 0;
  char _name[DEFAULT_CACHE_LINE_SIZE - sizeof(size_t)]; // Use name as padding.
  PendingList _pending_lists[2];
  DECLARE_PADDED_MEMBER(1, volatile uint, _active_pending_list);
  DECLARE_PADDED_MEMBER(2, typename BufferNode::Stack, _free_list);
  DECLARE_PADDED_MEMBER(3, volatile size_t, _free_count);
  DECLARE_PADDED_MEMBER(4, volatile bool, _transfer_lock);
#undef DECLARE_PADDED_MEMBER

  Arena _arena;

  void delete_list(BufferNode* list);
  bool try_transfer_pending();

  NONCOPYABLE(NodeAllocator);

public:
  template <typename... Args>
  NodeAllocator(const char* name, size_t buffer_size, Args&&... args);

  const char* name() const { return _name; }

  ~NodeAllocator();


  size_t buffer_size() const { return _buffer_size; }
  size_t free_count() const;
  size_t pending_count() const;

  BufferNode* allocate();
  void release(BufferNode* node);
  void reset();

  const Arena* arena() const { return &_arena; } // called for statistics

  size_t mem_size() const {
    return sizeof(*this) + _arena.mem_size();
  }

  size_t wasted_mem_size() {
    return _arena.wasted_mem_size(pending_count());
  }

  void print(outputStream* os) {
    _arena.print(os, (uint)pending_count());
  }

  // Deallocate some of the available buffers.  remove_goal is the target
  // number to remove.  Returns the number actually deallocated, which may
  // be less than the goal if there were fewer available.
  size_t reduce_free_list(size_t remove_goal);
};

#endif // SHARE_GC_SHARED_NODEALLOCATOR_HPP
