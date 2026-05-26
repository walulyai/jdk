/*
 * Copyright (c) 2015, 2026, Oracle and/or its affiliates. All rights reserved.
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

#include "code/dependencies.hpp"
#include "code/dependencyContext.hpp"
#include "code/nmethod.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/atomicAccess.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/os.hpp"
#include "runtime/perfData.hpp"
#include "utilities/exceptions.hpp"

PerfCounter* DependencyContext::_perf_total_buckets_allocated_count   = nullptr;
PerfCounter* DependencyContext::_perf_total_buckets_deallocated_count = nullptr;
PerfCounter* DependencyContext::_perf_total_buckets_stale_count       = nullptr;
PerfCounter* DependencyContext::_perf_total_buckets_stale_acc_count   = nullptr;
nmethodBucket* volatile DependencyContext::_purge_list                = nullptr;
volatile uint64_t DependencyContext::_cleaning_epoch                  = 0;
uint64_t  DependencyContext::_cleaning_epoch_monotonic                = 0;

void dependencyContext_init() {
  DependencyContext::init();
}

void DependencyContext::init() {
  if (UsePerfData) {
    EXCEPTION_MARK;
    _perf_total_buckets_allocated_count =
        PerfDataManager::create_counter(SUN_CI, "nmethodBucketsAllocated", PerfData::U_Events, CHECK);
    _perf_total_buckets_deallocated_count =
        PerfDataManager::create_counter(SUN_CI, "nmethodBucketsDeallocated", PerfData::U_Events, CHECK);
    _perf_total_buckets_stale_count =
        PerfDataManager::create_counter(SUN_CI, "nmethodBucketsStale", PerfData::U_Events, CHECK);
    _perf_total_buckets_stale_acc_count =
        PerfDataManager::create_counter(SUN_CI, "nmethodBucketsStaleAccumulated", PerfData::U_Events, CHECK);
  }
}

//
// Walk the list of dependent nmethods searching for nmethods which
// are dependent on the changes that were passed in and mark them for
// deoptimization.
//
void DependencyContext::mark_dependent_nmethods(DeoptimizationScope* deopt_scope, DepChange& changes) {
  for (nmethodBucket* b = dependencies_not_unloading(); b != nullptr; b = b->next_not_unloading()) {
    nmethod* nm = b->get_nmethod();
    if (nm->is_marked_for_deoptimization()) {
      deopt_scope->dependent(nm);
    } else if (nm->check_dependency_on(changes)) {
      LogTarget(Info, dependencies) lt;
      if (lt.is_enabled()) {
        ResourceMark rm;
        LogStream ls(&lt);
        ls.print_cr("Marked for deoptimization");
        changes.print_on(&ls);
        nm->print_on(&ls);
        nm->print_dependencies_on(&ls);
      }
      deopt_scope->mark(nm, !changes.is_call_site_change());
    }
  }
}

//
// Add an nmethod to the dependency context.
//
void DependencyContext::add_dependent_nmethod(nmethod* nm,
                                              const InstanceKlass* owner_klass,
                                              bool owner_is_call_site) {
  assert_lock_strong(CodeCache_lock);
  assert(nm->is_not_installed(), "Precondition: new nmethod");

  // This method tries to add never before seen nmethod, holding the CodeCache_lock
  // until all dependencies are added. The caller code can call multiple times
  // with the same nmethod, but always under the same lock hold.
  //
  // This means the buckets list is guaranteed to be in either of two states, with
  // regards to the newly added nmethod:
  //   1. The nmethod is not in the list, and can be just added to the head of the list.
  //   2. The nmethod is in the list, and it is already at the head of the list.
  //
  // This path is the only path that adds to the list. There can be concurrent removals
  // from the list, but they do not break this invariant. This invariant allows us
  // to skip list scans. The individual method checks are cheap, but walking the large
  // list of dependencies gets expensive.

  nmethodBucket* head = AtomicAccess::load(_dependency_context_addr);
  if (head != nullptr && nm == head->get_nmethod()) {
    return;
  }

#ifdef ASSERT
  for (nmethodBucket* b = head; b != nullptr; b = b->next()) {
    assert(nm != b->get_nmethod(), "Invariant: should not be in the list yet");
  }
#endif

  nmethodBucket* new_head = new nmethodBucket(nm, _dependency_context_addr, owner_klass, owner_is_call_site);
  for (;;) {
    new_head->set_next(head);
    new_head->set_previous(nullptr);
    if (AtomicAccess::cmpxchg(_dependency_context_addr, head, new_head) == head) {
      if (head != nullptr) {
        head->set_previous(new_head);
      }
      nm->add_dependency_context_bucket(new_head);
      break;
    }
    head = AtomicAccess::load(_dependency_context_addr);
  }
  if (UsePerfData) {
    _perf_total_buckets_allocated_count->inc();
  }
}

void DependencyContext::release(nmethodBucket* b) {
  if (delete_on_release()) {
    assert_locked_or_safepoint(CodeCache_lock);
    delete b;
    if (UsePerfData) {
      _perf_total_buckets_deallocated_count->inc();
    }
  } else {
    // Mark the context as having stale entries, since it is not safe to
    // expunge the list right now.
    for (;;) {
      nmethodBucket* purge_list_head = AtomicAccess::load(&_purge_list);
      b->set_purge_list_next(purge_list_head);
      if (AtomicAccess::cmpxchg(&_purge_list, purge_list_head, b) == purge_list_head) {
        break;
      }
    }
    if (UsePerfData) {
      _perf_total_buckets_stale_count->inc();
      _perf_total_buckets_stale_acc_count->inc();
    }
  }
}

bool DependencyContext::remove_dependent_nmethod(nmethod* nm, NMethodUnloadingStats* stats) {
  jlong start = 0;
  if (stats != nullptr) {
    start = os::elapsed_counter();
  }

  size_t buckets = 0;
  bool removed = false;

  // Serialize direct bucket removal. The legacy cleanup path is lock-free because
  // it may clean whole contexts in parallel; direct removal targets one nmethod
  // bucket and is expected to do much less total work.
  MutexLocker ml(CodeCache_lock, Mutex::_no_safepoint_check_flag);
  nmethodBucket* cur = dependencies();
  while (cur != nullptr) {
    nmethodBucket* next = cur->next();
    buckets++;
    if (cur->get_nmethod() == nm) {
      removed = cur->unlink_from_context();
      break;
    }
    cur = next;
  }

  if (stats != nullptr) {
    jlong elapsed = os::elapsed_counter() - start;
    stats->_dependency_context_remove_ticks += elapsed;
    stats->_dependency_context_remove_buckets += buckets;
    if (removed) {
      stats->_dependency_context_remove_removed++;
    }
    if (elapsed > stats->_dependency_context_remove_max_ticks) {
      stats->_dependency_context_remove_max_ticks = elapsed;
      stats->_dependency_context_remove_max_buckets = buckets;
    }
  }

  return removed;
}

bool nmethodBucket::unlink_from_context() {
  assert_locked_or_safepoint(CodeCache_lock);

  if (is_removed()) {
    return false;
  }

  nmethodBucket* next_bucket = next();
  nmethodBucket* previous_bucket = previous();
  if (previous_bucket == nullptr) {
    AtomicAccess::store(_dependency_context_addr, next_bucket);
  } else {
    previous_bucket->set_next(next_bucket);
  }
  if (next_bucket != nullptr) {
    next_bucket->set_previous(previous_bucket);
  }

  set_next(nullptr);
  set_previous(nullptr);
  mark_removed();
  DependencyContext::release(this);
  return true;
}

static bool dependency_context_is_unloading(nmethod* nm, NMethodUnloadingStats* stats) {
  if (stats == nullptr) {
    return nm->is_unloading();
  }

  stats->_dependency_buckets_checked++;
  jlong start = os::elapsed_counter();
  bool unloading = nm->is_unloading();
  stats->_dependency_bucket_is_unloading_ticks += os::elapsed_counter() - start;
  if (unloading) {
    stats->_dependency_buckets_unloading++;
  }
  return unloading;
}

static void record_dependency_context_bucket_unlink_start(NMethodUnloadingStats* stats,
                                                          jlong* start) {
  if (stats != nullptr) {
    stats->_dependency_bucket_unlink_attempts++;
    *start = os::elapsed_counter();
  }
}

static void record_dependency_context_bucket_unlink_end(NMethodUnloadingStats* stats,
                                                        jlong start,
                                                        bool unlinked) {
  if (stats != nullptr) {
    if (unlinked) {
      stats->_dependency_buckets_unlinked++;
    }
    stats->_dependency_bucket_unlink_ticks += os::elapsed_counter() - start;
  }
}

//
// Reclaim all unused buckets.
//
void DependencyContext::purge_dependency_contexts() {
  int removed = 0;
  for (nmethodBucket* b = _purge_list; b != nullptr;) {
    nmethodBucket* next = b->purge_list_next();
    removed++;
    delete b;
    b = next;
  }
  if (UsePerfData && removed > 0) {
    _perf_total_buckets_deallocated_count->inc(removed);
  }
  _purge_list = nullptr;
}

//
// Cleanup a dependency context by unlinking and placing all dependents corresponding
// to is_unloading nmethods on a purge list, which will be deleted later when it is safe.
void DependencyContext::clean_unloading_dependents(NMethodUnloadingStats* stats) {
  if (!claim_cleanup()) {
    // Somebody else is cleaning up this dependency context.
    if (stats != nullptr) {
      stats->_dependency_contexts_skipped++;
    }
    return;
  }
  if (stats != nullptr) {
    stats->_dependency_contexts_claimed++;
  }

  // Walk the nmethodBuckets and move dead entries on the purge list, which will
  // be deleted during ClassLoaderDataGraph::purge().
  jlong start = 0;
  size_t start_buckets_checked = 0;
  if (stats != nullptr) {
    start = os::elapsed_counter();
    start_buckets_checked = stats->_dependency_buckets_checked;
  }

  nmethodBucket* b = dependencies_not_unloading(stats);
  while (b != nullptr) {
    nmethodBucket* next = b->next_not_unloading(stats);
    b = next;
  }

  if (stats != nullptr) {
    jlong elapsed = os::elapsed_counter() - start;
    size_t buckets_checked = stats->_dependency_buckets_checked - start_buckets_checked;

    stats->_dependency_context_walk_ticks += elapsed;
    if (elapsed > stats->_dependency_context_max_ticks) {
      stats->_dependency_context_max_ticks = elapsed;
      stats->_dependency_context_max_buckets = buckets_checked;
    }
  }
}

//
// Invalidate all dependencies in the context
void DependencyContext::remove_all_dependents() {
  // Assume that the dependency is not deleted immediately but moved into the
  // purge list when calling this.
  assert(!delete_on_release(), "should not delete on release");

  nmethodBucket* first = AtomicAccess::load_acquire(_dependency_context_addr);
  if (first == nullptr) {
    return;
  }

  nmethodBucket* cur = first;
  nmethodBucket* last = cur;
  jlong count = 0;
  for (; cur != nullptr; cur = cur->next()) {
    assert(cur->get_nmethod()->is_unloading(), "must be");
    cur->mark_removed();
    last = cur;
    count++;
  }

  // Add the whole list to the purge list at once.
  nmethodBucket* old_purge_list_head = AtomicAccess::load(&_purge_list);
  for (;;) {
    last->set_purge_list_next(old_purge_list_head);
    nmethodBucket* next_purge_list_head = AtomicAccess::cmpxchg(&_purge_list, old_purge_list_head, first);
    if (old_purge_list_head == next_purge_list_head) {
      break;
    }
    old_purge_list_head = next_purge_list_head;
  }

  if (UsePerfData) {
    _perf_total_buckets_stale_count->inc(count);
    _perf_total_buckets_stale_acc_count->inc(count);
  }

  set_dependencies(nullptr);
}

#ifndef PRODUCT
bool DependencyContext::is_empty() {
  return dependencies() == nullptr;
}

void DependencyContext::print_dependent_nmethods(bool verbose) {
  int idx = 0;
  for (nmethodBucket* b = dependencies_not_unloading(); b != nullptr; b = b->next_not_unloading()) {
    nmethod* nm = b->get_nmethod();
    tty->print("[%d] { ", idx++);
    if (!verbose) {
      nm->print_on_with_msg(tty, "nmethod");
      tty->print_cr(" } ");
    } else {
      nm->print();
      nm->print_dependencies_on(tty);
      tty->print_cr("--- } ");
    }
  }
}
#endif //PRODUCT

bool DependencyContext::is_dependent_nmethod(nmethod* nm) {
  for (nmethodBucket* b = dependencies_not_unloading(); b != nullptr; b = b->next_not_unloading()) {
    if (nm == b->get_nmethod()) {
      return true;
    }
  }
  return false;
}

// We use a monotonically increasing epoch counter to track the last epoch a given
// dependency context was cleaned. GC threads claim cleanup tasks by performing
// a CAS on this value.
bool DependencyContext::claim_cleanup() {
  uint64_t cleaning_epoch = AtomicAccess::load(&_cleaning_epoch);
  uint64_t last_cleanup = AtomicAccess::load(_last_cleanup_addr);
  if (last_cleanup >= cleaning_epoch) {
    return false;
  }
  return AtomicAccess::cmpxchg(_last_cleanup_addr, last_cleanup, cleaning_epoch) == last_cleanup;
}

bool DependencyContext::delete_on_release() {
  return AtomicAccess::load(&_cleaning_epoch) == 0;
}

// Retrieve the first nmethodBucket that has a dependent that does not correspond to
// an is_unloading nmethod. Any nmethodBucket entries observed from the original head
// that is_unloading() will be unlinked and placed on the purge list.
nmethodBucket* DependencyContext::dependencies_not_unloading(NMethodUnloadingStats* stats) {
  for (;;) {
    // Need acquire because the read value could come from a concurrent insert.
    nmethodBucket* head = AtomicAccess::load_acquire(_dependency_context_addr);
    if (head == nullptr || !dependency_context_is_unloading(head->get_nmethod(), stats)) {
      return head;
    }
    nmethodBucket* head_next = head->next();
    OrderAccess::loadload();
    if (AtomicAccess::load(_dependency_context_addr) != head) {
      // Unstable load of head w.r.t. head->next
      continue;
    }
    jlong unlink_start = 0;
    record_dependency_context_bucket_unlink_start(stats, &unlink_start);
    bool unlinked = AtomicAccess::cmpxchg(_dependency_context_addr, head, head_next) == head;
    if (unlinked) {
      if (head_next != nullptr) {
        head_next->set_previous(nullptr);
      }
      head->mark_removed();
      // Release is_unloading entries if unlinking was claimed
      DependencyContext::release(head);
    }
    record_dependency_context_bucket_unlink_end(stats, unlink_start, unlinked);
  }
}

// Relaxed accessors
void DependencyContext::set_dependencies(nmethodBucket* b) {
  AtomicAccess::store(_dependency_context_addr, b);
}

nmethodBucket* DependencyContext::dependencies() {
  return AtomicAccess::load(_dependency_context_addr);
}

// After the gc_prologue, the dependency contexts may be claimed by the GC
// and releasing of nmethodBucket entries will be deferred and placed on
// a purge list to be deleted later.
void DependencyContext::cleaning_start() {
  assert(SafepointSynchronize::is_at_safepoint(), "must be");
  uint64_t epoch = ++_cleaning_epoch_monotonic;
  AtomicAccess::store(&_cleaning_epoch, epoch);
}

// The epilogue marks the end of dependency context cleanup by the GC,
// and also makes subsequent releases of nmethodBuckets cause immediate
// deletion. It is okay to delay calling of cleaning_end() to a concurrent
// phase, subsequent to the safepoint operation in which cleaning_start()
// was called. That allows dependency contexts to be cleaned concurrently.
void DependencyContext::cleaning_end() {
  uint64_t epoch = 0;
  AtomicAccess::store(&_cleaning_epoch, epoch);
}

// This function skips over nmethodBuckets in the list corresponding to
// nmethods that are is_unloading. This allows exposing a view of the
// dependents as-if they were already cleaned, despite being cleaned
// concurrently. Any entry observed that is_unloading() will be unlinked
// and placed on the purge list.
nmethodBucket* nmethodBucket::next_not_unloading(NMethodUnloadingStats* stats) {
  for (;;) {
    // Do not need acquire because the loaded entry can never be
    // concurrently inserted.
    nmethodBucket* next = AtomicAccess::load(&_next);
    if (next == nullptr || !dependency_context_is_unloading(next->get_nmethod(), stats)) {
      return next;
    }
    nmethodBucket* next_next = AtomicAccess::load(&next->_next);
    OrderAccess::loadload();
    if (AtomicAccess::load(&_next) != next) {
      // Unstable load of next w.r.t. next->next
      continue;
    }
    jlong unlink_start = 0;
    record_dependency_context_bucket_unlink_start(stats, &unlink_start);
    bool unlinked = AtomicAccess::cmpxchg(&_next, next, next_next) == next;
    if (unlinked) {
      if (next_next != nullptr) {
        next_next->set_previous(this);
      }
      next->mark_removed();
      // Release is_unloading entries if unlinking was claimed
      DependencyContext::release(next);
    }
    record_dependency_context_bucket_unlink_end(stats, unlink_start, unlinked);
  }
}

// Relaxed accessors
nmethodBucket* nmethodBucket::next() {
  return AtomicAccess::load(&_next);
}

void nmethodBucket::set_next(nmethodBucket* b) {
  AtomicAccess::store(&_next, b);
}

nmethodBucket* nmethodBucket::previous() {
  return AtomicAccess::load(&_previous);
}

void nmethodBucket::set_previous(nmethodBucket* b) {
  AtomicAccess::store(&_previous, b);
}

nmethodBucket* nmethodBucket::nmethod_next() {
  return _nmethod_next;
}

void nmethodBucket::set_nmethod_next(nmethodBucket* b) {
  _nmethod_next = b;
}

nmethodBucket* nmethodBucket::purge_list_next() {
  return AtomicAccess::load(&_purge_list_next);
}

void nmethodBucket::set_purge_list_next(nmethodBucket* b) {
  AtomicAccess::store(&_purge_list_next, b);
}
