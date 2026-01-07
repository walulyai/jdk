/*
 * Copyright (c) 2016, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentMark.inline.hpp"
#include "gc/g1/g1ConcurrentMarkObjArrayProcessor.inline.hpp"
#include "gc/g1/g1HeapRegion.inline.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/partialArraySplitter.inline.hpp"
#include "gc/shared/partialArrayState.hpp"
#include "memory/memRegion.hpp"
#include "utilities/globalDefinitions.hpp"

size_t G1CMObjArrayProcessor::scan_array(objArrayOop obj, MemRegion mr) {
  return _task->scan_objArray(obj, mr);
}

size_t G1CMObjArrayProcessor::scan_array(oop obj) {
  assert(should_be_sliced(obj), "Must be an array object %d and large %zu", obj->is_objArray(), obj->size());
  size_t obj_size_in_words = obj->size();
  objArrayOop obj_array = objArrayOop(obj);
  size_t initial_chunk_size = _task->partial_array_splitter()->start(_task->task_queue(), obj_array, nullptr, obj_size_in_words);

  HeapWord* start = cast_from_oop<HeapWord*>(obj);
  MemRegion mr(start, initial_chunk_size);
  return scan_array(obj_array, mr);
}

size_t G1CMObjArrayProcessor::scan_partial_array(const G1TaskQueueEntry& task, bool stolen) {
  PartialArrayState* state = task.to_partial_array_state();
  // Access state before release by claim().
  objArrayOop obj = objArrayOop(state->source());
  PartialArraySplitter::Claim claim =
    _task->partial_array_splitter()->claim(state, _task->task_queue(), stolen);
    // _partial_array_splitter.claim(state, _task->task_queue(), stolen);

  HeapWord* base = cast_from_oop<HeapWord*>(obj);

  HeapWord* start = base + claim._start;
  HeapWord* end = base + claim._end;

  MemRegion mr(start, end);
  return scan_array(obj, mr);
}
