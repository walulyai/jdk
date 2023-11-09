/*
 * Copyright (c) 2001, 2023, Oracle and/or its affiliates. All rights reserved.
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

#include "classfile/vmClasses.hpp"
#include "gc/g1/g1BlockOffsetTable.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentMarkBitMap.inline.hpp"
#include "gc/g1/g1MonotonicArena.inline.hpp"
#include "gc/g1/g1Policy.hpp"
#include "gc/g1/g1Predictions.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/init.hpp"
#include "runtime/prefetch.inline.hpp"
#include "runtime/safepoint.hpp"
#include "utilities/align.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/globalDefinitions.hpp"

inline bool LiveMap::claim_segment(BitMap::idx_t segment) {
  return segment_claim_bits().par_set_bit(segment, memory_order_acq_rel);
}

inline BitMap::idx_t LiveMap::first_live_segment() const {
  return segment_live_bits().find_first_set_bit(0, nsegments);
}

inline BitMap::idx_t LiveMap::next_live_segment(BitMap::idx_t segment) const {
  return segment_live_bits().find_first_set_bit(segment + 1, nsegments);
}

inline bool LiveMap::get(size_t index) const {
  BitMap::idx_t segment = index_to_segment(index);
  return is_segment_live(segment) &&                  // Segment is marked
        _bitmap.par_at(index, memory_order_relaxed); // Object is marked
}

inline bool LiveMap::set(size_t index) {
  /* TODO: if (!is_marked()) {
    // First object to be marked during this
    // cycle, reset marking information.
    reset(index);
  } */
  const BitMap::idx_t segment = index_to_segment(index);
  if (!is_segment_live(segment)) {
    // First object to be marked in this segment during
    // this cycle, reset segment bitmap.
    reset_segment(segment);
  }

  return _bitmap.par_set_bit(index);
}


inline bool LiveMap::is_segment_live(BitMap::idx_t segment) const {
  return segment_live_bits().par_at(segment);
}

inline bool LiveMap::set_segment_live(BitMap::idx_t segment) {
  return segment_live_bits().par_set_bit(segment, memory_order_release);
}

inline void LiveMap::inc_live(uint32_t objects, size_t bytes) {
  Atomic::add(&_live_objects, objects);
  Atomic::add(&_live_bytes, bytes);
}

inline BitMap::idx_t LiveMap::find_first_set_bit(BitMap::idx_t beg, BitMap::idx_t end) const {
  BitMap::idx_t assert_index = _bitmap.find_first_set_bit(beg, end);
  BitMap::idx_t segment = index_to_segment(beg);
  if (is_segment_live(segment)) {
    BitMap::idx_t end_index = segment_end(segment);

    // TODO: for christsake clean this up
    end_index = (end_index < end) ? end_index : end;
    assert(end_index <= _bitmap.size(), "is_segment_live");
    BitMap::idx_t index = _bitmap.find_first_set_bit(beg, end_index);
    if (index < end_index) {
      assert(assert_index == index, "must be %zu != %zu / %zu (%zu)", assert_index, index, end, _bitmap.find_first_set_bit(beg, end));
      return index;
    }
  }

  segment = next_live_segment(segment);
  if (segment > index_to_segment(end)) {
    return end;
  }
  const BitMap::idx_t start_index = segment_start(segment);
  BitMap::idx_t end_index   = segment_end(segment);
  end_index = (end_index < end) ? end_index : end;
  assert(start_index <= _bitmap.size(), "start_index");
  assert(end_index <= _bitmap.size(), "end_index %zu > size %zu range [%zu - %zu] / [%zu - %zu] ", end_index, _bitmap.size(), start_index, end_index, beg, end);
  BitMap::idx_t index = _bitmap.find_first_set_bit(start_index, end_index);

  assert(assert_index == index, "must be %zu != %zu", assert_index, index);
  return (index < end) ? index : end;
}

