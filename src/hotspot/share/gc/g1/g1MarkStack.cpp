/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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
 */

 #include "gc/g1/g1MarkStack.inline.hpp"
 #include "logging/log.hpp"
 #include "memory/allocation.hpp"
 #include "runtime/vmOperations.hpp"
 #include "utilities/globalCounter.inline.hpp"

 G1MarkStack::G1MarkStack(size_t capacity)
  : _top(0),
    _capacity(capacity),
    _next(nullptr) {}

 G1MarkStack* G1MarkStack::create(bool first_stack) {
  // When allocating the first stack on a stripe, we try to use a
  // smaller mark stack to promote sharing of stacks with other
  // threads instead. Once more than one stack is needed, we revert
  // to a larger stack size instead, which reduces synchronization
  // overhead of churning around stacks on a stripe.
  // const size_t capacity = first_stack ? 128 : 512;
  const size_t capacity = 1024;

  void* const memory = AllocateHeap(size_in_bytes(capacity), mtGC);
  return ::new (memory) G1MarkStack(capacity);
}

void G1MarkStack::destroy(G1MarkStack* stack) {
  assert(stack != nullptr, "pre-condition");
  assert(stack->is_empty(), "stack should be empty");
  // Wait for concurrent readers of the segment to exit before freeing; but only if the VM
  // isn't exiting.
  if (!VM_Exit::vm_exited()) {
    GlobalCounter::write_synchronize();
  }
  stack->~G1MarkStack();
  FreeHeap(stack);
}

G1MarkStack::AllocatorConfig::AllocatorConfig(size_t size)
  : _capacity(size)
{
  assert(size >= 1, "Invalid capacity capacity %zu", size);
}

void* G1MarkStack::AllocatorConfig::allocate() {
  size_t byte_size = size_in_bytes(capacity());
  return NEW_C_HEAP_ARRAY(char, byte_size, mtGC);
}

void G1MarkStack::AllocatorConfig::deallocate(void* node) {
  assert(node != nullptr, "precondition");
  FREE_C_HEAP_ARRAY(char, node);
}

G1MarkStack::Allocator::Allocator(const char* name, size_t capacity) :
  _config(capacity),
  _free_list(name, &_config)
{}

size_t G1MarkStack::Allocator::free_count() const {
  return _free_list.free_count();
}

G1MarkStack* G1MarkStack::Allocator::allocate() {
  return ::new (_free_list.allocate()) G1MarkStack(capacity());
}

void G1MarkStack::Allocator::release(G1MarkStack* stack) {
  assert(stack != nullptr, "precondition");
  assert(stack->next() == nullptr, "precondition");
  assert(stack->capacity() == capacity(),
         "Wrong size %zu, expected %zu", stack->capacity(), capacity());
  stack->~G1MarkStack();
  _free_list.release(stack);
}

G1MarkStack* G1MarkStackStripe::steal_stack() {
  GlobalCounter::CriticalSection cs(Thread::current());

  G1MarkStack* stack = _stacks.pop();

  if (stack != nullptr) {
    // Perform bookkeeping of the population count.
    stack->set_next(nullptr);
    Atomic::dec(&_length, memory_order_relaxed);
  }
  return stack;
}

void G1MarkStackStripe::delete_all() {
  G1MarkStack* stack = _stacks.pop_all();

  while (stack != nullptr) {
    G1MarkStack* next = stack->next();
    stack->~G1MarkStack();
    FreeHeap(stack);
    stack = next;
  }

}

G1MarkStackStripeSet::G1MarkStackStripeSet()
  : _nstripes_mask(0),
    _stripes() {}

void G1MarkStackStripeSet::set_nstripes(size_t nstripes) {
  assert(is_power_of_2(nstripes), "Must be a power of two");
  assert(is_power_of_2(MarkStripesMax), "Must be a power of two");
  assert(nstripes >= 1, "Invalid number of stripes");
  assert(nstripes <= MarkStripesMax, "Invalid number of stripes");

  // 

  const size_t new_nstripes_mask = nstripes - 1;
  _nstripes_mask = new_nstripes_mask;

  log_debug(gc, marking)("Using %zu mark stripes", nstripes);
}

