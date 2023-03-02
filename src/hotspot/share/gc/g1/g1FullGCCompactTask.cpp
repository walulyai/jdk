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
  if (obj->is_forwarded()) {
    G1FullGCCompactTask::copy_object_to_new_location(obj);
  }

  // Clear the mark for the compacted object to allow reuse of the
  // bitmap without an additional clearing step.
  clear_in_bitmap(obj);
  return obj->size();
}

void G1FullGCCompactTask::copy_object_to_new_location(oop obj) {
  assert(obj->is_forwarded(), "Sanity!");
  assert(obj->forwardee() != obj, "Object must have a new location");

  // copy object and reinit its mark
  HeapWord* src_addr = cast_from_oop<HeapWord*>(obj);
  HeapWord* destination = cast_from_oop<HeapWord*>(obj->forwardee());
  size_t size = obj->size();
  Copy::aligned_conjoint_words(src_addr, destination, size);

  // There is no need to transform stack chunks - marking already did that.
  cast_to_oop(destination)->init_mark();
  assert(cast_to_oop(destination)->klass() != nullptr, "should have a class");
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

void G1FullGCCompactTask::work(uint worker_id) {
  Ticks start = Ticks::now();
  GrowableArray<HeapRegion*>* compaction_queue = collector()->compaction_point(worker_id)->regions();
  for (GrowableArrayIterator<HeapRegion*> it = compaction_queue->begin();
       it != compaction_queue->end();
       ++it) {
    compact_region(*it);
  }

  G1ResetSkipCompactingClosure hc(collector());
  _g1h->heap_region_par_iterate_from_worker_offset(&hc, &_claimer, worker_id);
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

  for (HeapRegion* hr : *(collector()->humongous_compaction_regions())) {
    assert(collector()->is_compaction_target(hr->hrm_index()), "Sanity");
    compact_humongous_obj(hr);
  }
}

void G1FullGCCompactTask::reset_humongous_metadata(HeapRegion* start_hr, uint num_regions, size_t word_size) {
  // Calculate the new top of the humongous object
  HeapWord* dest_top = start_hr->bottom() + word_size;
  // The word size sum of all the regions used
  size_t word_size_sum = num_regions * HeapRegion::GrainWords;
  assert(word_size <= word_size_sum, "sanity");

  // How many words memory we "waste" which cannot hold a filler object.
  size_t words_not_fillable = 0;
  // Next, pad out the unused tail of the last region with filler
  // objects, for improved usage accounting.

  // How many words can we use for filler objects.
  size_t words_fillable = word_size_sum - word_size;

  if (words_fillable >= G1CollectedHeap::min_fill_size()) {
    G1CollectedHeap::fill_with_objects(dest_top, words_fillable);
  } else {
    // We have space to fill, but we cannot fit an object there.
    words_not_fillable = words_fillable;
    words_fillable = 0;
  }

  // Set up the first region as "starts humongous". This will also update
  // the BOT covering all the regions to reflect that there is a single
  // object that starts at the bottom of the first region.

  start_hr->set_free(); // Avoid triggering asserts when changing region type
  start_hr->set_top(start_hr->bottom());
  start_hr->set_starts_humongous(dest_top, words_fillable);
  start_hr->reset_compacted_after_full_gc(start_hr->end());

  uint start_idx = start_hr->hrm_index();
  uint end_idx   = start_idx + num_regions - 1;

  // If there are any, we set up the "continues humongous" regions.
  for (uint i = start_idx + 1; i <= end_idx; i++) {
    HeapRegion* hr = _g1h->region_at(i);
    hr->set_free();
    hr->set_top(hr->bottom());
    hr->set_continues_humongous(start_hr);
    hr->reset_compacted_after_full_gc(hr->end());
  }

  // If we cannot fit a filler object, we must set top to the end
  // of the humongous object, otherwise we cannot iterate the heap
  // and the BOT will not be complete.
  HeapRegion* end_hr = _g1h->region_at(end_idx);
  end_hr->set_top(end_hr->end() - words_not_fillable);
}

void G1FullGCCompactTask::compact_humongous_obj(HeapRegion* src_hr) {
  assert(src_hr->is_starts_humongous(), "Should be start region of the humongous object");

  oop obj = cast_to_oop(src_hr->bottom());
  size_t word_size = obj->size();

  uint num_regions = (uint) G1CollectedHeap::humongous_obj_size_in_regions(word_size);
  HeapWord* destination = cast_from_oop<HeapWord*>(obj->forwardee());

  assert(collector()->mark_bitmap()->is_marked(obj), "Should only compact marked objects");
  collector()->mark_bitmap()->clear(obj);

  copy_object_to_new_location(obj);

  uint dest_start_idx = _g1h->addr_to_region(destination);
  // Update the metadata for the destination regions.
  reset_humongous_metadata(_g1h->region_at(dest_start_idx), num_regions, word_size);

  // Free the source regions that do not overlap with the destination regions.
  uint src_start_idx = src_hr->hrm_index();
  free_non_overlapping_regions(src_start_idx, dest_start_idx, num_regions);
}

void G1FullGCCompactTask::free_non_overlapping_regions(uint src_start_idx, uint dest_start_idx, uint num_regions) {
  uint dest_end_idx = dest_start_idx + num_regions -1;
  uint src_end_idx  = src_start_idx + num_regions - 1;

  uint non_overlapping_start = dest_end_idx < src_start_idx ?
                               src_start_idx :
                               dest_end_idx + 1;

  for (uint i = non_overlapping_start; i <= src_end_idx; ++i) {
    HeapRegion* hr = _g1h->region_at(i);
    _g1h->free_humongous_region(hr, nullptr);
  }
}
