/*
 * Copyright (c) 2001, 2022, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_HEAPREGION_INLINE_HPP
#define SHARE_GC_G1_HEAPREGION_INLINE_HPP

#include "gc/g1/heapRegion.hpp"

#include "gc/g1/g1BlockOffsetTable.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentMarkBitMap.inline.hpp"
#include "gc/g1/g1Predictions.hpp"
#include "gc/g1/g1SegmentedArray.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/prefetch.inline.hpp"
#include "utilities/align.hpp"
#include "utilities/globalDefinitions.hpp"

inline HeapWord* HeapRegion::allocate_impl(size_t min_word_size,
                                           size_t desired_word_size,
                                           size_t* actual_size) {
  HeapWord* obj = top();
  size_t available = pointer_delta(end(), obj);
  size_t want_to_allocate = MIN2(available, desired_word_size);
  if (want_to_allocate >= min_word_size) {
    HeapWord* new_top = obj + want_to_allocate;
    set_top(new_top);
    assert(is_object_aligned(obj) && is_object_aligned(new_top), "checking alignment");
    *actual_size = want_to_allocate;
    return obj;
  } else {
    return NULL;
  }
}

inline HeapWord* HeapRegion::par_allocate_impl(size_t min_word_size,
                                               size_t desired_word_size,
                                               size_t* actual_size) {
  do {
    HeapWord* obj = top();
    size_t available = pointer_delta(end(), obj);
    size_t want_to_allocate = MIN2(available, desired_word_size);
    if (want_to_allocate >= min_word_size) {
      HeapWord* new_top = obj + want_to_allocate;
      HeapWord* result = Atomic::cmpxchg(&_top, obj, new_top);
      // result can be one of two:
      //  the old top value: the exchange succeeded
      //  otherwise: the new value of the top is returned.
      if (result == obj) {
        assert(is_object_aligned(obj) && is_object_aligned(new_top), "checking alignment");
        *actual_size = want_to_allocate;
        return obj;
      }
    } else {
      return NULL;
    }
  } while (true);
}

inline HeapWord* HeapRegion::block_start(const void* p) {
  return _bot_part.block_start(p);
}

inline HeapWord* HeapRegion::live_block_at_or_spanning(HeapWord* start) {
  assert(is_aligned(start, BOTConstants::card_size_in_words()), "Start not aligned: " PTR_FORMAT, p2i(start));
  G1CMBitMap* bitmap = G1CollectedHeap::heap()->concurrent_mark()->mark_bitmap();
  // We need to find an object starting at start or before.
  if (bitmap->is_marked(start)) {
    // If start is marked, or this is the first block in a region,
    // simply return start.
    return start;
  }

  // Need to search the blocks before start if still in the current region.
  HeapWord* block_start = start;
  while (block_start > _bottom) {
    // The limit for this round is the previous block_start.
    HeapWord* limit = block_start;
    // Step to the previous block.
    block_start -= BOTConstants::card_size_in_words();
    assert(is_in(block_start), "Should never search outside the region");

    // Now find the last marked object in this block.
    HeapWord* current = bitmap->get_next_marked_addr(block_start, limit);
    HeapWord* last_marked_in_block = NULL;
    // Entering this loop mean we found a marked object in the block
    // and should return it. Step forward to the last marked and
    // keep it recorded in last_marked_in_block.
    while (current < limit) {
      last_marked_in_block = current;
      current = bitmap->get_next_marked_addr(current + 1, limit);
    }
    assert(current == limit, "invariant");

    // When we have found a marked object, we check if it is spanning
    // into the chunk starting at start.
    if (last_marked_in_block != NULL) {
      oop last_marked = cast_to_oop<HeapWord*>(last_marked_in_block);
      HeapWord* next = last_marked_in_block + last_marked->size();
      if (next > start) {
        return last_marked_in_block;
      } else {
        // Found object not spanning into block starting at start.
        return NULL;
      }
    }
   }

  // Did not find any marked objects return NULL.
  assert (block_start == _bottom, "Must search the whole region");
  assert (!bitmap->is_marked(_bottom), "Should not be marked, should have been found above");
  return NULL;
}

inline bool HeapRegion::obj_is_parsable(const HeapWord* addr) const {
  return  is_closed_archive() || addr >= _parsable_bottom;
}

inline bool HeapRegion::is_marked_in_bitmap(oop obj) const {
  return G1CollectedHeap::heap()->concurrent_mark()->mark_bitmap()->is_marked(obj);
}

inline bool HeapRegion::block_is_obj(const HeapWord* p) const {
  assert(p >= bottom() && p < top(), "precondition");
  assert(!is_continues_humongous(), "p must point to block-start");
  return !is_obj_dead(cast_to_oop(p)) || obj_is_scrubbed(cast_to_oop(p));
}

inline bool HeapRegion::obj_is_scrubbed(const oop obj) const {
  Klass* k = obj->klass();
  return k == Universe::fillerArrayKlassObj() || k == vmClasses::FillerObject_klass();
}

inline bool HeapRegion::is_obj_dead(const oop obj) const {
  assert(is_in_reserved(obj), "Object " PTR_FORMAT " must be in region", p2i(obj));
  // Any object allocated since the last mark cycle is live. During Remark
  // the marking completes and what is considered the last cycle is updated.
  if (obj_allocated_since_last_marking(obj)) {
    return false;
  }

  // Objects in closed archive regions are always live.
  if (is_closed_archive()) {
    return false;
  }

  // From Remark until a region has been concurrently scrubbed, parts of the
  // region is not guaranteed to be parsable. Use the bitmap for liveness.
  if (!obj_is_parsable(cast_from_oop<HeapWord*>(obj))) {
    return !is_marked_in_bitmap(obj);
  }

  // This object is in the parsable part of the heap, live unless scrubbed.
  return obj_is_scrubbed(obj);
}

inline size_t HeapRegion::size_of_block(const HeapWord* addr) const {
  assert(_parsable_bottom > _bottom, "Should only be used when part of the region is un-parsable");
  assert(!obj_is_parsable(addr), "Should only be used when addr is in un-parsable range");

  const G1CMBitMap* bitmap = G1CollectedHeap::heap()->concurrent_mark()->mark_bitmap();
  HeapWord* next_live = bitmap->get_next_marked_addr(addr, _parsable_bottom);

  assert(next_live > addr, "Must move forward");
  return pointer_delta(next_live, addr);
}

inline size_t HeapRegion::block_size(const HeapWord *addr) const {
  assert(addr < top(), "precondition");
  //assert(block_is_obj(addr), "All blocks must be valid");

  if (!block_is_obj(addr)) {
    return size_of_block(addr);
  }

  return cast_to_oop(addr)->size();
}

inline void HeapRegion::reset_compaction_top_after_compaction() {
  set_top(compaction_top());
  _compaction_top = bottom();
}

inline void HeapRegion::reset_compacted_after_full_gc() {
  assert(!is_pinned(), "must be");

  reset_compaction_top_after_compaction();
  // After a compaction the mark bitmap in a non-pinned regions is invalid.
  // But all objects are live, we get this by setting "prev" TAMS to bottom.
  zero_marked_bytes();
  init_top_at_mark_start();

  reset_after_full_gc_common();
}

inline void HeapRegion::reset_skip_compacting_after_full_gc() {
  assert(!is_free(), "must be");

  assert(compaction_top() == bottom(),
         "region %u compaction_top " PTR_FORMAT " must not be different from bottom " PTR_FORMAT,
         hrm_index(), p2i(compaction_top()), p2i(bottom()));

  _prev_top_at_mark_start = top(); // Keep existing top and usage.
  _prev_marked_bytes = used();
  _top_at_mark_start = bottom();
  _next_marked_bytes = 0;

  reset_after_full_gc_common();
}

inline void HeapRegion::reset_after_full_gc_common() {
  // Everything above bottom() is parsable and live.
  _parsable_bottom = bottom();

  // Clear unused heap memory in debug builds.
  if (ZapUnusedHeapArea) {
    mangle_unused_area();
  }
}

template<typename ApplyToMarkedClosure>
inline void HeapRegion::apply_to_marked_objects(G1CMBitMap* bitmap, ApplyToMarkedClosure* closure) {
  HeapWord* limit = top();
  HeapWord* next_addr = bottom();

  while (next_addr < limit) {
    Prefetch::write(next_addr, PrefetchScanIntervalInBytes);
    // This explicit is_marked check is a way to avoid
    // some extra work done by get_next_marked_addr for
    // the case where next_addr is marked.
    if (bitmap->is_marked(next_addr)) {
      oop current = cast_to_oop(next_addr);
      next_addr += closure->apply(current);
    } else {
      next_addr = bitmap->get_next_marked_addr(next_addr, limit);
    }
  }

  assert(next_addr == limit, "Should stop the scan at the limit.");
}

inline HeapWord* HeapRegion::par_allocate(size_t min_word_size,
                                          size_t desired_word_size,
                                          size_t* actual_word_size) {
  return par_allocate_impl(min_word_size, desired_word_size, actual_word_size);
}

inline HeapWord* HeapRegion::allocate(size_t word_size) {
  size_t temp;
  return allocate(word_size, word_size, &temp);
}

inline HeapWord* HeapRegion::allocate(size_t min_word_size,
                                      size_t desired_word_size,
                                      size_t* actual_word_size) {
  return allocate_impl(min_word_size, desired_word_size, actual_word_size);
}

inline void HeapRegion::update_bot_for_obj(HeapWord* obj_start, size_t obj_size) {
  assert(is_old(), "should only do BOT updates for old regions");

  HeapWord* obj_end = obj_start + obj_size;

  assert(is_in(obj_start), "obj_start must be in this region: " HR_FORMAT
         " obj_start " PTR_FORMAT " obj_end " PTR_FORMAT,
         HR_FORMAT_PARAMS(this),
         p2i(obj_start), p2i(obj_end));

  _bot_part.update_for_block(obj_start, obj_end);
}

inline void HeapRegion::note_start_of_marking() {
  _next_marked_bytes = 0;
  _top_at_mark_start = top();
  _gc_efficiency = -1.0;
}

inline void HeapRegion::note_end_of_marking() {
  _prev_top_at_mark_start = _top_at_mark_start;
  _parsable_bottom = _top_at_mark_start;
  _top_at_mark_start = bottom();
  _prev_marked_bytes = _next_marked_bytes;
  _next_marked_bytes = 0;
}

inline void HeapRegion::note_end_of_scrubbing() {
  _parsable_bottom = _bottom;
}

inline bool HeapRegion::in_collection_set() const {
  return G1CollectedHeap::heap()->is_in_cset(this);
}

template <class Closure, bool is_gc_active>
HeapWord* HeapRegion::do_oops_on_memregion_in_humongous(MemRegion mr,
                                                        Closure* cl,
                                                        G1CollectedHeap* g1h) {
  assert(is_humongous(), "precondition");
  HeapRegion* sr = humongous_start_region();
  oop obj = cast_to_oop(sr->bottom());

  // If concurrent and klass_or_null is NULL, then space has been
  // allocated but the object has not yet been published by setting
  // the klass.  That can only happen if the card is stale.  However,
  // we've already set the card clean, so we must return failure,
  // since the allocating thread could have performed a write to the
  // card that might be missed otherwise.
  if (!is_gc_active && (obj->klass_or_null_acquire() == NULL)) {
    return NULL;
  }

  // We have a well-formed humongous object at the start of sr.
  // Only filler objects follow a humongous object in the containing
  // regions, and we can ignore those.  So only process the one
  // humongous object.
  if (g1h->is_obj_dead(obj, sr)) {
    // The object is dead. There can be no other object in this region, so return
    // the end of that region.
    return end();
  }
  if (obj->is_objArray() || (sr->bottom() < mr.start())) {
    // objArrays are always marked precisely, so limit processing
    // with mr.  Non-objArrays might be precisely marked, and since
    // it's humongous it's worthwhile avoiding full processing.
    // However, the card could be stale and only cover filler
    // objects.  That should be rare, so not worth checking for;
    // instead let it fall out from the bounded iteration.
    obj->oop_iterate(cl, mr);
    return mr.end();
  } else {
    // If obj is not an objArray and mr contains the start of the
    // obj, then this could be an imprecise mark, and we need to
    // process the entire object.
    size_t size = obj->oop_iterate_size(cl);
    // We have scanned to the end of the object, but since there can be no objects
    // after this humongous object in the region, we can return the end of the
    // region if it is greater.
    return MAX2(cast_from_oop<HeapWord*>(obj) + size, mr.end());
  }
}

template <class Closure, bool is_gc_active>
inline HeapWord* HeapRegion::oops_on_memregion_iterate(MemRegion mr, Closure* cl) {
  // Cache the boundaries of the memory region in some const locals
  HeapWord* const start = mr.start();
  HeapWord* const end = mr.end();

  // Find the obj that extends onto mr.start().
  HeapWord* cur;
  if (obj_is_parsable(start) || is_gc_active) {
    // If start is in the the parsable part of the region or we
    // are in a safepoint the BOT is stable and we can use
    // block_start to find the object start.
    cur = block_start(start);

    assert(cur <= start, "cur: " PTR_FORMAT ", start: " PTR_FORMAT, p2i(cur), p2i(start));
    DEBUG_ONLY(HeapWord* next = cur + block_size(cur);)
    assert(start < next, "start: " PTR_FORMAT ", next: " PTR_FORMAT, p2i(start), p2i(next));
  } else {
    // Doing concurrent refinement and during the scrubbing phase.
    // At this point the BOT is not stable and we need to get the
    // object potentially spanning into the chunk by searching the
    // bitmap.
    cur = live_block_at_or_spanning(start);
    if (cur == NULL) {
      // No live object spanning into this memory region. Find
      // the first live after start.
      cur = start + size_of_block(start);
      // Not safe to scan past the given MemRegion so we need to
      // check that cur is still within it.
      if (cur >= end) {
        return end;
      }
    }
    assert(cur < end, "cur: " PTR_FORMAT ", end: " PTR_FORMAT, p2i(cur), p2i(end));
  }

  while (true) {
    oop obj = cast_to_oop(cur);
    assert(oopDesc::is_oop(obj, true), "Not an oop at " PTR_FORMAT, p2i(cur));
    assert(obj->klass_or_null() != NULL,
           "Unparsable heap at " PTR_FORMAT, p2i(cur));

    bool is_dead = is_obj_dead(obj);
    bool is_precise = false;

    cur += block_size(cur);
    if (!is_dead) {
      // Process live object's references.

      // Non-objArrays are usually marked imprecise at the object
      // start, in which case we need to iterate over them in full.
      // objArrays are precisely marked, but can still be iterated
      // over in full if completely covered.
      if (!obj->is_objArray() || (cast_from_oop<HeapWord*>(obj) >= start && cur <= end)) {
        obj->oop_iterate(cl);
      } else {
        obj->oop_iterate(cl, mr);
        is_precise = true;
      }
    }
    if (cur >= end) {
      return is_precise ? end : cur;
    }
  }
}

template <bool is_gc_active, class Closure>
HeapWord* HeapRegion::oops_on_memregion_seq_iterate_careful(MemRegion mr,
                                                            Closure* cl) {
  assert(MemRegion(bottom(), end()).contains(mr), "Card region not in heap region");
  G1CollectedHeap* g1h = G1CollectedHeap::heap();

  // Special handling for humongous regions.
  if (is_humongous()) {
    return do_oops_on_memregion_in_humongous<Closure, is_gc_active>(mr, cl, g1h);
  }
  assert(is_old() || is_archive(), "Wrongly trying to iterate over region %u type %s", _hrm_index, get_type_str());

  // Because mr has been trimmed to what's been allocated in this
  // region, the parts of the heap that are examined here are always
  // parsable; there's no need to use klass_or_null to detect
  // in-progress allocation.
  // We might be in the progress of scrubbing this region and in this
  // case there might be objects that have their classes unloaded and
  // therefore needs to be scanned using the bitmap.

  return oops_on_memregion_iterate<Closure, is_gc_active>(mr, cl);
}

inline int HeapRegion::age_in_surv_rate_group() const {
  assert(has_surv_rate_group(), "pre-condition");
  assert(has_valid_age_in_surv_rate(), "pre-condition");
  return _surv_rate_group->age_in_group(_age_index);
}

inline bool HeapRegion::has_valid_age_in_surv_rate() const {
  return G1SurvRateGroup::is_valid_age_index(_age_index);
}

inline bool HeapRegion::has_surv_rate_group() const {
  return _surv_rate_group != NULL;
}

inline double HeapRegion::surv_rate_prediction(G1Predictions const& predictor) const {
  assert(has_surv_rate_group(), "pre-condition");
  return _surv_rate_group->surv_rate_pred(predictor, age_in_surv_rate_group());
}

inline void HeapRegion::install_surv_rate_group(G1SurvRateGroup* surv_rate_group) {
  assert(surv_rate_group != NULL, "pre-condition");
  assert(!has_surv_rate_group(), "pre-condition");
  assert(is_young(), "pre-condition");

  _surv_rate_group = surv_rate_group;
  _age_index = surv_rate_group->next_age_index();
}

inline void HeapRegion::uninstall_surv_rate_group() {
  if (has_surv_rate_group()) {
    assert(has_valid_age_in_surv_rate(), "pre-condition");
    assert(is_young(), "pre-condition");

    _surv_rate_group = NULL;
    _age_index = G1SurvRateGroup::InvalidAgeIndex;
  } else {
    assert(!has_valid_age_in_surv_rate(), "pre-condition");
  }
}

inline void HeapRegion::record_surv_words_in_group(size_t words_survived) {
  assert(has_surv_rate_group(), "pre-condition");
  assert(has_valid_age_in_surv_rate(), "pre-condition");
  int age_in_group = age_in_surv_rate_group();
  _surv_rate_group->record_surviving_words(age_in_group, words_survived);
}

#endif // SHARE_GC_G1_HEAPREGION_INLINE_HPP
