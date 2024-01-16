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
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentMarkBitMap.inline.hpp"
#include "gc/g1/heapRegion.hpp"
#include "memory/virtualspace.hpp"

#include "logging/logStream.hpp"

G1CMBitMap::G1CMBitMap(G1CollectedHeap* g1h) :
  _listener(),
  _bitmap(),
  _bitmap_mapper(nullptr),
  _g1h(g1h),
  _covered(),
  _max_regions(_g1h->max_regions())
{
  _listener.set_bitmap(this);

  _region_livemaps = NEW_C_HEAP_ARRAY(G1CMBitMapHR, _max_regions, mtGC);
  for (size_t i = 0; i < _max_regions; i++) {
    ::new (&_region_livemaps[i]) G1CMBitMapHR();
  }
}


G1CMBitMap::~G1CMBitMap() {
  for (size_t i = 0; i < _max_regions; i++) {
    _region_livemaps[i].~G1CMBitMapHR();
  }
  FREE_C_HEAP_ARRAY(G1CMBitMapHR, _region_livemaps);
}

void G1CMBitMapHR::reset() {
  if (is_marked()) {
    Atomic::release_store(&_state, BitmapState::Initialized);
  }
}

G1CMBitMapHR::G1CMBitMapHR() :
  _state(BitmapState::Uninitialized)
{ }

void G1CMBitMapHR::initialize(size_t index, G1RegionToSpaceMapper* _bitmap_mapper) {

  if (Atomic::load(&_state) == BitmapState::Uninitialized) {
    if (Atomic::cmpxchg(&_state, BitmapState::Uninitialized, BitmapState::Initializing) == BitmapState::Uninitialized) {
      log_debug(gc,plab) ("initialising %zu", index);
      _bitmap_mapper->commit_regions(index, 1, nullptr /* pretouch_workers */);
      Atomic::store(&_state, BitmapState::Initialized);
      log_debug(gc,plab) ("initialised %zu", index);
    }
  }

  while (Atomic::load(&_state) == BitmapState::Initializing) { }
}

void G1CMBitMap::initialize(MemRegion heap, G1RegionToSpaceMapper* storage) {
  _bitmap.initialize(heap, storage->reserved());
  _bitmap_mapper = storage;
  _bitmap_mapper->set_mapping_changed_listener(&_listener);
  _covered = heap;
  for (size_t i = 0; i < _max_regions; i++) {
   assert(_region_livemaps[i]._state == G1CMBitMapHR::BitmapState::Uninitialized, "Must be");
  }
}

void G1CMBitMapMappingChangedListener::on_commit(uint start_region, size_t num_regions, bool zero_filled) {
  if (zero_filled) {
    return;
  }
  // We need to clear the bitmap on commit, removing any existing information.
  _bm->clear_regions(start_region, num_regions);
  
}

void G1CMBitMapMappingChangedListener::on_uncommit(uint start_region, size_t num_regions) {
  _bm->uncommit_regions(start_region, num_regions);
}

void G1CMBitMap::uncommit_regions(uint start_idx, size_t num_regions) {
  for (uint region_idx = start_idx; region_idx < start_idx + num_regions; region_idx++ ){
    if (_region_livemaps[region_idx].is_initialized()) {
       _bitmap_mapper->uncommit_regions(region_idx, 1);
    }
  }
}

void G1CMBitMap::clear_bitmap_for_region(HeapRegion* hr) {
  uint region_idx = hr->hrm_index();
  if (!_region_livemaps[region_idx].is_marked()) {
    return;
  }

  _region_livemaps[region_idx].reset();
  _bitmap.clear_range_large(MemRegion(hr->bottom(), hr->end()));
}

void G1CMBitMap::clear_range(MemRegion mr) {
  uint region_idx = _g1h->addr_to_region(mr.start());
  if (!_region_livemaps[region_idx].is_marked()) {
    // Bitmap was not initialized
    return;
  }

  _bitmap.clear_range(mr);
}

void G1CMBitMapHR::set(size_t index, G1RegionToSpaceMapper* _bitmap_mapper) {
  log_debug(gc,plab) ("set start %zu state %u", index, is_initialized());
  if (!is_initialized()) {
     log_debug(gc,plab) ("No Initialized %zu", index);
    // First object to be marked during this
    // cycle, reset marking information.
    initialize(index, _bitmap_mapper);
  }

  if (!is_marked()) {
    log_debug(gc,plab) ("Marking %zu", index);
    Atomic::cmpxchg(&_state, BitmapState::Initialized, BitmapState::Marked);
  }
  log_debug(gc,plab) ("Is already marked %u", Atomic::load_acquire(&_state) == BitmapState::Marked);
  assert(Atomic::load(&_state) == BitmapState::Marked, "sanity!");
}

bool G1CMBitMap::par_mark(oop obj){
  return par_mark(cast_from_oop<HeapWord*>(obj));
}

bool G1CMBitMap::par_mark(HeapWord* addr) {
  uint region_idx = _g1h->addr_to_region(addr);
  assert(addr <= _g1h->region_at(region_idx)->end(), "Out of bounds");
  _region_livemaps[region_idx].set(region_idx, _bitmap_mapper);
  // FIXME: 
  assert(region_idx == 15 || _region_livemaps[15].is_uninitialized(), "must be");
  return _bitmap.par_mark(addr);
}

void G1CMBitMap::clear_regions(uint start_idx, size_t num_regions) {
  for (uint region_idx = start_idx; region_idx < start_idx + num_regions; region_idx++ ){
    if (!_region_livemaps[region_idx].is_uninitialized()) {
      MemRegion mr(G1CollectedHeap::heap()->bottom_addr_for_region(region_idx), HeapRegion::GrainWords);
      _bitmap.clear_range(mr);
    }
  }
}

void G1CMBitMap::print_on_error(outputStream* st, const char* prefix) const {
  _bitmap.print_on_error(st, prefix);
}
