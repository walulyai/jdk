/*
 * Copyright (c) 2017, 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1CONCURRENTMARKBITMAP_INLINE_HPP
#define SHARE_GC_G1_G1CONCURRENTMARKBITMAP_INLINE_HPP

#include "gc/g1/g1ConcurrentMarkBitMap.hpp"

#include "gc/shared/markBitMap.inline.hpp"
#include "memory/memRegion.hpp"
#include "utilities/align.hpp"
#include "utilities/bitMap.inline.hpp"

/*
inline bool G1CMBitMap::iterate(G1CMBitMapClosure* cl, MemRegion mr) {
  assert(!mr.is_empty(), "Does not support empty memregion to iterate over");
  assert(_covered.contains(mr),
         "Given MemRegion from " PTR_FORMAT " to " PTR_FORMAT " not contained in heap area",
         p2i(mr.start()), p2i(mr.end()));

  BitMap::idx_t const end_offset = addr_to_offset(mr.end());
  BitMap::idx_t offset = _bm.find_first_set_bit(addr_to_offset(mr.start()), end_offset);

  while (offset < end_offset) {
    HeapWord* const addr = offset_to_addr(offset);
    if (!cl->do_addr(addr)) {
      return false;
    }
    size_t const obj_size = cast_to_oop(addr)->size();
    offset = _bm.find_first_set_bit(offset + (obj_size >> _shifter), end_offset);
  }
  return true;
}
*/


inline bool G1CMBitMapHR::get(size_t index) const {
  return is_marked() &&
         _bitmap.par_at(index, memory_order_relaxed);
}

inline bool G1CMBitMapHR::set(size_t index) {
  if (!is_marked()) {
    // First object to be marked during this
    // cycle, reset marking information.
    initialize();
  }

  return _bitmap.par_set_bit(index);
}

inline BitMap::idx_t G1CMBitMapHR::find_first_set_bit(BitMap::idx_t beg, BitMap::idx_t end) const {
  if (!is_marked() || beg == end) {
    return end;
  }

  assert(beg < end && end <= BitsPerRegion, "out of bounds beg %zu < end %zu BitsPerRegion %zu", beg, end, BitsPerRegion);

  return _bitmap.find_first_set_bit(beg, end);
}

inline void G1CMBitMapHR::clear(size_t index) {
  _bitmap.clear_bit(index);
}


inline HeapWord* G1CMBitMap::get_next_marked_addr(const HeapWord* const addr,
                                                  HeapWord* const limit) const {

  assert(limit != nullptr, "limit must not be null");
  if (addr == limit) {
    return limit;
  }

  uint region_idx = _g1h->addr_to_region(addr);

  G1CMBitMapHR* livemap = &_region_livemaps[region_idx];

  if (!livemap->is_marked()) {
    return limit;
  }

  assert(_g1h->_hrm.is_available(region_idx), "why do we have a marked unavailable region?? %d", livemap->is_marked());

  assert(limit <= _g1h->region_at(region_idx)->end(), "Out of bounds");

  if (addr == limit) {
    return limit;
  }

  HeapWord* bottom = _g1h->bottom_addr_for_region(region_idx);

  size_t const addr_offset = addr_to_offset(bottom, addr);
  size_t const limit_offset = addr_to_offset(bottom, limit);

  size_t const nextOffset = livemap->find_first_set_bit(addr_offset, limit_offset);

  return offset_to_addr(bottom, nextOffset);
}

inline bool G1CMBitMap::is_marked(HeapWord* addr) const {
  uint region_idx = _g1h->addr_to_region(addr);
  assert(addr <= _g1h->region_at(region_idx)->end(), "Out of bounds");
  size_t index = addr_to_offset(_g1h->bottom_addr_for_region(region_idx), addr);
  return _region_livemaps[region_idx].get(index);
}

inline bool G1CMBitMap::is_marked(oop obj) const {
  return is_marked(cast_from_oop<HeapWord*>(obj));
}

inline bool G1CMBitMap::par_mark(HeapWord* addr) {
  uint region_idx = _g1h->addr_to_region(addr);
  assert(addr <= _g1h->region_at(region_idx)->end(), "Out of bounds");
  size_t index = addr_to_offset(_g1h->bottom_addr_for_region(region_idx), addr);
  return _region_livemaps[region_idx].set(index);
}


inline bool G1CMBitMap::par_mark(oop obj){
  return par_mark(cast_from_oop<HeapWord*>(obj));
}

inline void G1CMBitMap::clear(HeapWord* addr) {
  uint region_idx = _g1h->addr_to_region(addr);
  assert(addr <= _g1h->region_at(region_idx)->end(), "Out of bounds");
  size_t index = addr_to_offset(_g1h->bottom_addr_for_region(region_idx), addr);
  _region_livemaps[region_idx].clear(index);
}

inline void G1CMBitMap::clear(oop obj) {
  clear(cast_from_oop<HeapWord*>(obj));
}

inline void G1CMBitMap::reset_livemap(HeapRegion* hr) {
  uint region_idx = hr->hrm_index();
  G1CMBitMapHR* livemap = &_region_livemaps[region_idx];
  livemap->reset();
}

inline bool G1CMBitMap::iterate(G1CMBitMapClosure* cl, MemRegion mr) {
  assert(!mr.is_empty(), "Does not support empty memregion to iterate over");

  assert(_g1h->reserved().contains(mr),
         "Given MemRegion from " PTR_FORMAT " to " PTR_FORMAT " not contained in current region",
         p2i(mr.start()), p2i(mr.end()));

  uint region_idx = _g1h->addr_to_region(mr.start());
  
  HeapWord* bottom = _g1h->bottom_addr_for_region(region_idx);
  BitMap::idx_t const end_offset = addr_to_offset(bottom, mr.end());

  G1CMBitMapHR* livemap = &_region_livemaps[region_idx];

  BitMap::idx_t offset = livemap->find_first_set_bit(addr_to_offset(bottom, mr.start()), end_offset);

  while (offset < end_offset) {
    HeapWord* const addr = offset_to_addr(bottom, offset);
    assert(livemap->get(offset), "why did we find it if its not marked");
    assert(is_marked(addr), "why did we find it if its not marked");
    if (!cl->do_addr(addr)) {
      return false;
    }

    size_t const obj_size = cast_to_oop(addr)->size();
    offset = livemap->find_first_set_bit(offset + (obj_size >> LogMinObjAlignment), end_offset);
  }
  return true;
}

#endif // SHARE_GC_G1_G1CONCURRENTMARKBITMAP_INLINE_HPP
