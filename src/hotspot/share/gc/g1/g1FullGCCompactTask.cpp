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
  assert(!hr->is_humongous(), "Should be no humongous regions in compaction queue");
  assert(!hr->is_pinned(), "Should be no pinned region in compaction queue");
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
  GCTraceTime(Debug, gc, phases) tm("Phase 4: Humongous Compaction", collector()->scope()->timer());
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  const uint num_regions = g1h->num_regions();

  for (uint idx = 0; idx < num_regions; ++idx) {
    HeapRegion* hr = g1h->region_at(idx);

    if (hr->is_starts_humongous()) {
      oop obj = cast_to_oop(hr->bottom());
      HeapWord* destination = cast_from_oop<HeapWord*>(obj->forwardee());

      size_t word_size = obj->size();
      uint obj_regions = (uint) G1CollectedHeap::humongous_obj_size_in_regions(word_size);

      if (destination == NULL) {
        // Object not moving
        //FIXME: can skip over the regions covered by this object.
        idx += (obj_regions - 1);
        continue;
      }

      // Index of last region in the series.
      uint old_first = hr->hrm_index();
      uint old_last = old_first + obj_regions - 1;

      uint new_first = g1h->addr_to_region(destination);
      assert(new_first < old_first,  "must be!");
      uint new_last = new_first + obj_regions - 1;

      assert(old_first != new_first, "sanity");

      // The word size sum of all the regions used
      size_t word_size_sum = (size_t) obj_regions * HeapRegion::GrainWords;
      assert(word_size <= word_size_sum, "sanity");

      HeapRegion* first_hr =  g1h->region_at(new_first);
      assert(!first_hr->is_humongous(), "sanity / pre-condition %s %d idx %d", first_hr->get_type_str(), first_hr->is_humongous(), new_first);
      assert(!first_hr->is_pinned(), "Should not be a pinned region");

      // This will be the new top of the new object.
      HeapWord* new_obj_top = destination + word_size;

      HeapWord* old_obj = g1h->region_at(old_first)->bottom();

      collector()->mark_bitmap()->clear(old_obj);

      // Copy object. Use conjoint copying the new object
      // may overlap with the old object.
      Copy::aligned_conjoint_words(old_obj, destination, word_size);
      assert(destination == first_hr->bottom(), "sanity");
      oop new_obj = cast_to_oop(destination);
      new_obj->init_mark();
      assert(new_obj->klass() != NULL, "should have a class");

      // FIXME: what do we do about the regions we moved from????
      uint non_overlapping_start = (new_last < old_first) ? old_first : new_last + 1;
      for (uint i = non_overlapping_start; i <= old_last; ++i) {
        HeapRegion* hr = g1h->region_at(i);
        //FIXME: collector()->mark_bitmap()->clear_region(hr);
        g1h->free_humongous_region(hr, nullptr);
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
      first_hr->set_free();
      first_hr->set_starts_humongous(new_obj_top, word_fill_size);
      g1h->policy()->remset_tracker()->update_at_allocate(first_hr);

      

      // Then, if there are any, we will set up the "continues
      // humongous" regions.
      for (uint i = new_first + 1; i <= new_last; ++i) {
        HeapRegion* hr = g1h->region_at(i);
        hr->change_continues_humongous(first_hr);
        g1h->policy()->remset_tracker()->update_at_allocate(hr);
      }

    // Now, we will update the top fields of the "continues humongous"
    // regions except the last one.
      for (uint i = new_first; i <= new_last; ++i) {
        hr = g1h->region_at(i);
        hr->set_top(hr->end());
      }

      log_error(gc) ("Move region: from %d to %d num_regions: %d addr: %p block_start: %p ", old_first, new_first, obj_regions, first_hr, first_hr->block_start(destination));
      assert(first_hr->top() == first_hr->end(), "must be!");
      assert(first_hr->bottom() < first_hr->top(), "must be!");
      assert(!collector()->is_skip_compacting(new_first), "must not be");

      HeapRegion* last_hr = g1h->region_at(new_last);
      // last_hr->set_continues_humongous(first_hr);
      // If we cannot fit a filler object, we must set top to the end
      // of the humongous object, otherwise we cannot iterate the heap
      // and the BOT will not be complete.
      last_hr->set_top(last_hr->end() - words_not_fillable);

      assert(last_hr->bottom() < new_obj_top && new_obj_top <= last_hr->end(),
             "obj_top should be in last region");

      assert(words_not_fillable == 0 ||
             first_hr->bottom() + word_size_sum - words_not_fillable == last_hr->top(),
             "Miscalculation in humongous allocation");
      
      // Skip forward to to last region in the humongous object
      idx += (obj_regions - 1);
    }
  }

}
