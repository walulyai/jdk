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

#ifndef SHARE_GC_SHARED_NODEFREELIST_HPP
#define SHARE_GC_SHARED_NODEFREELIST_HPP

#include <type_traits>

#include "memory/padded.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/lockFreeStack.hpp"

  struct FreeNode {
    static FreeNode* volatile* next_ptr(FreeNode& node) { return node.next_addr(); }
    typedef LockFreeStack<FreeNode, &next_ptr> Stack;
    FreeNode * _next;

    FreeNode() : _next (nullptr) { }

    FreeNode* next() { return _next; }

    FreeNode** next_addr() { return &_next; }

    void set_next(FreeNode* next) { _next = next; }
  };

template <bool padding>
struct NodeFreeListBase;

template <>
struct NodeFreeListBase<true> {
  // If we don't expect many instances, padding seems like a good tradeoff here.
#define DECLARE_PADDED_MEMBER(Id, Type, Name) \
  Type Name; DEFINE_PAD_MINUS_SIZE(Id, DEFAULT_CACHE_LINE_SIZE, sizeof(Type))
  DECLARE_PADDED_MEMBER(1, volatile size_t, _free_count);
  DECLARE_PADDED_MEMBER(2, typename FreeNode::Stack, _free_list);
  DECLARE_PADDED_MEMBER(3, volatile uint, _active_pending_list);
  DECLARE_PADDED_MEMBER(4, volatile bool, _transfer_lock);

  NodeFreeListBase() :
    _free_count(0),
    _free_list(),
    _active_pending_list(0),
    _transfer_lock(false)
  { }
#undef DECLARE_PADDED_MEMBER
};

template <>
struct NodeFreeListBase<false> {
  volatile size_t _free_count;
  typename FreeNode::Stack _free_list;
  volatile uint _active_pending_list;
  volatile bool _transfer_lock;

  NodeFreeListBase() :
    _free_count(0),
    _free_list(),
    _active_pending_list(0),
    _transfer_lock(false)
  { }
};

// Allocation is based on a lock-free free list of nodes, linked through
// Node::_next (see BufferNode::Stack).  To solve the ABA problem,
// popping a node from the free list is performed within a GlobalCounter
// critical section, and pushing nodes onto the free list is done after
// a GlobalCounter synchronization associated with the nodes to be pushed.
// This is documented behavior so that other parts of the node life-cycle
// can depend on and make use of it too.
template <bool padding = true>
class NodeFreeList: NodeFreeListBase<padding>  {
  friend class BufferNodeAllocatorTest;

  using  NodeFreeListBase<padding>:: _free_count;
  using  NodeFreeListBase<padding>::_free_list;
  using  NodeFreeListBase<padding>::_active_pending_list;
  using  NodeFreeListBase<padding>::_transfer_lock;

  struct NodeList {
    FreeNode* _head;            // First node in list or NULL if empty.
    FreeNode* _tail;            // Last node in list or NULL if empty.
    size_t _entry_count; // Sum of entries in nodes in list.

    NodeList() :
      _head(NULL), _tail(NULL), _entry_count(0) {}

    NodeList(FreeNode* head, FreeNode* tail, size_t entry_count) :
      _head(head), _tail(tail), _entry_count(entry_count)
    {
      assert((_head == NULL) == (_tail == NULL), "invariant");
      assert((_head == NULL) == (_entry_count == 0), "invariant");
    }
  };
  class PendingList {
    FreeNode* _tail;
    FreeNode* volatile _head;
    volatile size_t _count;

    NONCOPYABLE(PendingList);

  public:
    PendingList();
    ~PendingList();

    // Add node to the list.  Returns the number of nodes in the list.
    // Thread-safe against concurrent add operations.
    size_t add(FreeNode* node);

    size_t count() const;

    // Return the nodes in the list, leaving the list empty.
    // Not thread-safe.
    NodeList take_all();
  };

  char _name[DEFAULT_CACHE_LINE_SIZE];

  PendingList _pending_lists[2];

  template<class DELETE_FUNC>
  void delete_list(FreeNode* list, DELETE_FUNC& delete_fn);

  bool try_transfer_pending();

  NONCOPYABLE(NodeFreeList);

public:
  NodeFreeList(const char* name);

  const char* name() const { return _name; }

  ~NodeFreeList();

  size_t free_count() const;
  size_t pending_count() const;

  void* get();
  void release(void* node);
  void reset();

  size_t mem_size() const {
    return sizeof(*this);
  }

  // Deallocate some of the available buffers.  remove_goal is the target
  // number to remove.  Returns the number actually deallocated, which may
  // be less than the goal if there were fewer available.
  template<class DELETE_FUNC>
  size_t reduce_free_list(size_t remove_goal, DELETE_FUNC& delete_fn);
};

#endif // SHARE_GC_SHARED_NODEFREELIST_HPP