inline HeapWord* HeapRegion::get_next_marked_addr(const HeapWord* const addr,
                                                  HeapWord* const limit) const {
  assert(limit != nullptr, "limit must not be null");
  // Round addr up to a possible object boundary to be safe.
  size_t const addr_offset = addr_to_offset(align_up(addr, HeapWordSize << LogMinObjAlignment));
  size_t const limit_offset = addr_to_offset(limit);

  assert(addr_offset <= _livemap._bitmap.size(), "addr_offset");
  assert(limit_offset <= _livemap._bitmap.size(), "limit_offset");
  assert(addr_offset <= limit_offset, "what the heck");

  size_t const nextOffset = _livemap.find_first_set_bit(addr_offset, limit_offset);

  return offset_to_addr(nextOffset);
}

inline bool HeapRegion::is_object_marked(HeapWord* addr) const {
  size_t index = addr_to_offset(addr);
  return _livemap.get(index);
}

inline bool HeapRegion::is_object_marked(oop obj) const {
  return is_object_marked(cast_from_oop<HeapWord*>(obj));
}

inline bool HeapRegion::mark_object(oop obj){
  return mark_object(cast_from_oop<HeapWord*>(obj));
}

inline bool HeapRegion::mark_object(HeapWord* addr) {
  size_t index = addr_to_offset(addr);
  return _livemap.set(index);
}



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
    return nullptr;
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
      return nullptr;
    }
  } while (true);
}

inline HeapWord* HeapRegion::block_start(const void* addr) const {
  return block_start(addr, parsable_bottom_acquire());
}

inline HeapWord* HeapRegion::advance_to_block_containing_addr(const void* addr,
                                                              HeapWord* const pb,
                                                              HeapWord* first_block) const {
  HeapWord* cur_block = first_block;
  while (true) {
    HeapWord* next_block = cur_block + block_size(cur_block, pb);
    if (next_block > addr) {
      assert(cur_block <= addr, "postcondition");
      return cur_block;
    }
    cur_block = next_block;
    // Because the BOT is precise, we should never step into the next card
    // (i.e. crossing the card boundary).
    assert(!G1BlockOffsetTablePart::is_crossing_card_boundary(cur_block, (HeapWord*)addr), "must be");
  }
}

inline HeapWord* HeapRegion::block_start(const void* addr, HeapWord* const pb) const {
  HeapWord* first_block = _bot_part.block_start_reaching_into_card(addr);
  return advance_to_block_containing_addr(addr, pb, first_block);
}

inline bool HeapRegion::is_in_parsable_area(const void* const addr) const {
  return is_in_parsable_area(addr, parsable_bottom());
}

inline bool HeapRegion::is_in_parsable_area(const void* const addr, const void* const pb) {
  return addr >= pb;
}

inline bool HeapRegion::is_marked_in_bitmap(oop obj) const {
  // TODO: 
  is_object_marked(cast_from_oop<HeapWord*>(obj));
  return G1CollectedHeap::heap()->concurrent_mark()->mark_bitmap()->is_marked(obj);
}

inline bool HeapRegion::block_is_obj(const HeapWord* const p, HeapWord* const pb) const {
  assert(p >= bottom() && p < top(), "precondition");
  assert(!is_continues_humongous(), "p must point to block-start");

  if (is_in_parsable_area(p, pb)) {
    return true;
  }

  // When class unloading is enabled it is not safe to only consider top() to conclude if the
  // given pointer is a valid object. The situation can occur both for class unloading in a
  // Full GC and during a concurrent cycle.
  // To make sure dead objects can be handled without always keeping an additional bitmap, we
  // scrub dead objects and create filler objects that are considered dead. We do this even if
  // class unloading is disabled to avoid special code.
  // From Remark until the region has been completely scrubbed obj_is_parsable will return false
  // and we have to use the bitmap to know if a block is a valid object.
  return is_marked_in_bitmap(cast_to_oop(p));
}

