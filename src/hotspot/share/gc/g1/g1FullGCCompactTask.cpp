/*
 * Copyright (c) 2017, 2022, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1ConcurrentMarkBitMap.inline.hpp"
#include "gc/g1/g1FullCollector.inline.hpp"
#include "gc/g1/g1FullGCCompactionPoint.hpp"
#include "gc/g1/g1FullGCCompactTask.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "logging/log.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/ticks.hpp"

// Do work for all skip-compacting regions.
class G1ResetSkipCompactingClosure : public HeapRegionClosure {
  G1FullCollector* _collector;

public:
  G1ResetSkipCompactingClosure(G1FullCollector* collector) : _collector(collector) { }

  bool do_heap_region(HeapRegion* r) {
    uint region_index = r->hrm_index();
    // Only for skip-compaction regions; early return otherwise.
    if (!_collector->is_skip_compacting(region_index)) {
      return false;
    }
#ifdef ASSERT
    if (r->is_humongous()) {
      oop obj = cast_to_oop(r->humongous_start_region()->bottom());
      assert(_collector->mark_bitmap()->is_marked(obj), "must be live");
    } else if (r->is_open_archive()) {
      bool is_empty = (_collector->live_words(r->hrm_index()) == 0);
      assert(!is_empty, "should contain at least one live obj");
    } else if (r->is_closed_archive()) {
      // should early-return above
      ShouldNotReachHere();
    } else {
      assert(_collector->live_words(region_index) > _collector->scope()->region_compaction_threshold(),
             "should be quite full");
    }
#endif
    assert(_collector->compaction_top(r) == nullptr,
           "region %u compaction_top " PTR_FORMAT " must not be different from bottom " PTR_FORMAT,
           r->hrm_index(), p2i(_collector->compaction_top(r)), p2i(r->bottom()));

    r->reset_skip_compacting_after_full_gc();
    return false;
  }
};

void G1FullGCCompactTask::G1CompactRegionClosure::clear_in_bitmap(oop obj) {
  assert(_bitmap->is_marked(obj), "Should only compact marked objects");
  _bitmap->clear(obj);
}

size_t G1FullGCCompactTask::G1CompactRegionClosure::apply(oop obj) {
  size_t size = obj->size();
  if (obj->is_forwarded()) {
    HeapWord* destination = cast_from_oop<HeapWord*>(obj->forwardee());

    // copy object and reinit its mark
    HeapWord* obj_addr = cast_from_oop<HeapWord*>(obj);
    assert(obj_addr != destination, "everything in this pass should be moving");
    Copy::aligned_conjoint_words(obj_addr, destination, size);

    // There is no need to transform stack chunks - marking already did that.
    cast_to_oop(destination)->init_mark();
    assert(cast_to_oop(destination)->klass() != NULL, "should have a class");
  }

  // Clear the mark for the compacted object to allow reuse of the
  // bitmap without an additional clearing step.
  clear_in_bitmap(obj);
  return size;
}

void G1FullGCCompactTask::compact_region(HeapRegion* hr) {
  assert(!hr->is_pinned(), "Should be no pinned region in compaction queue");
  assert(!hr->is_humongous(), "Should be no humongous regions in compaction queue");

  if (!collector()->is_free(hr->hrm_index())) {
    // The compaction closure not only copies the object to the new
    // location, but also clears the bitmap for it. This is needed
    // for bitmap verification and to be able to use the bitmap
    // for evacuation failures in the next young collection. Testing
    // showed that it was better overall to clear bit by bit, compared
    // to clearing the whole region at the end. This difference was
    // clearly seen for regions with few marks.
    G1CompactRegionClosure compact(collector()->mark_bitmap());
    hr->apply_to_marked_objects(collector()->mark_bitmap(), &compact);
  }

  hr->reset_compacted_after_full_gc(_collector->compaction_top(hr));
}


void G1FullGCCompactTask::compact_humongous(HeapRegion* start) {
  assert(start->is_starts_humongous(), "Should be start region of the humongous object");

  oop obj = cast_to_oop(start->bottom());
  HeapWord* destination = cast_from_oop<HeapWord*>(obj->forwardee());
  size_t word_size = obj->size();
  uint num_regions = (uint) G1CollectedHeap::humongous_obj_size_in_regions(word_size);

  G1CollectedHeap* g1h = G1CollectedHeap::heap();

  uint dest_start = g1h->addr_to_region(destination);
  uint dest_end   = dest_start + num_regions - 1;
  uint src_start   = start->hrm_index();
  assert(dest_start < src_start, "Must be!");

  log_debug(gc, region) ("Moving region: fro %u to %u - %u num_regions %u",
                         src_start, dest_start, dest_end, num_regions);

  HeapRegion* dest_start_hr =  g1h->region_at(dest_start);
  dest_start_hr->set_top(dest_start_hr->bottom());
  // FIXME: clean up below
  /*if (dest_start_hr->is_humongous()) {
    g1h->free_humongous_region(dest_start_hr, nullptr);
  } else if (dest_start_hr->is_free() && !dest_start_hr->is_empty()){
    dest_start_hr->hr_clear(true);
  } else if (!dest_start_hr->is_free()) {
    g1h->free_region(dest_start_hr, nullptr);
  }
  */

  // copy object and reinit its mark
  HeapWord* obj_src = cast_from_oop<HeapWord*>(obj);
  assert(obj_src == start->bottom(), "Must be!");
  Copy::aligned_conjoint_words(obj_src, destination, word_size);
  cast_to_oop(destination)->init_mark();
  assert(cast_to_oop(destination)->klass() != NULL, "should have a class");

  // Clear the mark for the compacted object to allow reuse of the
  // bitmap without an additional clearing step.
  //clear_in_bitmap(obj);
  assert(collector()->mark_bitmap()->is_marked(obj), "Should only compact marked objects");
  collector()->mark_bitmap()->clear(obj);

  // This will be the new top of the new object.
  HeapWord* dest_top = destination + word_size;
  // The word size sum of all the regions used
  size_t word_size_sum = (size_t) num_regions * HeapRegion::GrainWords;
  assert(word_size <= word_size_sum, "sanity");

  // How many words memory we "waste" which cannot hold a filler object.
  size_t words_not_fillable = 0;

  // Next, pad out the unused tail of the last region with filler
  // objects, for improved usage accounting.
  // How many words we use for filler objects.
  size_t word_fill_size = word_size_sum - word_size;
  if (word_fill_size >= G1CollectedHeap::min_fill_size()) {
    G1CollectedHeap::fill_with_objects(dest_top, word_fill_size);
  } else {
    // We have space to fill, but we cannot fit an object there.
    words_not_fillable = word_fill_size;
    word_fill_size = 0;
  }

  // We will set up the first region as "starts humongous". This
  // will also update the BOT covering all the regions to reflect
  // that there is a single object that starts at the bottom of the
  // first region.
  dest_start_hr->set_free(); // FIXME: doing it here to avoid asserts
  dest_start_hr->set_starts_humongous(dest_top, word_fill_size);

  dest_start_hr->reset_compacted_humongous_after_full_gc(dest_start_hr->end());
  // Then, if there are any, we will set up the "continues
  // humongous" regions.
  for (uint i = dest_start + 1; i <= dest_end; i++) {
    HeapRegion* c_hr = g1h->region_at(i);
    // FIXME: doing it here to avoid asserts
    c_hr->change_continues_humongous(dest_start_hr);
    c_hr->reset_compacted_humongous_after_full_gc(c_hr->end());
  }

  assert(dest_start_hr->top() == dest_start_hr->end() || num_regions == 1 , "Must be!");
  assert(dest_start_hr->bottom() < dest_start_hr->top(), "Must be!");

  HeapRegion* dest_end_hr = g1h->region_at(dest_end);
  // If we cannot fit a filler object, we must set top to the end
  // of the humongous object, otherwise we cannot iterate the heap
  // and the BOT will not be complete.
  dest_end_hr->set_top(dest_end_hr->end() - words_not_fillable);

  assert(dest_end_hr->bottom() < dest_top && dest_top <= dest_end_hr->end(), "Object top should be in last region");

  assert(words_not_fillable == 0 || dest_start_hr->bottom() + word_size_sum - words_not_fillable == dest_end_hr->top(),
         "Miscalculation in humongous allocation");

  // FIXME: what do we do about the regions we moved from????
  uint src_end = src_start + num_regions - 1;
  uint non_overlapping_start = (dest_end < src_start) ? src_start : dest_end + 1;
  for (uint i = non_overlapping_start; i <= src_end; ++i) {
    HeapRegion* c_hr = g1h->region_at(i);
    g1h->free_humongous_region(c_hr, nullptr);
  }
}

