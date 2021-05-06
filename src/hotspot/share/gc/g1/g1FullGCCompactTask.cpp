/*
 * Copyright (c) 2017, 2021, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1FullCollector.hpp"
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
    assert(_collector->live_words(region_index) > _collector->scope()->region_compaction_threshold() ||
         !r->is_starts_humongous() ||
         _collector->mark_bitmap()->is_marked(cast_to_oop(r->bottom())),
         "must be, otherwise reclaimed earlier");
    r->reset_skip_compacting_after_full_gc();
    return false;
  }
};

size_t G1FullGCCompactTask::G1CompactRegionClosure::apply(oop obj) {
  size_t size = obj->size();
  HeapWord* destination = cast_from_oop<HeapWord*>(obj->forwardee());
  if (destination == NULL) {
    // Object not moving
    return size;
  }

  // copy object and reinit its mark
  HeapWord* obj_addr = cast_from_oop<HeapWord*>(obj);
  assert(obj_addr != destination, "everything in this pass should be moving");
  Copy::aligned_conjoint_words(obj_addr, destination, size);
  cast_to_oop(destination)->init_mark();
  assert(cast_to_oop(destination)->klass() != NULL, "should have a class");

  return size;
}

void G1FullGCCompactTask::compact_region(HeapRegion* hr) {
  assert(!hr->is_pinned(), "Should be no pinned region in compaction queue");
  assert(!hr->is_humongous(), "Should be no humongous regions in compaction queue");
  G1CompactRegionClosure compact(collector()->mark_bitmap());
  hr->apply_to_marked_objects(collector()->mark_bitmap(), &compact);
  // Clear the liveness information for this region if necessary i.e. if we actually look at it
  // for bitmap verification. Otherwise it is sufficient that we move the TAMS to bottom().
  if (G1VerifyBitmaps) {
    collector()->mark_bitmap()->clear_region(hr);
  }
  hr->reset_compacted_after_full_gc();
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
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  const uint num_regions = g1h->num_regions();

  for(uint idx = num_regions; idx > 0; --idx) {
    HeapRegion* hr = g1h->region_at(idx - 1);

    if (hr->is_starts_humongous()) {
      oop obj = cast_to_oop(hr->bottom());
      HeapWord* destination = cast_from_oop<HeapWord*>(obj->forwardee());
      if (destination == NULL) {
        // Object not moving
        continue;
      }
      size_t word_size = obj->size();
      uint obj_regions = (uint) G1CollectedHeap::humongous_obj_size_in_regions(word_size);
      log_error(gc) ("Object %d is moving |  object size: %zu num_regions: %d", hr->hrm_index(), word_size, obj_regions);

      // Index of last region in the series.
      uint old_first = hr->hrm_index();
      uint old_last = old_first + obj_regions - 1;
      
      uint new_first = g1h->addr_to_region(destination);
      assert(new_first < num_regions,  "must be!");
      uint new_last = new_first + obj_regions - 1;

      assert(old_first != new_first, "sanity");

      // The word size sum of all the regions used
      size_t word_size_sum = (size_t) obj_regions * HeapRegion::GrainWords;
      assert(word_size <= word_size_sum, "sanity");

      HeapRegion* first_hr =  g1h->region_at(new_first);
      //HeapWord* new_obj  = first_hr->bottom();
      // This will be the new top of the new object.
      HeapWord* new_obj_top = destination + word_size;

      HeapWord* old_obj = g1h->region_at(old_first)->bottom();

      // Copy object. Use conjoint copying the new object 
      // may overlap with the old object.
      Copy::aligned_conjoint_words(old_obj, destination, word_size);
      assert(destination == g1h->region_at(new_first)->bottom(), "sanity");
      oop new_obj = cast_to_oop(destination);
      new_obj->init_mark();
      assert(new_obj->klass() != NULL, "should have a class");


      for (uint i = old_first; i <= old_last; ++i) {
        HeapRegion* hr = g1h->region_at(i);
        // Clear the liveness information for this region if necessary i.e. if we actually look at it
        // for bitmap verification. Otherwise it is sufficient that we move the TAMS to bottom().
        if (G1VerifyBitmaps) {
          collector()->mark_bitmap()->clear_region(hr);
        }
        hr->set_free();
        hr->reset_compacted_after_full_gc();
      }

      // Next, pad out the unused tail of the last region with filler
      // objects, for improved usage accounting.
      // How many words we use for filler objects.
      size_t word_fill_size = word_size_sum - word_size;

      // How many words memory we "waste" which cannot hold a filler object.
      size_t words_not_fillable = 0;

      if (word_fill_size >= G1CollectedHeap::min_fill_size()) {
        G1CollectedHeap::fill_with_objects(new_obj_top, word_fill_size);
      } else if (word_fill_size > 0) {
        // We have space to fill, but we cannot fit an object there.
        words_not_fillable = word_fill_size;
        word_fill_size = 0;
      }
      // We will set up the first region as "starts humongous". This
      // will also update the BOT covering all the regions to reflect
      // that there is a single object that starts at the bottom of the
      // first region.
      first_hr->set_starts_humongous(new_obj_top, word_fill_size);

      for (uint i = new_first + 1; i < new_last; ++i) {
        HeapRegion* hr = g1h->region_at(i);
        hr->set_continues_humongous(first_hr);
        hr->set_top(hr->end());
      }

      HeapRegion* last_hr = g1h->region_at(new_last);
      last_hr->set_continues_humongous(first_hr);
      // If we cannot fit a filler object, we must set top to the end
      // of the humongous object, otherwise we cannot iterate the heap
      // and the BOT will not be complete.
      last_hr->set_top(last_hr->end() - words_not_fillable);
      // cast_to_oop(destination)->init_mark();
      assert(words_not_fillable == 0 ||
             first_hr->bottom() + word_size_sum - words_not_fillable == last_hr->top(),
             "Miscalculation in humongous allocation");
      /*
      for (uint i = first; i <= last; ++i) {
        hr = region_at(i);
        _humongous_set.add(hr);
        _hr_printer.alloc(hr);
      }
      */

    }
  }
}