inline HeapWord* HeapRegion::next_live_in_unparsable(G1CMBitMap* const bitmap, const HeapWord* p, HeapWord* const limit) const {
  // TODO: 
  get_next_marked_addr(p, limit);
  return bitmap->get_next_marked_addr(p, limit);
}

inline HeapWord* HeapRegion::next_live_in_unparsable(const HeapWord* p, HeapWord* const limit) const {
  G1CMBitMap* bitmap = G1CollectedHeap::heap()->concurrent_mark()->mark_bitmap();
  return next_live_in_unparsable(bitmap, p, limit);
}

inline bool HeapRegion::is_collection_set_candidate() const {
 return G1CollectedHeap::heap()->is_collection_set_candidate(this);
}

inline size_t HeapRegion::block_size(const HeapWord* p) const {
  return block_size(p, parsable_bottom());
}

inline size_t HeapRegion::block_size(const HeapWord* p, HeapWord* const pb) const {
  assert(p < top(), "precondition");

  if (!block_is_obj(p, pb)) {
    return pointer_delta(next_live_in_unparsable(p, pb), p);
  }

  return cast_to_oop(p)->size();
}

inline void HeapRegion::reset_compacted_after_full_gc(HeapWord* new_top) {
  set_top(new_top);
  // After a compaction the mark bitmap in a movable region is invalid.
  // But all objects are live, we get this by setting TAMS to bottom.
  init_top_at_mark_start();

  reset_after_full_gc_common();
}

