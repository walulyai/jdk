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

#include <type_traits>

#include "gc/shared/bufferNodeList.inline.hpp"
#include "memory/padded.hpp"
#include "utilities/globalDefinitions.hpp"

template <class Node, bool padding = true>
struct NodeAllocatorBase {
  // If we don't expect many instances, padding seems like a good tradeoff here.
#define DECLARE_PADDED_MEMBER(Id, Type, Name) \
  Type Name; DEFINE_PAD_MINUS_SIZE(Id, DEFAULT_CACHE_LINE_SIZE, sizeof(Type))
  const size_t _buffer_size = 0;
  char _name[DEFAULT_CACHE_LINE_SIZE - sizeof(size_t)]; // Use name as padding.
  DECLARE_PADDED_MEMBER(1, volatile uint, _active_pending_list);
  DECLARE_PADDED_MEMBER(2, typename Node::Stack, _free_list);
  DECLARE_PADDED_MEMBER(3, volatile size_t, _free_count);
  DECLARE_PADDED_MEMBER(4, volatile bool, _transfer_lock);

  struct PendingListBase {
    Node* _tail;
    DECLARE_PADDED_MEMBER(1, Node* volatile, _head);
    DECLARE_PADDED_MEMBER(2, volatile size_t, _count);
    PendingListBase() : _tail(nullptr), _head(nullptr), _count(0) {}
  };

  NodeAllocatorBase(const char* name, size_t buffer_size) :
    _buffer_size(buffer_size),
    _active_pending_list(0),
    _free_list(),
    _free_count(0),
    _transfer_lock(false)
  {
    strncpy(_name, name, sizeof(_name) - 1);
    _name[sizeof(_name) - 1] = '\0';
  }
#undef DECLARE_PADDED_MEMBER

  const char* name() const { return _name; }

};

template <class Node>
struct NodeAllocatorBase<Node, false> {
  const size_t _buffer_size = 0;
  volatile uint _active_pending_list;
  typename Node::Stack _free_list;
  volatile size_t _free_count;
  volatile bool _transfer_lock;
  struct PendingListBase {
    Node* _tail;
    Node* volatile _head;
    volatile size_t _count;

    PendingListBase() : _tail(nullptr), _head(nullptr), _count(0) {}
  };

  NodeAllocatorBase(const char* name, size_t buffer_size) :
    _buffer_size(buffer_size),
    _active_pending_list(0),
    _free_list(),
    _free_count(0),
    _transfer_lock(false)
  { }

  const char* name() const { return ""; }

};

// Allocation is based on a lock-free free list of nodes, linked through
// Node::_next (see BufferNode::Stack).  To solve the ABA problem,
// popping a node from the free list is performed within a GlobalCounter
// critical section, and pushing nodes onto the free list is done after
// a GlobalCounter synchronization associated with the nodes to be pushed.
// This is documented behavior so that other parts of the node life-cycle
// can depend on and make use of it too.
template <class Node, class Arena, bool padding = true>
class NodeAllocator: NodeAllocatorBase<Node, padding>  {
  friend class BufferNodeAllocatorTest;
  using  NodeAllocatorBase<Node, padding>::_buffer_size;
  using  NodeAllocatorBase<Node, padding>::_active_pending_list;
  using  NodeAllocatorBase<Node, padding>::_free_list;
  using  NodeAllocatorBase<Node, padding>:: _free_count;
  using  NodeAllocatorBase<Node, padding>::_transfer_lock;
  class PendingList: NodeAllocatorBase<Node, padding>::PendingListBase {
    using NodeAllocatorBase<Node, padding>::PendingListBase::_tail;
    using NodeAllocatorBase<Node, padding>::PendingListBase::_head;
    using NodeAllocatorBase<Node, padding>::PendingListBase::_count;

    NONCOPYABLE(PendingList);

  public:
    PendingList();
    ~PendingList();

    // Add node to the list.  Returns the number of nodes in the list.
    // Thread-safe against concurrent add operations.
    size_t add(Node* node);

    size_t count() const;

    // Return the nodes in the list, leaving the list empty.
    // Not thread-safe.
    BufferNodeList<Node> take_all();
  };

  PendingList _pending_lists[2];
  Arena _arena;

  void delete_list(Node* list);
  bool try_transfer_pending();

  NONCOPYABLE(NodeAllocator);

public:
  template <typename... Args>
  NodeAllocator(const char* name, size_t buffer_size, Args&&... args);

  const char* name() const { return NodeAllocatorBase<Node, padding>::name(); }

  ~NodeAllocator();

  size_t buffer_size() const { return _buffer_size; }
  size_t free_count() const;
  size_t pending_count() const;

  Node* allocate();
  void release(Node* node);
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
