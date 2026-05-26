/*
 * Copyright (c) 2019, 2026, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1PARALLELCLEANING_HPP
#define SHARE_GC_G1_G1PARALLELCLEANING_HPP

#include "gc/shared/parallelCleaning.hpp"
#if INCLUDE_JVMCI
#include "runtime/atomic.hpp"
#endif

class outputStream;
class InstanceKlass;
template <class T> class WorkerDataArray;

#if INCLUDE_JVMCI
class JVMCICleaningTask : public StackObj {
  Atomic<bool> _cleaning_claimed;

public:
  JVMCICleaningTask();
  // Clean JVMCI metadata handles.
  size_t work(bool unloading_occurred, bool* did_work);

private:
  bool claim_cleaning_task();
};
#endif

// Do cleanup of some weakly held data in the same parallel task.
// Assumes a non-moving context.
class G1ParallelCleaningTask : public WorkerTask {
private:
  enum G1CleaningSubPhase {
    JVMCIHandles,
    CodeCacheUnloading,
    KlassCleaning,
    G1CleaningSubPhaseCount
  };

  enum G1CleaningWorkItem {
    ProcessedItems,
    ProcessedKlasses,
    G1CleaningWorkItemCount
  };

  enum NMethodUnloadingSubPhase {
    NMethodDoUnloading,
    NMethodIsUnloading,
    NMethodIsUnloadingState,
    NMethodIsUnloadingCached,
    NMethodIsUnloadingUncached,
    NMethodIsUnloadingNonNMethod,
    NMethodHasDeadOop,
    NMethodHasDeadOopRootFilter,
    NMethodHasDeadOopImmediateFilter,
    NMethodHasDeadOopImmediateOops,
    NMethodHasDeadOopOopTable,
    NMethodHasDeadOopIsAlive,
    NMethodIsCold,
    NMethodIsColdPrecheck,
    NMethodIsColdStackState,
    NMethodIsColdEntryBarrier,
    NMethodIsColdEpoch,
    NMethodIsUnloadingCAS,
    NMethodUnlink,
    NMethodFlushDependencies,
    NMethodCallSiteDependency,
    NMethodKlassDependency,
    NMethodDependencyContextRemove,
    NMethodDependencyContextRemoveMax,
    NMethodDependencyContextWalk,
    NMethodDependencyContextMaxWalk,
    NMethodDependencyBucketIsUnloading,
    NMethodDependencyBucketUnlink,
    NMethodUnlinkFromMethod,
    NMethodUnlinkOSR,
    NMethodUnlinkJVMCI,
    NMethodPostUnload,
    NMethodRegisterUnlinked,
    NMethodUnloadCaches,
    NMethodUnloadExceptionCache,
    NMethodUnloadInlineCaches,
    NMethodInlineCacheFilter,
    NMethodInlineCacheCleanMetadata,
    NMethodInlineCacheCleanNMethod,
    NMethodInlineCacheNMethodLookup,
    NMethodInlineCacheNMethodState,
    NMethodInlineCacheIsInUse,
    NMethodInlineCacheIsUnloading,
    NMethodInlineCacheMethodCode,
    NMethodInlineCacheMetadataReloc,
    NMethodUnloadVerifyMetadata,
    NMethodDisarm,
    NMethodUnloadingSubPhaseCount
  };

  bool                    _unloading_occurred;
  bool                    _record_stats;
  CodeCacheUnloadingTask  _code_cache_task;
#if INCLUDE_JVMCI
  JVMCICleaningTask       _jvmci_cleaning_task;
#endif
  KlassCleaningTask       _klass_cleaning_task;
  WorkerDataArray<double>* _worker_times[G1CleaningSubPhaseCount];
  WorkerDataArray<double>* _nmethod_unloading_times[NMethodUnloadingSubPhaseCount];
  uint                     _num_workers;
  jlong*                   _max_dependency_context_ticks;
  size_t*                  _max_dependency_context_buckets;
  const InstanceKlass**    _max_dependency_context_klasses;
  bool*                    _max_dependency_context_is_call_site;
  jlong*                   _max_dependency_context_remove_ticks;
  size_t*                  _max_dependency_context_remove_buckets;
  const InstanceKlass**    _max_dependency_context_remove_klasses;
  bool*                    _max_dependency_context_remove_is_call_site;

  void record_worker_time(G1CleaningSubPhase phase,
                          uint worker_id,
                          jlong start_counter,
                          size_t num_processed_items,
                          size_t num_processed_klasses = 0);
  void record_nmethod_unloading_time(NMethodUnloadingSubPhase phase,
                                     uint worker_id,
                                     jlong ticks,
                                     size_t num_processed_items,
                                     size_t num_secondary_items = 0);
  void record_nmethod_unloading_stats(uint worker_id, const NMethodUnloadingStats& stats);
  void log_phase(G1CleaningSubPhase phase, uint indent_level, outputStream* out) const;
  void log_nmethod_unloading_stats(uint indent_level, outputStream* out) const;
  void log_slowest_dependency_context(uint indent_level, outputStream* out) const;

public:
  // The constructor is run in the VMThread.
  G1ParallelCleaningTask(bool unloading_occurred, uint num_workers, bool record_stats);
  ~G1ParallelCleaningTask();

  void work(uint worker_id);
  void log_statistics() const;
};

#endif // SHARE_GC_G1_G1PARALLELCLEANING_HPP
