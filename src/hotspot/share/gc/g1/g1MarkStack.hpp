/*
 * Copyright (c) 2001, 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1MARKSTACK_HPP
#define SHARE_GC_G1_G1MARKSTACK_HPP
#include "gc/g1/g1HeapRegion.hpp"
#include "gc/shared/taskqueue.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/lockFreeStack.hpp"


// This is a container class for either an oop or a continuation address for
// mark stack entries. Both are pushed onto the mark stack.
class G1TaskQueueEntry {
private:
  void* _holder;

  static const uintptr_t ArraySliceBit = 1;

  G1TaskQueueEntry(oop obj) : _holder(obj) {
    assert(_holder != nullptr, "Not allowed to set null task queue element");
  }
  G1TaskQueueEntry(HeapWord* addr) : _holder((void*)((uintptr_t)addr | ArraySliceBit)) { }
public:

  G1TaskQueueEntry() : _holder(nullptr) { }
  // Trivially copyable, for use in GenericTaskQueue.

  static G1TaskQueueEntry from_slice(HeapWord* what) { return G1TaskQueueEntry(what); }
  static G1TaskQueueEntry from_oop(oop obj) { return G1TaskQueueEntry(obj); }

  oop obj() const {
    assert(!is_array_slice(), "Trying to read array slice " PTR_FORMAT " as oop", p2i(_holder));
    return cast_to_oop(_holder);
  }

  uintptr_t addr() const {
    return (uintptr_t)_holder & ~1;
  }

  HeapWord* slice() const {
    assert(is_array_slice(), "Trying to read oop " PTR_FORMAT " as array slice", p2i(_holder));
    return (HeapWord*)((uintptr_t)_holder & ~ArraySliceBit);
  }

  bool is_oop() const { return !is_array_slice(); }
  bool is_array_slice() const { return ((uintptr_t)_holder & ArraySliceBit) != 0; }
  bool is_null() const { return _holder == nullptr; }
};

class G1MarkStack {
  size_t _top;
  const size_t _capacity;
  G1MarkStack* _next;
  // VLA implementation.
  G1TaskQueueEntry _entries[1];

  static size_t header_size_in_bytes();

  G1TaskQueueEntry* entries();

  G1MarkStack(size_t capacity);
public:
  static G1MarkStack* create(bool first_stack);
  static void destroy(G1MarkStack* stack);

  static size_t size_in_bytes(size_t num_entries) {
    return header_size_in_bytes() + sizeof(G1TaskQueueEntry) * num_entries;
  }

  bool is_empty() const;
  bool is_full() const;

  inline G1MarkStack* next() const;
  G1MarkStack* volatile* next_addr() { return &_next; }
  inline void set_next(G1MarkStack* next);

  void push(G1TaskQueueEntry entry);
  G1TaskQueueEntry pop();
};

class G1MarkStackStripe {
  static G1MarkStack* volatile* next_ptr(G1MarkStack& stack) {
    return stack.next_addr();
  }
  using StackofStacks = LockFreeStack<G1MarkStack, &next_ptr>;
  StackofStacks _stacks;
  ssize_t volatile      _length;
public:
  G1MarkStackStripe() : _stacks(), _length(0) {}
  ~G1MarkStackStripe();

  bool is_empty() const {
    return _stacks.empty();
  };

  size_t length() const {
    const ssize_t result = Atomic::load(&_length);
    return (result < 0) ? 0 : (size_t)result;
  }

  void publish_stack(G1MarkStack*);
  G1MarkStack* steal_stack();

  void delete_all();
};

class G1MarkStackStripeSet : public TaskQueueSetSuper {

  // Mark stripe size
  const size_t MarkStripeShift = G1HeapRegion::LogOfHRGrainBytes;

  // Max number of mark stripes
  static constexpr size_t MarkStripesMax = 16; // Must be a power of two
  
private:
  size_t _nstripes_mask;
  G1MarkStackStripe _stripes[MarkStripesMax];

public:
  G1MarkStackStripeSet();

  void set_nstripes(size_t nstripes);
  bool try_set_nstripes(size_t old_nstripes, size_t new_nstripes);
  size_t nstripes() const;

  bool is_empty() const;
  bool is_crowded() const;

  size_t stripe_id(const G1MarkStackStripe* stripe) const;
  inline G1MarkStackStripe* stripe_at(size_t index);
  inline G1MarkStackStripe* stripe_next(G1MarkStackStripe* stripe);
  G1MarkStackStripe* stripe_for_worker(uint nworkers, uint worker_id);
  inline G1MarkStackStripe* stripe_for_addr(uintptr_t addr);

  void delete_all();
  // 
  DEBUG_ONLY(virtual void assert_empty() const;)

  virtual uint tasks() const;
};

class G1MarkThreadLocalStacks {
  static constexpr size_t G1MarkStripesMax = 16;
  G1MarkStack* _stacks[G1MarkStripesMax];
  G1MarkStackStripeSet* _stripes;

  G1MarkStack** stack_addr(G1MarkStackStripe* stripe) {
    return &_stacks[_stripes->stripe_id(stripe)];
  }

public:
  G1MarkThreadLocalStacks(G1MarkStackStripeSet* stripes);

  bool is_empty() const;

  inline void install(G1MarkStackStripe* stripe,
                      G1MarkStack* stack);

  inline G1MarkStack* steal(G1MarkStackStripe* stripe);

  inline void push(G1MarkStackStripe* stripe,
                   G1TaskQueueEntry entry);

  inline bool pop(G1MarkStackStripe* stripe,
                  G1TaskQueueEntry* entry);

  bool flush();
};

#endif // SHARE_GC_G1_G1MARKSTACK_HPP