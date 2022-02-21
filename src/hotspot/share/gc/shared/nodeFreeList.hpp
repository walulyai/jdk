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

// Allocation is based on a lock-free free list of nodes, linked through
// Node::_next (see BufferNode::Stack).  To solve the ABA problem,
// popping a node from the free list is performed within a GlobalCounter
// critical section, and pushing nodes onto the free list is done after
// a GlobalCounter synchronization associated with the nodes to be pushed.
// This is documented behavior so that other parts of the node life-cycle
// can depend on and make use of it too.
class NodeFreeList {
  struct FreeNode {
    FreeNode * _next;

    FreeNode() : _next (nullptr) { }

    FreeNode* next() { return _next; }

    FreeNode** next_addr() { return &_next; }

    void set_next(FreeNode* next) { _next = next; }
  };

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

  static FreeNode* volatile* next_ptr(FreeNode& node) { return node.next_addr(); }
  typedef LockFreeStack<FreeNode, &next_ptr> Stack;
  typedef void (*DELETE_FUNC)(void *);

  volatile size_t _free_count;
  char _name[DEFAULT_CACHE_LINE_SIZE - sizeof(size_t)];  // Use name as padding.
#define DECLARE_PADDED_MEMBER(Id, Type, Name) \
  Type Name; DEFINE_PAD_MINUS_SIZE(Id, DEFAULT_CACHE_LINE_SIZE, sizeof(Type))
  DECLARE_PADDED_MEMBER(1, Stack, _free_list);
  DECLARE_PADDED_MEMBER(2, volatile uint, _active_pending_list);
  DECLARE_PADDED_MEMBER(3, volatile bool, _transfer_lock);
#undef DECLARE_PADDED_MEMBER

  PendingList _pending_lists[2];

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
  bool flush();

  size_t mem_size() const {
    return sizeof(*this);
  }

  typedef void (*DeleteFn)(void *);

  void delete_list(DeleteFn delete_fn);

  // Deallocate some of the available buffers.  remove_goal is the target
  // number to remove.  Returns the number actually deallocated, which may
  // be less than the goal if there were fewer available.
  size_t reduce_free_list(size_t remove_goal, DeleteFn delete_fn);
};

#endif // SHARE_GC_SHARED_NODEFREELIST_HPP
