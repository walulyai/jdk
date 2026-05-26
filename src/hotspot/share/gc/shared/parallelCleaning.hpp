/*
 * Copyright (c) 2018, 2026, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHARED_PARALLELCLEANING_HPP
#define SHARE_GC_SHARED_PARALLELCLEANING_HPP

#include "classfile/classLoaderDataGraph.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/oopStorageParState.hpp"
#include "gc/shared/workerThread.hpp"
#include "gc/shared/workerUtils.hpp"
#include "runtime/atomic.hpp"

class CodeCacheUnloadingTask {

  const bool                _unloading_occurred;

  // Variables used to claim nmethods.
  nmethod* _first_nmethod;
  Atomic<nmethod*> _claimed_nmethod;
  WorkerThreadsBarrierSync _decide_barrier;
  WorkerThreadsBarrierSync _cleanup_barrier;

public:
  CodeCacheUnloadingTask(bool unloading_occurred, uint num_workers);
  ~CodeCacheUnloadingTask();

private:
  static const int MaxClaimNmethods = 16;
  void reset_claim_nmethods();
  void claim_nmethods(nmethod** claimed_nmethods, int *num_claimed_nmethods);
  size_t work_unloading_decide(uint worker_id, NMethodUnloadingStats* stats);
  void work_unloading_cleanup(uint worker_id, NMethodUnloadingStats* stats);

public:
  // Cleaning and unloading of nmethods.
  size_t work(uint worker_id, NMethodUnloadingStats* stats = nullptr);
};

// Cleans out the Klass tree from stale data.
class KlassCleaningTask : public StackObj {
  ClassLoaderDataGraphIteratorAtomic _cld_iterator_atomic;

public:
  KlassCleaningTask() : _cld_iterator_atomic() { }

  size_t work(size_t* num_class_loader_data = nullptr);
};

#endif // SHARE_GC_SHARED_PARALLELCLEANING_HPP