inline void HeapRegion::reset_skip_compacting_after_full_gc() {
  assert(!is_free(), "must be");

  _garbage_bytes = 0;

  reset_top_at_mark_start();

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
      // TODO: 
      assert(is_object_marked(next_addr), "Must be!");
      oop current = cast_to_oop(next_addr);
      next_addr += closure->apply(current);
    } else {
      // TODO:
      HeapWord* start = next_addr;
      HeapWord* next_addr_new =  get_next_marked_addr(start, limit);
      next_addr = bitmap->get_next_marked_addr(next_addr, limit);
      assert(next_addr_new == next_addr, "Must be! " PTR_FORMAT " != " PTR_FORMAT " Limit: " PTR_FORMAT,
             p2i(next_addr_new), p2i(next_addr), p2i(start));
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

inline void HeapRegion::update_bot() {
  HeapWord* next_addr = bottom();

  HeapWord* prev_addr;
  while (next_addr < top()) {
    prev_addr = next_addr;
    next_addr  = prev_addr + cast_to_oop(prev_addr)->size();
    update_bot_for_block(prev_addr, next_addr);
  }
  assert(next_addr == top(), "Should stop the scan at the limit.");
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

inline HeapWord* HeapRegion::top_at_mark_start() const {
  return Atomic::load(&_top_at_mark_start);
}

inline void HeapRegion::set_top_at_mark_start(HeapWord* value) {
  Atomic::store(&_top_at_mark_start, value);
}

inline HeapWord* HeapRegion::parsable_bottom() const {
  assert(!is_init_completed() || SafepointSynchronize::is_at_safepoint(), "only during initialization or safepoint");
  return _parsable_bottom;
}

inline HeapWord* HeapRegion::parsable_bottom_acquire() const {
  return Atomic::load_acquire(&_parsable_bottom);
}

inline void HeapRegion::reset_parsable_bottom() {
  Atomic::release_store(&_parsable_bottom, bottom());
}

inline void HeapRegion::note_start_of_marking() {
  assert(top_at_mark_start() == bottom(), "Region's TAMS must always be at bottom");
  if (is_old_or_humongous() && !is_collection_set_candidate()) {
    set_top_at_mark_start(top());
  }
}

inline void HeapRegion::note_end_of_marking(size_t marked_bytes) {
  assert_at_safepoint();

  if (top_at_mark_start() != bottom()) {
    _garbage_bytes = byte_size(bottom(), top_at_mark_start()) - marked_bytes;
  }

  if (needs_scrubbing()) {
    _parsable_bottom = top_at_mark_start();
  }
}

inline void HeapRegion::note_end_of_scrubbing() {
  reset_parsable_bottom();
}

inline void HeapRegion::init_top_at_mark_start() {
  reset_top_at_mark_start();
  _parsable_bottom = bottom();
  _garbage_bytes = 0;
}

inline void HeapRegion::reset_top_at_mark_start() {
  // We do not need a release store here because
  //
  // - if this method is called during concurrent bitmap clearing, we do not read
  // the bitmap any more for live/dead information (we do not read the bitmap at
  // all at that point).
  // - otherwise we reclaim regions only during GC and we do not read tams and the
  // bitmap concurrently.
  set_top_at_mark_start(bottom());
}

inline bool HeapRegion::needs_scrubbing() const {
  return is_old();
}

inline bool HeapRegion::in_collection_set() const {
  return G1CollectedHeap::heap()->is_in_cset(this);
}

template <class Closure, bool in_gc_pause>
HeapWord* HeapRegion::do_oops_on_memregion_in_humongous(MemRegion mr,
                                                        Closure* cl) {
  assert(is_humongous(), "precondition");
  HeapRegion* sr = humongous_start_region();
  oop obj = cast_to_oop(sr->bottom());

  // If concurrent and klass_or_null is null, then space has been
  // allocated but the object has not yet been published by setting
  // the klass.  That can only happen if the card is stale.  However,
  // we've already set the card clean, so we must return failure,
  // since the allocating thread could have performed a write to the
  // card that might be missed otherwise.
  if (!in_gc_pause && (obj->klass_or_null_acquire() == nullptr)) {
    return nullptr;
  }

  // We have a well-formed humongous object at the start of sr.
  // Only filler objects follow a humongous object in the containing
  // regions, and we can ignore those.  So only process the one
  // humongous object.
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

template <class Closure>
inline HeapWord* HeapRegion::oops_on_memregion_iterate_in_unparsable(MemRegion mr, HeapWord* block_start, Closure* cl) {
  HeapWord* const start = mr.start();
  HeapWord* const end = mr.end();

  G1CMBitMap* bitmap = G1CollectedHeap::heap()->concurrent_mark()->mark_bitmap();

  HeapWord* cur = block_start;

  while (true) {
    // Using bitmap to locate marked objs in the unparsable area
    // TODO
    get_next_marked_addr(cur, end);
    cur = bitmap->get_next_marked_addr(cur, end);
    if (cur == end) {
      return end;
    }
    // TODO:
    is_object_marked(cur);
    assert(bitmap->is_marked(cur), "inv");

    oop obj = cast_to_oop(cur);
    assert(oopDesc::is_oop(obj, true), "Not an oop at " PTR_FORMAT, p2i(cur));

    cur += obj->size();
    bool is_precise;

    if (!obj->is_objArray() || (cast_from_oop<HeapWord*>(obj) >= start && cur <= end)) {
      obj->oop_iterate(cl);
      is_precise = false;
    } else {
      obj->oop_iterate(cl, mr);
      is_precise = true;
    }

    if (cur >= end) {
      return is_precise ? end : cur;
    }
  }
}

// Applies cl to all reference fields of live objects in mr in non-humongous regions.
//
// For performance, the strategy here is to divide the work into two parts: areas
// below parsable_bottom (unparsable) and above parsable_bottom. The unparsable parts
// use the bitmap to locate live objects.
// Otherwise we would need to check for every object what the current location is;
// we expect that the amount of GCs executed during scrubbing is very low so such
// tests would be unnecessary almost all the time.
template <class Closure, bool in_gc_pause>
inline HeapWord* HeapRegion::oops_on_memregion_iterate(MemRegion mr, Closure* cl) {
  // Cache the boundaries of the memory region in some const locals
  HeapWord* const start = mr.start();
  HeapWord* const end = mr.end();

  // Snapshot the region's parsable_bottom.
  HeapWord* const pb = in_gc_pause ? parsable_bottom() : parsable_bottom_acquire();

  // Find the obj that extends onto mr.start().
  //
  // The BOT itself is stable enough to be read at any time as
  //
  // * during refinement the individual elements of the BOT are read and written
  //   atomically and any visible mix of new and old BOT entries will eventually lead
  //   to some (possibly outdated) object start.
  //
  // * during GC the BOT does not change while reading, and the objects corresponding
  //   to these block starts are valid as "holes" are filled atomically wrt to
  //   safepoints.
  //
  HeapWord* cur = block_start(start, pb);
  if (!is_in_parsable_area(start, pb)) {
    // Limit the MemRegion to the part of the area to scan to the unparsable one as using the bitmap
    // is slower than blindly iterating the objects.
    MemRegion mr_in_unparsable(mr.start(), MIN2(mr.end(), pb));
    cur = oops_on_memregion_iterate_in_unparsable<Closure>(mr_in_unparsable, cur, cl);
    // We might have scanned beyond end at this point because of imprecise iteration.
    if (cur >= end) {
      return cur;
    }
    // Parsable_bottom is always the start of a valid parsable object, so we must either
    // have stopped at parsable_bottom, or already iterated beyond end. The
    // latter case is handled above.
    assert(cur == pb, "must be cur " PTR_FORMAT " pb " PTR_FORMAT, p2i(cur), p2i(pb));
  }
  assert(cur < top(), "must be cur " PTR_FORMAT " top " PTR_FORMAT, p2i(cur), p2i(top()));

  // All objects >= pb are parsable. So we can just take object sizes directly.
  while (true) {
    oop obj = cast_to_oop(cur);
    assert(oopDesc::is_oop(obj, true), "Not an oop at " PTR_FORMAT, p2i(cur));

    bool is_precise = false;

    cur += obj->size();
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
    if (cur >= end) {
      return is_precise ? end : cur;
    }
  }
}

template <bool in_gc_pause, class Closure>
HeapWord* HeapRegion::oops_on_memregion_seq_iterate_careful(MemRegion mr,
                                                            Closure* cl) {
  assert(MemRegion(bottom(), top()).contains(mr), "Card region not in heap region");

  // Special handling for humongous regions.
  if (is_humongous()) {
    return do_oops_on_memregion_in_humongous<Closure, in_gc_pause>(mr, cl);
  }
  assert(is_old(), "Wrongly trying to iterate over region %u type %s", _hrm_index, get_type_str());

  // Because mr has been trimmed to what's been allocated in this
  // region, the objects in these parts of the heap have non-null
  // klass pointers. There's no need to use klass_or_null to detect
  // in-progress allocation.
  // We might be in the progress of scrubbing this region and in this
  // case there might be objects that have their classes unloaded and
  // therefore needs to be scanned using the bitmap.

  return oops_on_memregion_iterate<Closure, in_gc_pause>(mr, cl);
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
  return _surv_rate_group != nullptr;
}

inline double HeapRegion::surv_rate_prediction(G1Predictions const& predictor) const {
  assert(has_surv_rate_group(), "pre-condition");
  return _surv_rate_group->surv_rate_pred(predictor, age_in_surv_rate_group());
}

inline void HeapRegion::install_surv_rate_group(G1SurvRateGroup* surv_rate_group) {
  assert(surv_rate_group != nullptr, "pre-condition");
  assert(!has_surv_rate_group(), "pre-condition");
  assert(is_young(), "pre-condition");

  _surv_rate_group = surv_rate_group;
  _age_index = surv_rate_group->next_age_index();
}

inline void HeapRegion::uninstall_surv_rate_group() {
  if (has_surv_rate_group()) {
    assert(has_valid_age_in_surv_rate(), "pre-condition");
    assert(is_young(), "pre-condition");

    _surv_rate_group = nullptr;
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
