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


#include "gc/g1/g1ParallelCleaning.hpp"
#include "gc/shared/workerDataArray.inline.hpp"
#if INCLUDE_JVMCI
#include "jvmci/jvmci.hpp"
#endif
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/resourceArea.hpp"
#include "oops/instanceKlass.hpp"
#include "runtime/os.hpp"
#include "runtime/timer.hpp"

#if INCLUDE_JVMCI
JVMCICleaningTask::JVMCICleaningTask() :
  _cleaning_claimed(false) {
}

bool JVMCICleaningTask::claim_cleaning_task() {
  if (_cleaning_claimed.load_relaxed()) {
    return false;
  }

  return _cleaning_claimed.compare_set(false, true);
}

size_t JVMCICleaningTask::work(bool unloading_occurred, bool* did_work) {
  *did_work = false;

  // One worker will clean JVMCI metadata handles.
  if (unloading_occurred && EnableJVMCI && claim_cleaning_task()) {
    *did_work = true;
    return JVMCI::do_unloading(unloading_occurred);
  }
  return 0;
}
#endif // INCLUDE_JVMCI

G1ParallelCleaningTask::G1ParallelCleaningTask(bool unloading_occurred,
                                               uint num_workers,
                                               bool record_stats) :
  WorkerTask("G1 Parallel Cleaning"),
  _unloading_occurred(unloading_occurred),
  _record_stats(record_stats),
  _code_cache_task(unloading_occurred, num_workers),
  JVMCI_ONLY(_jvmci_cleaning_task() COMMA)
  _klass_cleaning_task(),
  _worker_times(),
  _nmethod_unloading_times(),
  _num_workers(num_workers),
  _max_dependency_context_ticks(nullptr),
  _max_dependency_context_buckets(nullptr),
  _max_dependency_context_klasses(nullptr),
  _max_dependency_context_is_call_site(nullptr),
  _max_dependency_context_remove_ticks(nullptr),
  _max_dependency_context_remove_buckets(nullptr),
  _max_dependency_context_remove_klasses(nullptr),
  _max_dependency_context_remove_is_call_site(nullptr) {
  if (!_record_stats) {
    return;
  }

  _max_dependency_context_ticks = NEW_C_HEAP_ARRAY(jlong, num_workers, mtGC);
  _max_dependency_context_buckets = NEW_C_HEAP_ARRAY(size_t, num_workers, mtGC);
  _max_dependency_context_klasses = NEW_C_HEAP_ARRAY(const InstanceKlass*, num_workers, mtGC);
  _max_dependency_context_is_call_site = NEW_C_HEAP_ARRAY(bool, num_workers, mtGC);
  _max_dependency_context_remove_ticks = NEW_C_HEAP_ARRAY(jlong, num_workers, mtGC);
  _max_dependency_context_remove_buckets = NEW_C_HEAP_ARRAY(size_t, num_workers, mtGC);
  _max_dependency_context_remove_klasses = NEW_C_HEAP_ARRAY(const InstanceKlass*, num_workers, mtGC);
  _max_dependency_context_remove_is_call_site = NEW_C_HEAP_ARRAY(bool, num_workers, mtGC);

  for (uint i = 0; i < num_workers; i++) {
    _max_dependency_context_ticks[i] = 0;
    _max_dependency_context_buckets[i] = 0;
    _max_dependency_context_klasses[i] = nullptr;
    _max_dependency_context_is_call_site[i] = false;
    _max_dependency_context_remove_ticks[i] = 0;
    _max_dependency_context_remove_buckets[i] = 0;
    _max_dependency_context_remove_klasses[i] = nullptr;
    _max_dependency_context_remove_is_call_site[i] = false;
  }

  _worker_times[JVMCIHandles] = new WorkerDataArray<double>(nullptr, "JVMCI Metadata Handles (ms):", num_workers);
  _worker_times[JVMCIHandles]->create_thread_work_items("Metadata Handles:", ProcessedItems);

  _worker_times[CodeCacheUnloading] = new WorkerDataArray<double>(nullptr, "Code Cache Unloading (ms):", num_workers);
  _worker_times[CodeCacheUnloading]->create_thread_work_items("NMethods:", ProcessedItems);

  _nmethod_unloading_times[NMethodDoUnloading] = new WorkerDataArray<double>(nullptr, "NMethod do_unloading (ms):", num_workers);
  _nmethod_unloading_times[NMethodDoUnloading]->create_thread_work_items("NMethods:", ProcessedItems);

  _nmethod_unloading_times[NMethodIsUnloading] = new WorkerDataArray<double>(nullptr, "Is Unloading (ms):", num_workers);
  _nmethod_unloading_times[NMethodIsUnloading]->create_thread_work_items("NMethods:", ProcessedItems);

  _nmethod_unloading_times[NMethodIsUnloadingState] = new WorkerDataArray<double>(nullptr, "Is Unloading State (ms):", num_workers);
  _nmethod_unloading_times[NMethodIsUnloadingState]->create_thread_work_items("NMethods:", ProcessedItems);

  _nmethod_unloading_times[NMethodIsUnloadingCached] = new WorkerDataArray<double>(nullptr, "Cached Is Unloading (ms):", num_workers);
  _nmethod_unloading_times[NMethodIsUnloadingCached]->create_thread_work_items("Cached NMethods:", ProcessedItems);

  _nmethod_unloading_times[NMethodIsUnloadingUncached] = new WorkerDataArray<double>(nullptr, "Uncached Is Unloading (ms):", num_workers);
  _nmethod_unloading_times[NMethodIsUnloadingUncached]->create_thread_work_items("Uncached NMethods:", ProcessedItems);

  _nmethod_unloading_times[NMethodIsUnloadingNonNMethod] = new WorkerDataArray<double>(nullptr, "NonNMethod Is Unloading (ms):", num_workers);
  _nmethod_unloading_times[NMethodIsUnloadingNonNMethod]->create_thread_work_items("Checks:", ProcessedItems);
  _nmethod_unloading_times[NMethodIsUnloadingNonNMethod]->create_thread_work_items("NonNMethods:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodHasDeadOop] = new WorkerDataArray<double>(nullptr, "Has Dead Oop (ms):", num_workers);
  _nmethod_unloading_times[NMethodHasDeadOop]->create_thread_work_items("Checked NMethods:", ProcessedItems);
  _nmethod_unloading_times[NMethodHasDeadOop]->create_thread_work_items("Dead Oop NMethods:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodHasDeadOopRootFilter] = new WorkerDataArray<double>(nullptr, "Has Dead Oop Root Filter (ms):", num_workers);
  _nmethod_unloading_times[NMethodHasDeadOopRootFilter]->create_thread_work_items("Root NMethods:", ProcessedItems);
  _nmethod_unloading_times[NMethodHasDeadOopRootFilter]->create_thread_work_items("No Root NMethods:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodHasDeadOopImmediateFilter] = new WorkerDataArray<double>(nullptr, "Has Dead Oop Immediate Filter (ms):", num_workers);
  _nmethod_unloading_times[NMethodHasDeadOopImmediateFilter]->create_thread_work_items("Immediate NMethods:", ProcessedItems);
  _nmethod_unloading_times[NMethodHasDeadOopImmediateFilter]->create_thread_work_items("No Immediate NMethods:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodHasDeadOopImmediateOops] = new WorkerDataArray<double>(nullptr, "Has Dead Oop Immediate Oops (ms):", num_workers);
  _nmethod_unloading_times[NMethodHasDeadOopImmediateOops]->create_thread_work_items("Relocations:", ProcessedItems);
  _nmethod_unloading_times[NMethodHasDeadOopImmediateOops]->create_thread_work_items("Immediate Oops:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodHasDeadOopOopTable] = new WorkerDataArray<double>(nullptr, "Has Dead Oop Oop Table (ms):", num_workers);
  _nmethod_unloading_times[NMethodHasDeadOopOopTable]->create_thread_work_items("Entries:", ProcessedItems);
  _nmethod_unloading_times[NMethodHasDeadOopOopTable]->create_thread_work_items("Oops:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodHasDeadOopIsAlive] = new WorkerDataArray<double>(nullptr, "Has Dead Oop Is Alive (ms):", num_workers);
  _nmethod_unloading_times[NMethodHasDeadOopIsAlive]->create_thread_work_items("Non-Null Oops:", ProcessedItems);
  _nmethod_unloading_times[NMethodHasDeadOopIsAlive]->create_thread_work_items("Dead Oops:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodIsCold] = new WorkerDataArray<double>(nullptr, "Is Cold (ms):", num_workers);
  _nmethod_unloading_times[NMethodIsCold]->create_thread_work_items("Checked NMethods:", ProcessedItems);
  _nmethod_unloading_times[NMethodIsCold]->create_thread_work_items("Cold NMethods:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodIsColdPrecheck] = new WorkerDataArray<double>(nullptr, "Is Cold Precheck (ms):", num_workers);
  _nmethod_unloading_times[NMethodIsColdPrecheck]->create_thread_work_items("Checks:", ProcessedItems);
  _nmethod_unloading_times[NMethodIsColdPrecheck]->create_thread_work_items("Bailouts:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodIsColdStackState] = new WorkerDataArray<double>(nullptr, "Is Cold Stack State (ms):", num_workers);
  _nmethod_unloading_times[NMethodIsColdStackState]->create_thread_work_items("Checks:", ProcessedItems);
  _nmethod_unloading_times[NMethodIsColdStackState]->create_thread_work_items("Cold NMethods:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodIsColdEntryBarrier] = new WorkerDataArray<double>(nullptr, "Is Cold Entry Barrier (ms):", num_workers);
  _nmethod_unloading_times[NMethodIsColdEntryBarrier]->create_thread_work_items("Checks:", ProcessedItems);
  _nmethod_unloading_times[NMethodIsColdEntryBarrier]->create_thread_work_items("Unsupported:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodIsColdEpoch] = new WorkerDataArray<double>(nullptr, "Is Cold Epoch (ms):", num_workers);
  _nmethod_unloading_times[NMethodIsColdEpoch]->create_thread_work_items("Checks:", ProcessedItems);
  _nmethod_unloading_times[NMethodIsColdEpoch]->create_thread_work_items("Cold NMethods:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodIsUnloadingCAS] = new WorkerDataArray<double>(nullptr, "Is Unloading CAS (ms):", num_workers);
  _nmethod_unloading_times[NMethodIsUnloadingCAS]->create_thread_work_items("CAS NMethods:", ProcessedItems);
  _nmethod_unloading_times[NMethodIsUnloadingCAS]->create_thread_work_items("CAS Wins:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodUnlink] = new WorkerDataArray<double>(nullptr, "Unlink NMethods (ms):", num_workers);
  _nmethod_unloading_times[NMethodUnlink]->create_thread_work_items("Unloading NMethods:", ProcessedItems);
  _nmethod_unloading_times[NMethodUnlink]->create_thread_work_items("Already Unlinked:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodFlushDependencies] = new WorkerDataArray<double>(nullptr, "Flush Dependencies (ms):", num_workers);
  _nmethod_unloading_times[NMethodFlushDependencies]->create_thread_work_items("Dependencies:", ProcessedItems);
  _nmethod_unloading_times[NMethodFlushDependencies]->create_thread_work_items("NMethods:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodCallSiteDependency] = new WorkerDataArray<double>(nullptr, "CallSite Dependencies (ms):", num_workers);
  _nmethod_unloading_times[NMethodCallSiteDependency]->create_thread_work_items("CallSite Dependencies:", ProcessedItems);

  _nmethod_unloading_times[NMethodKlassDependency] = new WorkerDataArray<double>(nullptr, "Klass Dependencies (ms):", num_workers);
  _nmethod_unloading_times[NMethodKlassDependency]->create_thread_work_items("Klass Dependencies:", ProcessedItems);

  _nmethod_unloading_times[NMethodDependencyContextRemove] = new WorkerDataArray<double>(nullptr, "Remove Dependency Context (ms):", num_workers);
  _nmethod_unloading_times[NMethodDependencyContextRemove]->create_thread_work_items("Processed Buckets:", ProcessedItems);
  _nmethod_unloading_times[NMethodDependencyContextRemove]->create_thread_work_items("Removed Buckets:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodDependencyContextRemoveMax] = new WorkerDataArray<double>(nullptr, "Max Remove Dependency Context (ms):", num_workers);
  _nmethod_unloading_times[NMethodDependencyContextRemoveMax]->create_thread_work_items("Max Buckets:", ProcessedItems);

  _nmethod_unloading_times[NMethodDependencyContextWalk] = new WorkerDataArray<double>(nullptr, "Dependency Context Walk (ms):", num_workers);
  _nmethod_unloading_times[NMethodDependencyContextWalk]->create_thread_work_items("Claimed Contexts:", ProcessedItems);
  _nmethod_unloading_times[NMethodDependencyContextWalk]->create_thread_work_items("Skipped Contexts:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodDependencyContextMaxWalk] = new WorkerDataArray<double>(nullptr, "Max Dependency Context Walk (ms):", num_workers);
  _nmethod_unloading_times[NMethodDependencyContextMaxWalk]->create_thread_work_items("Max Buckets:", ProcessedItems);

  _nmethod_unloading_times[NMethodDependencyBucketIsUnloading] = new WorkerDataArray<double>(nullptr, "Dependency Bucket Is Unloading (ms):", num_workers);
  _nmethod_unloading_times[NMethodDependencyBucketIsUnloading]->create_thread_work_items("Checked Buckets:", ProcessedItems);
  _nmethod_unloading_times[NMethodDependencyBucketIsUnloading]->create_thread_work_items("Unloading Buckets:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodDependencyBucketUnlink] = new WorkerDataArray<double>(nullptr, "Unlink Dependency Buckets (ms):", num_workers);
  _nmethod_unloading_times[NMethodDependencyBucketUnlink]->create_thread_work_items("Unlink Attempts:", ProcessedItems);
  _nmethod_unloading_times[NMethodDependencyBucketUnlink]->create_thread_work_items("Unlinked Buckets:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodUnlinkFromMethod] = new WorkerDataArray<double>(nullptr, "Unlink From Method (ms):", num_workers);
  _nmethod_unloading_times[NMethodUnlinkFromMethod]->create_thread_work_items("NMethods:", ProcessedItems);

  _nmethod_unloading_times[NMethodUnlinkOSR] = new WorkerDataArray<double>(nullptr, "Unlink OSR (ms):", num_workers);
  _nmethod_unloading_times[NMethodUnlinkOSR]->create_thread_work_items("OSR NMethods:", ProcessedItems);

  _nmethod_unloading_times[NMethodUnlinkJVMCI] = new WorkerDataArray<double>(nullptr, "Unlink JVMCI (ms):", num_workers);
  _nmethod_unloading_times[NMethodUnlinkJVMCI]->create_thread_work_items("JVMCI NMethods:", ProcessedItems);

  _nmethod_unloading_times[NMethodPostUnload] = new WorkerDataArray<double>(nullptr, "Post Unload Event (ms):", num_workers);
  _nmethod_unloading_times[NMethodPostUnload]->create_thread_work_items("NMethods:", ProcessedItems);

  _nmethod_unloading_times[NMethodRegisterUnlinked] = new WorkerDataArray<double>(nullptr, "Register Unlinked (ms):", num_workers);
  _nmethod_unloading_times[NMethodRegisterUnlinked]->create_thread_work_items("NMethods:", ProcessedItems);

  _nmethod_unloading_times[NMethodUnloadCaches] = new WorkerDataArray<double>(nullptr, "Unload NMethod Caches (ms):", num_workers);
  _nmethod_unloading_times[NMethodUnloadCaches]->create_thread_work_items("Live NMethods:", ProcessedItems);

  _nmethod_unloading_times[NMethodUnloadExceptionCache] = new WorkerDataArray<double>(nullptr, "Unload Exception Cache (ms):", num_workers);
  _nmethod_unloading_times[NMethodUnloadExceptionCache]->create_thread_work_items("Entries:", ProcessedItems);
  _nmethod_unloading_times[NMethodUnloadExceptionCache]->create_thread_work_items("Removed Entries:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodUnloadInlineCaches] = new WorkerDataArray<double>(nullptr, "Unload Inline Caches (ms):", num_workers);
  _nmethod_unloading_times[NMethodUnloadInlineCaches]->create_thread_work_items("Relocations:", ProcessedItems);
  _nmethod_unloading_times[NMethodUnloadInlineCaches]->create_thread_work_items("Cleaned Calls:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodInlineCacheFilter] = new WorkerDataArray<double>(nullptr, "IC NMethod Filter (ms):", num_workers);
  _nmethod_unloading_times[NMethodInlineCacheFilter]->create_thread_work_items("Checked NMethods:", ProcessedItems);
  _nmethod_unloading_times[NMethodInlineCacheFilter]->create_thread_work_items("Skipped NMethods:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodInlineCacheCleanMetadata] = new WorkerDataArray<double>(nullptr, "IC Clean Metadata (ms):", num_workers);
  _nmethod_unloading_times[NMethodInlineCacheCleanMetadata]->create_thread_work_items("Virtual Calls:", ProcessedItems);

  _nmethod_unloading_times[NMethodInlineCacheCleanNMethod] = new WorkerDataArray<double>(nullptr, "IC NMethod Checks (ms):", num_workers);
  _nmethod_unloading_times[NMethodInlineCacheCleanNMethod]->create_thread_work_items("Callsites:", ProcessedItems);
  _nmethod_unloading_times[NMethodInlineCacheCleanNMethod]->create_thread_work_items("Cleaned Calls:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodInlineCacheNMethodLookup] = new WorkerDataArray<double>(nullptr, "IC NMethod Lookup (ms):", num_workers);
  _nmethod_unloading_times[NMethodInlineCacheNMethodLookup]->create_thread_work_items("Lookups:", ProcessedItems);
  _nmethod_unloading_times[NMethodInlineCacheNMethodLookup]->create_thread_work_items("Skipped Calls:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodInlineCacheNMethodState] = new WorkerDataArray<double>(nullptr, "IC NMethod State (ms):", num_workers);
  _nmethod_unloading_times[NMethodInlineCacheNMethodState]->create_thread_work_items("NMethods:", ProcessedItems);
  _nmethod_unloading_times[NMethodInlineCacheNMethodState]->create_thread_work_items("Cleaned Calls:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodInlineCacheIsInUse] = new WorkerDataArray<double>(nullptr, "IC Is In Use (ms):", num_workers);
  _nmethod_unloading_times[NMethodInlineCacheIsInUse]->create_thread_work_items("Checks:", ProcessedItems);
  _nmethod_unloading_times[NMethodInlineCacheIsInUse]->create_thread_work_items("Not In Use:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodInlineCacheIsUnloading] = new WorkerDataArray<double>(nullptr, "IC Is Unloading (ms):", num_workers);
  _nmethod_unloading_times[NMethodInlineCacheIsUnloading]->create_thread_work_items("Checks:", ProcessedItems);
  _nmethod_unloading_times[NMethodInlineCacheIsUnloading]->create_thread_work_items("Unloading:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodInlineCacheMethodCode] = new WorkerDataArray<double>(nullptr, "IC Method Code (ms):", num_workers);
  _nmethod_unloading_times[NMethodInlineCacheMethodCode]->create_thread_work_items("Checks:", ProcessedItems);
  _nmethod_unloading_times[NMethodInlineCacheMethodCode]->create_thread_work_items("Skipped:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodInlineCacheMetadataReloc] = new WorkerDataArray<double>(nullptr, "IC Metadata Relocs (ms):", num_workers);
  _nmethod_unloading_times[NMethodInlineCacheMetadataReloc]->create_thread_work_items("Metadata Relocs:", ProcessedItems);
  _nmethod_unloading_times[NMethodInlineCacheMetadataReloc]->create_thread_work_items("Cleared Relocs:", ProcessedKlasses);

  _nmethod_unloading_times[NMethodUnloadVerifyMetadata] = new WorkerDataArray<double>(nullptr, "Verify Metadata (ms):", num_workers);
  _nmethod_unloading_times[NMethodUnloadVerifyMetadata]->create_thread_work_items("NMethods:", ProcessedItems);

  _nmethod_unloading_times[NMethodDisarm] = new WorkerDataArray<double>(nullptr, "Disarm NMethods (ms):", num_workers);
  _nmethod_unloading_times[NMethodDisarm]->create_thread_work_items("Disarmed NMethods:", ProcessedItems);

  _worker_times[KlassCleaning] = new WorkerDataArray<double>(nullptr, "Klass Cleaning (ms):", num_workers);
  _worker_times[KlassCleaning]->create_thread_work_items("ClassLoaderData:", ProcessedItems);
  _worker_times[KlassCleaning]->create_thread_work_items("Klasses:", ProcessedKlasses);
}

G1ParallelCleaningTask::~G1ParallelCleaningTask() {
  if (!_record_stats) {
    return;
  }

  for (uint i = 0; i < G1CleaningSubPhaseCount; i++) {
    delete _worker_times[i];
  }
  for (uint i = 0; i < NMethodUnloadingSubPhaseCount; i++) {
    delete _nmethod_unloading_times[i];
  }
  FREE_C_HEAP_ARRAY(_max_dependency_context_ticks);
  FREE_C_HEAP_ARRAY(_max_dependency_context_buckets);
  FREE_C_HEAP_ARRAY(_max_dependency_context_klasses);
  FREE_C_HEAP_ARRAY(_max_dependency_context_is_call_site);
  FREE_C_HEAP_ARRAY(_max_dependency_context_remove_ticks);
  FREE_C_HEAP_ARRAY(_max_dependency_context_remove_buckets);
  FREE_C_HEAP_ARRAY(_max_dependency_context_remove_klasses);
  FREE_C_HEAP_ARRAY(_max_dependency_context_remove_is_call_site);
}

void G1ParallelCleaningTask::record_worker_time(G1CleaningSubPhase phase,
                                                uint worker_id,
                                                jlong start_counter,
                                                size_t num_processed_items,
                                                size_t num_processed_klasses) {
  assert(_record_stats, "Should not record statistics");
  double elapsed_secs = TimeHelper::counter_to_seconds(os::elapsed_counter() - start_counter);
  WorkerDataArray<double>* worker_time = _worker_times[phase];

  worker_time->set(worker_id, elapsed_secs);
  worker_time->set_thread_work_item(worker_id, num_processed_items, ProcessedItems);
  if (phase == KlassCleaning) {
    worker_time->set_thread_work_item(worker_id, num_processed_klasses, ProcessedKlasses);
  }
}

void G1ParallelCleaningTask::record_nmethod_unloading_time(NMethodUnloadingSubPhase phase,
                                                           uint worker_id,
                                                           jlong ticks,
                                                           size_t num_processed_items,
                                                           size_t num_secondary_items) {
  assert(_record_stats, "Should not record statistics");
  WorkerDataArray<double>* worker_time = _nmethod_unloading_times[phase];
  worker_time->set(worker_id, TimeHelper::counter_to_seconds(ticks));
  worker_time->set_thread_work_item(worker_id, num_processed_items, ProcessedItems);
  if (worker_time->thread_work_items(ProcessedKlasses) != nullptr) {
    worker_time->set_thread_work_item(worker_id, num_secondary_items, ProcessedKlasses);
  }
}

void G1ParallelCleaningTask::record_nmethod_unloading_stats(uint worker_id, const NMethodUnloadingStats& stats) {
  assert(_record_stats, "Should not record statistics");
  _max_dependency_context_ticks[worker_id] = stats._dependency_context_max_ticks;
  _max_dependency_context_buckets[worker_id] = stats._dependency_context_max_buckets;
  _max_dependency_context_klasses[worker_id] = stats._dependency_context_max_klass;
  _max_dependency_context_is_call_site[worker_id] = stats._dependency_context_max_is_call_site;
  _max_dependency_context_remove_ticks[worker_id] = stats._dependency_context_remove_max_ticks;
  _max_dependency_context_remove_buckets[worker_id] = stats._dependency_context_remove_max_buckets;
  _max_dependency_context_remove_klasses[worker_id] = stats._dependency_context_remove_max_klass;
  _max_dependency_context_remove_is_call_site[worker_id] = stats._dependency_context_remove_max_is_call_site;

  record_nmethod_unloading_time(NMethodDoUnloading, worker_id,
                                stats._total_ticks, stats._processed_nmethods);
  record_nmethod_unloading_time(NMethodIsUnloading, worker_id,
                                stats._is_unloading_ticks, stats._processed_nmethods);
  record_nmethod_unloading_time(NMethodIsUnloadingState, worker_id,
                                stats._is_unloading_state_ticks, stats._is_unloading_state_nmethods);
  record_nmethod_unloading_time(NMethodIsUnloadingCached, worker_id,
                                stats._is_unloading_cached_ticks, stats._is_unloading_cached_nmethods);
  record_nmethod_unloading_time(NMethodIsUnloadingUncached, worker_id,
                                stats._is_unloading_uncached_ticks, stats._is_unloading_uncached_nmethods);
  record_nmethod_unloading_time(NMethodIsUnloadingNonNMethod, worker_id,
                                stats._is_unloading_non_nmethod_ticks,
                                stats._is_unloading_uncached_nmethods,
                                stats._is_unloading_non_nmethod_nmethods);
  record_nmethod_unloading_time(NMethodHasDeadOop, worker_id,
                                stats._has_dead_oop_ticks, stats._has_dead_oop_checks, stats._dead_oop_nmethods);
  record_nmethod_unloading_time(NMethodHasDeadOopRootFilter, worker_id,
                                stats._has_dead_oop_root_filter_ticks,
                                stats._has_dead_oop_root_nmethods,
                                stats._has_dead_oop_no_root_nmethods);
  record_nmethod_unloading_time(NMethodHasDeadOopImmediateFilter, worker_id,
                                stats._has_dead_oop_immediate_filter_ticks,
                                stats._has_dead_oop_immediate_oop_nmethods,
                                stats._has_dead_oop_no_immediate_oop_nmethods);
  record_nmethod_unloading_time(NMethodHasDeadOopImmediateOops, worker_id,
                                stats._has_dead_oop_immediate_oop_ticks,
                                stats._has_dead_oop_relocations,
                                stats._has_dead_oop_immediate_oops);
  record_nmethod_unloading_time(NMethodHasDeadOopOopTable, worker_id,
                                stats._has_dead_oop_oop_table_ticks,
                                stats._has_dead_oop_oop_table_entries,
                                stats._has_dead_oop_oop_table_oops);
  record_nmethod_unloading_time(NMethodHasDeadOopIsAlive, worker_id,
                                stats._has_dead_oop_is_alive_ticks,
                                stats._has_dead_oop_non_null_oops,
                                stats._has_dead_oop_dead_oops);
  record_nmethod_unloading_time(NMethodIsCold, worker_id,
                                stats._is_cold_ticks, stats._is_cold_checks, stats._cold_nmethods);
  record_nmethod_unloading_time(NMethodIsColdPrecheck, worker_id,
                                stats._is_cold_precheck_ticks,
                                stats._is_cold_precheck_checks,
                                stats._is_cold_precheck_bailouts);
  record_nmethod_unloading_time(NMethodIsColdStackState, worker_id,
                                stats._is_cold_stack_state_ticks,
                                stats._is_cold_stack_state_checks,
                                stats._is_cold_stack_state_cold);
  record_nmethod_unloading_time(NMethodIsColdEntryBarrier, worker_id,
                                stats._is_cold_entry_barrier_ticks,
                                stats._is_cold_entry_barrier_checks,
                                stats._is_cold_entry_barrier_unsupported);
  record_nmethod_unloading_time(NMethodIsColdEpoch, worker_id,
                                stats._is_cold_epoch_ticks,
                                stats._is_cold_epoch_checks,
                                stats._is_cold_epoch_cold);
  record_nmethod_unloading_time(NMethodIsUnloadingCAS, worker_id,
                                stats._is_unloading_cas_ticks, stats._is_unloading_cas_nmethods, stats._is_unloading_cas_wins);
  record_nmethod_unloading_time(NMethodUnlink, worker_id,
                                stats._unlink_ticks, stats._unloading_nmethods, stats._unlink_already_unlinked_nmethods);
  record_nmethod_unloading_time(NMethodFlushDependencies, worker_id,
                                stats._unlink_flush_dependencies_ticks, stats._dependencies_processed, stats._flush_dependencies_nmethods);
  record_nmethod_unloading_time(NMethodCallSiteDependency, worker_id,
                                stats._call_site_dependency_ticks, stats._call_site_dependencies);
  record_nmethod_unloading_time(NMethodKlassDependency, worker_id,
                                stats._klass_dependency_ticks, stats._klass_dependencies);
  record_nmethod_unloading_time(NMethodDependencyContextRemove, worker_id,
                                stats._dependency_context_remove_ticks,
                                stats._dependency_context_remove_buckets,
                                stats._dependency_context_remove_removed);
  record_nmethod_unloading_time(NMethodDependencyContextRemoveMax, worker_id,
                                stats._dependency_context_remove_max_ticks,
                                stats._dependency_context_remove_max_buckets);
  record_nmethod_unloading_time(NMethodDependencyContextWalk, worker_id,
                                stats._dependency_context_walk_ticks,
                                stats._dependency_contexts_claimed,
                                stats._dependency_contexts_skipped);
  record_nmethod_unloading_time(NMethodDependencyContextMaxWalk, worker_id,
                                stats._dependency_context_max_ticks,
                                stats._dependency_context_max_buckets);
  record_nmethod_unloading_time(NMethodDependencyBucketIsUnloading, worker_id,
                                stats._dependency_bucket_is_unloading_ticks,
                                stats._dependency_buckets_checked,
                                stats._dependency_buckets_unloading);
  record_nmethod_unloading_time(NMethodDependencyBucketUnlink, worker_id,
                                stats._dependency_bucket_unlink_ticks,
                                stats._dependency_bucket_unlink_attempts,
                                stats._dependency_buckets_unlinked);
  record_nmethod_unloading_time(NMethodUnlinkFromMethod, worker_id,
                                stats._unlink_from_method_ticks, stats._unloading_nmethods);
  record_nmethod_unloading_time(NMethodUnlinkOSR, worker_id,
                                stats._unlink_osr_ticks, stats._osr_nmethods);
  record_nmethod_unloading_time(NMethodUnlinkJVMCI, worker_id,
                                stats._unlink_jvmci_ticks, stats._jvmci_nmethods);
  record_nmethod_unloading_time(NMethodPostUnload, worker_id,
                                stats._unlink_post_unload_ticks, stats._unloading_nmethods);
  record_nmethod_unloading_time(NMethodRegisterUnlinked, worker_id,
                                stats._register_unlinked_ticks, stats._unloading_nmethods);
  record_nmethod_unloading_time(NMethodUnloadCaches, worker_id,
                                stats._unload_caches_ticks, stats._live_nmethods);
  record_nmethod_unloading_time(NMethodUnloadExceptionCache, worker_id,
                                stats._unload_exception_cache_ticks,
                                stats._exception_cache_entries,
                                stats._exception_cache_removed);
  record_nmethod_unloading_time(NMethodUnloadInlineCaches, worker_id,
                                stats._unload_inline_caches_ticks,
                                stats._inline_cache_relocations,
                                stats._inline_cache_cleaned_calls);
  record_nmethod_unloading_time(NMethodInlineCacheFilter, worker_id,
                                stats._inline_cache_filter_ticks,
                                stats._inline_cache_checked_nmethods,
                                stats._inline_cache_skipped_nmethods);
  record_nmethod_unloading_time(NMethodInlineCacheCleanMetadata, worker_id,
                                stats._inline_cache_clean_metadata_ticks,
                                stats._inline_cache_clean_metadata_calls);
  record_nmethod_unloading_time(NMethodInlineCacheCleanNMethod, worker_id,
                                stats._inline_cache_clean_nmethod_ticks,
                                stats._inline_cache_nmethod_checks,
                                stats._inline_cache_cleaned_calls);
  record_nmethod_unloading_time(NMethodInlineCacheNMethodLookup, worker_id,
                                stats._inline_cache_nmethod_lookup_ticks,
                                stats._inline_cache_nmethod_lookups,
                                stats._inline_cache_clean_call_skips + stats._inline_cache_stub_call_skips);
  record_nmethod_unloading_time(NMethodInlineCacheNMethodState, worker_id,
                                stats._inline_cache_nmethod_state_ticks,
                                stats._inline_cache_nmethod_state_checks,
                                stats._inline_cache_cleaned_calls);
  record_nmethod_unloading_time(NMethodInlineCacheIsInUse, worker_id,
                                stats._inline_cache_is_in_use_ticks,
                                stats._inline_cache_is_in_use_checks,
                                stats._inline_cache_not_in_use);
  record_nmethod_unloading_time(NMethodInlineCacheIsUnloading, worker_id,
                                stats._inline_cache_is_unloading_ticks,
                                stats._inline_cache_is_unloading_checks,
                                stats._inline_cache_unloading);
  record_nmethod_unloading_time(NMethodInlineCacheMethodCode, worker_id,
                                stats._inline_cache_method_code_ticks,
                                stats._inline_cache_method_code_checks,
                                stats._inline_cache_method_code_skips);
  record_nmethod_unloading_time(NMethodInlineCacheMetadataReloc, worker_id,
                                stats._inline_cache_metadata_reloc_ticks,
                                stats._inline_cache_metadata_relocations,
                                stats._inline_cache_metadata_cleared);
  record_nmethod_unloading_time(NMethodUnloadVerifyMetadata, worker_id,
                                stats._unload_verify_metadata_ticks,
                                stats._verify_metadata_nmethods);
  record_nmethod_unloading_time(NMethodDisarm, worker_id,
                                stats._disarm_ticks, stats._disarmed_nmethods);
}

void G1ParallelCleaningTask::log_phase(G1CleaningSubPhase phase, uint indent_level, outputStream* out) const {
  WorkerDataArray<double>* worker_time = _worker_times[phase];

  out->sp(indent_level * 2);
  worker_time->print_summary_on(out, true);

  LogTarget(Trace, gc, phases, task) trace;
  if (trace.is_enabled()) {
    LogStream ls(trace);
    ls.sp(indent_level * 2);
    worker_time->print_details_on(&ls);
  }

  for (uint i = 0; i < G1CleaningWorkItemCount; i++) {
    WorkerDataArray<size_t>* work_items = worker_time->thread_work_items(i);
    if (work_items != nullptr) {
      out->sp((indent_level + 1) * 2);
      work_items->print_summary_on(out, true);

      if (trace.is_enabled()) {
        LogStream ls(trace);
        ls.sp((indent_level + 1) * 2);
        work_items->print_details_on(&ls);
      }
    }
  }
}

void G1ParallelCleaningTask::log_nmethod_unloading_stats(uint indent_level, outputStream* out) const {
  LogTarget(Trace, gc, phases, task) trace;
  for (uint phase = 0; phase < NMethodUnloadingSubPhaseCount; phase++) {
    WorkerDataArray<double>* worker_time = _nmethod_unloading_times[phase];

    out->sp(indent_level * 2);
    worker_time->print_summary_on(out, true);

    if (trace.is_enabled()) {
      LogStream ls(trace);
      ls.sp(indent_level * 2);
      worker_time->print_details_on(&ls);
    }

    for (uint i = 0; i < G1CleaningWorkItemCount; i++) {
      WorkerDataArray<size_t>* work_items = worker_time->thread_work_items(i);
      if (work_items != nullptr) {
        out->sp((indent_level + 1) * 2);
        work_items->print_summary_on(out, true);

        if (trace.is_enabled()) {
          LogStream ls(trace);
          ls.sp((indent_level + 1) * 2);
          work_items->print_details_on(&ls);
        }
      }
    }
  }
}

void G1ParallelCleaningTask::log_slowest_dependency_context(uint indent_level, outputStream* out) const {
  uint max_worker_id = 0;
  jlong max_ticks = 0;
  for (uint i = 0; i < _num_workers; i++) {
    if (_max_dependency_context_ticks[i] > max_ticks) {
      max_worker_id = i;
      max_ticks = _max_dependency_context_ticks[i];
    }
  }

  const char* owner = "<unknown>";
  ResourceMark rm;
  if (max_ticks > 0) {
    if (_max_dependency_context_is_call_site[max_worker_id]) {
      owner = "CallSite";
    } else if (_max_dependency_context_klasses[max_worker_id] != nullptr) {
      owner = _max_dependency_context_klasses[max_worker_id]->external_name();
    }

    out->sp(indent_level * 2);
    out->print_cr("Slowest Dependency Context:    Worker: %u, Time: %.2fms, Buckets: %zu, Owner: %s",
                  max_worker_id,
                  TimeHelper::counter_to_millis(max_ticks),
                  _max_dependency_context_buckets[max_worker_id],
                  owner);
  }

  max_worker_id = 0;
  max_ticks = 0;
  for (uint i = 0; i < _num_workers; i++) {
    if (_max_dependency_context_remove_ticks[i] > max_ticks) {
      max_worker_id = i;
      max_ticks = _max_dependency_context_remove_ticks[i];
    }
  }

  if (max_ticks == 0) {
    return;
  }

  owner = "<unknown>";
  if (_max_dependency_context_remove_is_call_site[max_worker_id]) {
    owner = "CallSite";
  } else if (_max_dependency_context_remove_klasses[max_worker_id] != nullptr) {
    owner = _max_dependency_context_remove_klasses[max_worker_id]->external_name();
  }

  out->sp(indent_level * 2);
  out->print_cr("Slowest Dependency Remove:     Worker: %u, Time: %.2fms, Buckets: %zu, Owner: %s",
                max_worker_id,
                TimeHelper::counter_to_millis(max_ticks),
                _max_dependency_context_remove_buckets[max_worker_id],
                owner);
}

void G1ParallelCleaningTask::log_statistics() const {
  LogTarget(Trace, gc, phases, task) trace;
  if (!trace.is_enabled()) {
    return;
  }

  LogStream ls(trace);
  log_phase(JVMCIHandles, 2, &ls);
  log_phase(CodeCacheUnloading, 2, &ls);
  log_phase(KlassCleaning, 2, &ls);
  log_nmethod_unloading_stats(3, &ls);
  log_slowest_dependency_context(3, &ls);
}

// The parallel work done by all worker threads.
void G1ParallelCleaningTask::work(uint worker_id) {
  const bool record_stats = _record_stats;

  // Clean JVMCI metadata handles.
  // Execute this task first because it is serial task.
  JVMCI_ONLY({
    jlong start = record_stats ? os::elapsed_counter() : 0;
    bool did_work = false;
    size_t num_processed_handles = _jvmci_cleaning_task.work(_unloading_occurred, &did_work);
    if (record_stats && did_work) {
      record_worker_time(JVMCIHandles, worker_id, start, num_processed_handles);
    }
  })

  // Do first pass of code cache cleaning.
  {
    jlong start = record_stats ? os::elapsed_counter() : 0;
    if (record_stats) {
      NMethodUnloadingStats stats;
      size_t num_processed_nmethods = _code_cache_task.work(worker_id, &stats);
      record_worker_time(CodeCacheUnloading, worker_id, start, num_processed_nmethods);
      record_nmethod_unloading_stats(worker_id, stats);
    } else {
      _code_cache_task.work(worker_id, nullptr);
    }
  }

  // Clean all klasses that were not unloaded.
  // The weak metadata in klass doesn't need to be
  // processed if there was no unloading.
  if (_unloading_occurred) {
    jlong start = record_stats ? os::elapsed_counter() : 0;
    size_t num_processed_clds = 0;
    size_t* num_processed_clds_ptr = record_stats ? &num_processed_clds : nullptr;
    size_t num_processed_klasses = _klass_cleaning_task.work(num_processed_clds_ptr);
    if (record_stats) {
      record_worker_time(KlassCleaning, worker_id, start, num_processed_clds, num_processed_klasses);
    }
  }
}