void G1FullGCCompactTask::work(uint worker_id) {
  Ticks start = Ticks::now();
  GrowableArray<HeapRegion*>* compaction_queue = collector()->compaction_point(worker_id)->regions();
  for (GrowableArrayIterator<HeapRegion*> it = compaction_queue->begin();
       it != compaction_queue->end();
       ++it) {
    compact_region(*it);
  }

  G1ResetSkipCompactingClosure hc(collector());
  G1CollectedHeap::heap()->heap_region_par_iterate_from_worker_offset(&hc, &_claimer, worker_id);
  log_task("Compaction task", worker_id, start);
}

void G1FullGCCompactTask::serial_compaction() {
  GCTraceTime(Debug, gc, phases) tm("Phase 4: Serial Compaction", collector()->scope()->timer());
  GrowableArray<HeapRegion*>* compaction_queue = collector()->serial_compaction_point()->regions();
  for (GrowableArrayIterator<HeapRegion*> it = compaction_queue->begin();
       it != compaction_queue->end();
       ++it) {
    compact_region(*it);
  }
}

void G1FullGCCompactTask::humongous_compaction() {
  GCTraceTime(Debug, gc, phases) tm("Phase 4: Humonguous Compaction", collector()->scope()->timer());
  GrowableArray<HeapRegion*>* target_regions = collector()->humongous_start_regions();

  for (GrowableArrayIterator<HeapRegion*> it = target_regions->begin();
       it != target_regions->end();
       ++it) {
    HeapRegion* hr = *it;
    if (!collector()->is_skip_compacting(hr->hrm_index())) {
      compact_humongous(hr);
    }
  }
}
