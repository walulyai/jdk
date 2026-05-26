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

#ifndef SHARE_CODE_DEPENDENCYCONTEXT_HPP
#define SHARE_CODE_DEPENDENCYCONTEXT_HPP

#include "memory/allocation.hpp"
#include "oops/oop.hpp"
#include "runtime/handles.hpp"
#include "runtime/perfData.hpp"
#include "runtime/safepoint.hpp"

class nmethod;
class InstanceKlass;
class DeoptimizationScope;
class DepChange;
struct NMethodUnloadingStats;

//
// nmethodBucket is used to record dependent nmethods for
// deoptimization.  nmethod dependencies are actually <klass, method>
// pairs but we really only care about the klass part for purposes of
// finding nmethods which might need to be deoptimized.
//
class nmethodBucket: public CHeapObj<mtClass> {
 private:
  nmethod*       _nmethod;
  nmethodBucket* volatile* _dependency_context_addr;
  const InstanceKlass* _owner_klass;
  nmethodBucket* volatile _next;
  nmethodBucket* volatile _previous;
  nmethodBucket* _nmethod_next;
  nmethodBucket* volatile _purge_list_next;
  bool _owner_is_call_site;
  bool _removed;

 public:
  nmethodBucket(nmethod* nmethod,
                nmethodBucket* volatile* dependency_context_addr,
                const InstanceKlass* owner_klass,
                bool owner_is_call_site) :
    _nmethod(nmethod),
    _dependency_context_addr(dependency_context_addr),
    _owner_klass(owner_klass),
    _next(nullptr),
    _previous(nullptr),
    _nmethod_next(nullptr),
    _purge_list_next(nullptr),
    _owner_is_call_site(owner_is_call_site),
    _removed(false) {}

  nmethodBucket* next();
  nmethodBucket* next_not_unloading(NMethodUnloadingStats* stats = nullptr);
  nmethodBucket* previous();
  void set_next(nmethodBucket* b);
  void set_previous(nmethodBucket* b);
  nmethodBucket* nmethod_next();
  void set_nmethod_next(nmethodBucket* b);
  nmethodBucket* purge_list_next();
  void set_purge_list_next(nmethodBucket* b);
  nmethod* get_nmethod()                     { return _nmethod; }
  const InstanceKlass* owner_klass() const    { return _owner_klass; }
  bool owner_is_call_site() const             { return _owner_is_call_site; }
  bool is_removed() const                     { return _removed; }
  void mark_removed()                         { _removed = true; }
  bool unlink_from_context();
};

//
// Utility class to manipulate nmethod dependency context.
// Dependency context can be attached either to an InstanceKlass (_dep_context field)
// or CallSite oop for call_site_target dependencies (see javaClasses.hpp).
// DependencyContext class operates on some location which holds a nmethodBucket* value
// and uint64_t integer recording the safepoint counter at the last cleanup.
//
class DependencyContext : public StackObj {
  friend class TestDependencyContext;
 private:
  nmethodBucket* volatile* _dependency_context_addr;
  volatile uint64_t*       _last_cleanup_addr;

  bool claim_cleanup();
  static bool delete_on_release();
  void set_dependencies(nmethodBucket* b);
  nmethodBucket* dependencies();
  nmethodBucket* dependencies_not_unloading(NMethodUnloadingStats* stats = nullptr);

  static PerfCounter*            _perf_total_buckets_allocated_count;
  static PerfCounter*            _perf_total_buckets_deallocated_count;
  static PerfCounter*            _perf_total_buckets_stale_count;
  static PerfCounter*            _perf_total_buckets_stale_acc_count;
  static nmethodBucket* volatile _purge_list;
  static uint64_t                _cleaning_epoch_monotonic;
  static volatile uint64_t       _cleaning_epoch;

 public:
#ifdef ASSERT
  // Safepoints are forbidden during DC lifetime. GC can invalidate
  // _dependency_context_addr if it relocates the holder
  SafepointStateTracker _safepoint_tracker;

  DependencyContext(nmethodBucket* volatile* bucket_addr, volatile uint64_t* last_cleanup_addr)
    : _dependency_context_addr(bucket_addr),
      _last_cleanup_addr(last_cleanup_addr),
      _safepoint_tracker(SafepointSynchronize::safepoint_state_tracker()) {}

  ~DependencyContext() {
    assert(!_safepoint_tracker.safepoint_state_changed(), "must be the same safepoint");
  }
#else
  DependencyContext(nmethodBucket* volatile* bucket_addr, volatile uint64_t* last_cleanup_addr)
    : _dependency_context_addr(bucket_addr),
      _last_cleanup_addr(last_cleanup_addr) {}
#endif // ASSERT

  static void init();

  void mark_dependent_nmethods(DeoptimizationScope* deopt_scope, DepChange& changes);
  void add_dependent_nmethod(nmethod* nm,
                             const InstanceKlass* owner_klass = nullptr,
                             bool owner_is_call_site = false);
  bool remove_dependent_nmethod(nmethod* nm, NMethodUnloadingStats* stats = nullptr);
  void remove_all_dependents();
  void clean_unloading_dependents(NMethodUnloadingStats* stats = nullptr);
  static void purge_dependency_contexts();
  static void release(nmethodBucket* b);
  static void cleaning_start();
  static void cleaning_end();

#ifndef PRODUCT
  void print_dependent_nmethods(bool verbose);
  bool is_empty();
#endif //PRODUCT
  bool is_dependent_nmethod(nmethod* nm);
};
#endif // SHARE_CODE_DEPENDENCYCONTEXT_HPP
