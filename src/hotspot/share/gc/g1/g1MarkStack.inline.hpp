/*
 * Copyright (c) 2021, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1MARKSTACK_INLINE_HPP
#define SHARE_GC_G1_G1MARKSTACK_INLINE_HPP

#include "gc/g1/g1MarkStack.hpp"
#include "logging/log.hpp"

inline size_t G1MarkStack::header_size_in_bytes() {
  return offset_of(G1MarkStack, _entries);
}

inline G1TaskQueueEntry* G1MarkStack::entries() {
  void* ptr = reinterpret_cast<char*>(this) + header_size_in_bytes();
  return reinterpret_cast<G1TaskQueueEntry*>(ptr);
}

inline bool G1MarkStack::is_empty() const {
  return _top == 0;
}

inline bool G1MarkStack::is_full() const {
  return _top == _capacity;
}

inline void G1MarkStack::push(G1TaskQueueEntry entry) {
  assert(!is_full(), "can't push to full stack");
  // log_info(gc)("Push %zu", (entry.addr()));
  entries()[_top++] = entry;
}

inline G1TaskQueueEntry G1MarkStack::pop() {
  assert(!is_empty(), "can't pop from empty stack");
  // log_info(gc)("Pop %zu", (entries()[_top - 1].addr()));
  return entries()[--_top];
}

G1MarkStack* G1MarkStack::next() const {
  return _next;
}

void G1MarkStack::set_next(G1MarkStack* next) {
  _next = next;
}

inline size_t G1MarkStackStripeSet::stripe_id(const G1MarkStackStripe* stripe) const {
  const size_t index = ((uintptr_t)stripe - (uintptr_t)_stripes) / sizeof(G1MarkStackStripe);
  assert(index < MarkStripesMax, "Invalid index");
  return index;
}

inline G1MarkStackStripe* G1MarkStackStripeSet::stripe_at(size_t index) {
  assert(index < MarkStripesMax, "Invalid index");
  return &_stripes[index];
}

inline G1MarkStackStripe* G1MarkStackStripeSet::stripe_next(G1MarkStackStripe* stripe) {
  const size_t index = (stripe_id(stripe) + 1) & (MarkStripesMax - 1);
  assert(index < MarkStripesMax, "Invalid index");
  return &_stripes[index];
}

inline G1MarkStackStripe* G1MarkStackStripeSet::stripe_for_addr(uintptr_t addr) {
  // TODO: use addr_to_region
  const size_t index = (addr >> G1HeapRegion::LogOfHRGrainBytes) & Atomic::load(&_nstripes_mask);
  assert(index < MarkStripesMax, "Invalid index");
  return &_stripes[index];
}

inline void G1MarkThreadLocalStacks::install(G1MarkStackStripe* stripe,
                                             G1MarkStack* stack) {
  G1MarkStack** const stackp = stack_addr(stripe);
  assert(*stackp == nullptr, "Should be empty");
  *stackp = stack;
}

inline G1MarkStack* G1MarkThreadLocalStacks::steal(G1MarkStackStripe* stripe) {
  G1MarkStack** const stackp = stack_addr(stripe);
  G1MarkStack* const stack = *stackp;
  if (stack != nullptr) {
    *stackp = nullptr;
  }

  return stack;
}

inline void G1MarkStackStripe::publish_stack(G1MarkStack* stack) {
  assert(!stack->is_empty(), "we never publish empty stacks");
  assert(stack->next() == nullptr, "stack already part of a list of stacks");

  // TODO: Between reading the head and the linearizing CAS that pushes
  // the node onto the list, there could be an ABA problem. Except,
  // on the pushing side, that is benign. The node is never
  // dereferenced while pushing and if we were to detect the ABA
  // situation and run this loop one more time, we would end up
  // having the same side effects: set the next pointer to the same
  // head again, and CAS the head link.

  _stacks.push(*stack);
  // Bookkeeping
  Atomic::inc(&_length, memory_order_relaxed);
}

inline void G1MarkThreadLocalStacks::push(G1MarkStackStripe* stripe,
                                          G1TaskQueueEntry entry) {
  G1MarkStack** const stackp = stack_addr(stripe);
  G1MarkStack* const prev_stack = *stackp;

  if (prev_stack != nullptr) {
    if (!prev_stack->is_full()) {
      // There's a stack and it isn't full: just push
      prev_stack->push(entry);
      return;
    }

    // Publish full stacks
    stripe->publish_stack(prev_stack);
    *stackp = nullptr;
  }

  // If no stack was available, allocate one and push to it
  const bool first_stack = prev_stack == nullptr;
  // G1MarkStack* const new_stack = G1MarkStack::create(first_stack);
  G1MarkStack* const new_stack = _allocator->allocate();
  *stackp = new_stack;

  new_stack->push(entry);
}

inline bool G1MarkThreadLocalStacks::pop(G1MarkStackStripe* stripe,
                                         G1TaskQueueEntry* entry) {
  G1MarkStack** const stackp = stack_addr(stripe);
  G1MarkStack* stack = *stackp;

  // First make sure there is a stack to pop from
  if (stack == nullptr) {
    // If we have no stack, try to steal one
    stack = stripe->steal_stack();
    *stackp = stack;

    if (stack == nullptr) {
      // Out of stacks to pop from
      return false;
    }
  }

  *entry = stack->pop();

  if (stack->is_empty()) {
    // Eagerly free empty stacks while on a worker thread
    //G1MarkStack::destroy(stack);
    _allocator->release(stack);
    *stackp = nullptr;
  }

  return true;
}

#endif // SHARE_GC_G1_G1MARKSTACK_INLINE_HPP