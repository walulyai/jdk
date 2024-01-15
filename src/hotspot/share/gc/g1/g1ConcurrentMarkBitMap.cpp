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
#include "utilities/spinYield.hpp"

G1CMBitMap::G1CMBitMap(G1CollectedHeap* g1h) :
  _listener(),
  _bitmap_mapper(nullptr),
  _bitmap_commits(mtGC),
  _g1h(g1h),
  _covered(),
  _bitmap_storage(),
  _cur_bitmap_region(0)
{
  _region_livemaps = NEW_C_HEAP_ARRAY(G1HRLivemap, _g1h->max_regions(), mtGC);
  for (uint i = 0; i < _g1h->max_regions(); i++) {
    ::new (&_region_livemaps[i]) G1HRLivemap(i);
  }

  _listener.set_bitmap(this);
  _bitmap_commits.initialize(_g1h->max_regions(), true);
}

void G1CMBitMap::assign_bitmap_storage(HeapRegion* hr, MarkBitMap* bm) {
  uint region_idx = hr->hrm_index();
  uint bitmap_region_idx = Atomic::fetch_then_add(&_cur_bitmap_region, 1u);

  assert(bitmap_region_idx < _g1h->max_regions(), "Out of bounds %u >= %u", bitmap_region_idx, _g1h->max_regions());

  if (!_bitmap_commits.par_at(bitmap_region_idx)) {
    _bitmap_mapper->commit_regions(bitmap_region_idx, 1, nullptr /* pretouch_workers */);
    bool result = _bitmap_commits.par_set_bit(bitmap_region_idx);
    assert(result, "sanity!");
  }

  HeapWord* start = _bitmap_storage.start() + bitmap_region_idx * bits_per_region();
  MemRegion bitmap_region(start, start + bits_per_region());

  bm->initialize(MemRegion(hr->bottom(), hr->end()), bitmap_region);
}

size_t G1CMBitMap::bits_per_region() const {
  return HeapRegion::GrainWords / MarkBitMap::heap_map_factor(); 
}

G1CMBitMap::~G1CMBitMap() {
  for (size_t i = 0; i < _g1h->max_regions(); i++) {
    _region_livemaps[i].~G1HRLivemap();
  }
  FREE_C_HEAP_ARRAY(G1HRLivemap, _region_livemaps);
}

void G1CMBitMap::prepare_for_marking() {
  uint committed_regions  = _bitmap_commits.count_one_bits();
  uint threshold = _g1h->eden_regions_count();
  if (committed_regions < threshold) {
    uint regions_to_commit = (threshold - committed_regions);
    _bitmap_mapper->commit_regions(committed_regions, regions_to_commit, _g1h->workers());
    _bitmap_commits.set_range(committed_regions, threshold);
  }
}

void G1CMBitMap::reset() {
  for (size_t i = 0; i < _g1h->max_regions(); i++) {
    _region_livemaps[i].reset();
  }

  uint committed_regions = _bitmap_commits.count_one_bits();
  // FIXME: threshold picked by the gods
  if (committed_regions > 100u) {
    uint regions_to_keep = committed_regions / 2u; // FIXME: 2 is random number picked by the gods
    _bitmap_mapper->uncommit_regions(regions_to_keep, (committed_regions - regions_to_keep));
    _bitmap_commits.clear_range(regions_to_keep, committed_regions);
  }

  Atomic::release_store(&_cur_bitmap_region, 0u);
}

void G1CMBitMap::initialize(MemRegion heap, G1RegionToSpaceMapper* storage) {
  _bitmap_mapper = storage;
  _bitmap_mapper->set_mapping_changed_listener(&_listener);
  _bitmap_storage = storage->reserved();
  _covered = heap;
}

void G1CMBitMapMappingChangedListener::on_commit(uint start_region, size_t num_regions, bool zero_filled) {
  if (zero_filled) {
    return;
  }
  // We need to clear the bitmap on commit, removing any existing information.
  _bm->clear_regions(start_region, num_regions);
}

void G1CMBitMap::clear_bitmap_for_region(HeapRegion* hr) {
  G1HRLivemap* livemap = get_livemap(hr->bottom());

  livemap->clear_range(MemRegion(hr->bottom(), hr->end()), true);

  livemap->clear();
}

void G1CMBitMap::clear_range(MemRegion mr) {
  get_livemap(mr.start())->clear_range(mr, false);
}

void G1CMBitMap::clear_regions(uint start_idx, size_t num_regions) {
  HeapWord* start = _bitmap_storage.start() + start_idx * bits_per_region();
  MemRegion bitmap_region(start, start + num_regions * bits_per_region());

  BitMapView bm = BitMapView((BitMap::bm_word_t*) bitmap_region.start(), bitmap_region.word_size());
  bm.clear_large();
}

void G1CMBitMap::clear_livemap(HeapRegion* hr) {
  uint region_idx = hr->hrm_index();
  G1HRLivemap* livemap = &_region_livemaps[region_idx];
  livemap->clear();
}

void G1CMBitMap::print_on_error(outputStream* st, const char* prefix) const {
  // FIXME: 
  // _bitmap.print_on_error(st, prefix);
}

void G1HRLivemap::reset() {
  Atomic::release_store(&_state, BitmapState::Uninitialized);
  Atomic::release_store(&_is_humongous, false);
}

void G1HRLivemap::clear() {
  if (is_marked()) {
    Atomic::release_store(&_state, BitmapState::Initialized);
    if (is_humongous()) {
      Atomic::release_store(&_is_humongous, false);
      Atomic::release_store(&_state, BitmapState::Uninitialized);
    }
  }
}

G1HRLivemap::G1HRLivemap(uint region) :
  _state(BitmapState::Uninitialized),
  _is_humongous(false),
  _region_idx(region),
  _bitmap()
{ }

bool G1HRLivemap::initialize(G1CMBitMap* _cm_bitmap) {

  if (Atomic::load_acquire(&_state) == BitmapState::Uninitialized) {
    if (Atomic::cmpxchg(&_state, BitmapState::Uninitialized, BitmapState::Initializing) == BitmapState::Uninitialized) {
      HeapRegion* hr = G1CollectedHeap::heap()->region_at(_region_idx);
      if (hr->is_humongous()) {
        Atomic::release_store(&_is_humongous, true);
      } else {
        _cm_bitmap->assign_bitmap_storage(hr, &_bitmap);
        assert(get_next_marked_addr(hr->bottom(), hr->end()) == hr->end(), "ghost mark bits");
      }
      Atomic::release_store(&_state, BitmapState::Initialized);
      return true;
    }
  }
  // Wait for initilization to complete.
  SpinYield spin_yield;
  while (Atomic::load_acquire(&_state) == BitmapState::Initializing) {
    spin_yield.wait();
  }
  return false;
}

void G1HRLivemap::clear_range(MemRegion mr, bool large) {
  if (!is_marked() || is_humongous()) {
    return;
  }

  if (large) {
    _bitmap.clear_range_large(mr);
  } else {
    _bitmap.clear_range(mr);
  }
}