bool G1MarkStackStripeSet::try_set_nstripes(size_t old_nstripes, size_t new_nstripes) {
  assert(is_power_of_2(new_nstripes), "Must be a power of two");
  assert(is_power_of_2(MarkStripesMax), "Must be a power of two");
  assert(new_nstripes >= 1, "Invalid number of stripes");
  assert(new_nstripes <= MarkStripesMax, "Invalid number of stripes");

  const size_t old_nstripes_mask = old_nstripes - 1;
  const size_t new_nstripes_mask = new_nstripes - 1;

  // Mutators may read these values concurrently. It doesn't matter
  // if they see the old or new values.
  if (Atomic::cmpxchg(&_nstripes_mask, old_nstripes_mask, new_nstripes_mask) == old_nstripes_mask) {
    log_debug(gc, marking)("Using %zu mark stripes", new_nstripes);
    return true;
  }

  return false;
}

size_t G1MarkStackStripeSet::nstripes() const {
  return Atomic::load(&_nstripes_mask) + 1;
}

bool G1MarkStackStripeSet::is_empty() const {
  for (size_t i = 0; i < MarkStripesMax; i++) {
    if (!_stripes[i].is_empty()) {
      return false;
    }
  }
  return true;
}

#ifdef ASSERT
void G1MarkStackStripeSet::assert_empty() const {
  assert(is_empty(), "not empty");
}
#endif // ASSERT

void G1MarkStackStripeSet::delete_all() {
  for (size_t i = 0; i < MarkStripesMax; i++) {
    _stripes[i].delete_all();
  }
}

uint G1MarkStackStripeSet::tasks() const {
  size_t population = 0;

  for (size_t i = 0; i < MarkStripesMax; i++) {
    population += _stripes[i].length();
  }

  return (uint)population;
}

bool G1MarkStackStripeSet::is_crowded() const {
  size_t population = 0;
  const size_t crowded_threshold = nstripes() << 4;

  for (size_t i = 0; i < MarkStripesMax; i++) {
    population += _stripes[i].length();
    if (population > crowded_threshold) {
      return true;
    }
  }

  return false;
}

G1MarkStackStripe* G1MarkStackStripeSet::stripe_for_worker(uint nworkers, uint worker_id) {
  const size_t mask = Atomic::load(&_nstripes_mask);
  const size_t nstripes = mask + 1;

  const size_t spillover_limit = (nworkers / nstripes) * nstripes;
  size_t index;

  if (worker_id < spillover_limit) {
    // Not a spillover worker, use natural stripe
    index = worker_id & mask;
  } else {
    // Distribute spillover workers evenly across stripes
    const size_t spillover_nworkers = nworkers - spillover_limit;
    const size_t spillover_worker_id = worker_id - spillover_limit;
    const double spillover_chunk = (double)nstripes / (double)spillover_nworkers;
    index = (size_t)(spillover_worker_id * spillover_chunk);
  }

  assert(index < nstripes, "Invalid index");
  return &_stripes[index];
}


G1MarkThreadLocalStacks::G1MarkThreadLocalStacks(G1MarkStackStripeSet* stripes, G1MarkStack::Allocator* allocator)
: _stripes(stripes),_allocator(allocator) {
  for (size_t i = 0; i < G1MarkStripesMax; i++) {
    _stacks[i] = nullptr;
  }
}

bool G1MarkThreadLocalStacks::is_empty() const {
  for (size_t i = 0; i < G1MarkStripesMax; i++) {
    G1MarkStack* const stack = _stacks[i];
    if (stack != nullptr) {
      return false;
    }
  }

  return true;
}

bool G1MarkThreadLocalStacks::flush() {
  bool flushed = false;

  // Flush all stacks
  for (size_t i = 0; i < G1MarkStripesMax; i++) {
    G1MarkStack** const stackp = &_stacks[i];
    G1MarkStack* const stack = *stackp;
    if (stack == nullptr) {
      continue;
    }

    // Free/Publish and uninstall stack
    G1MarkStackStripe* const stripe = _stripes->stripe_at(i);
    stripe->publish_stack(stack);
    flushed = true;
    *stackp = nullptr;
  }

  return flushed;
}