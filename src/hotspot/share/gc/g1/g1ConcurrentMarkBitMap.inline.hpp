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

inline bool G1CMBitMap::iterate(G1CMBitMapClosure* cl, MemRegion mr) {
  uint region_idx = _g1h->addr_to_region(mr.start());

  if (!_region_livemaps[region_idx].is_marked()) {
    return true;
  }

  assert(!mr.is_empty(), "Does not support empty memregion to iterate over");
  assert(_covered.contains(mr),
         "Given MemRegion from " PTR_FORMAT " to " PTR_FORMAT " not contained in heap area",
         p2i(mr.start()), p2i(mr.end()));

  HeapWord* addr = get_next_marked_addr(mr.start(), mr.end());
  while (addr < mr.end()) {
    if (!cl->do_addr(addr)) {
      return false;
    }
    size_t const obj_size = cast_to_oop(addr)->size();
    addr = _bitmap.get_next_marked_addr((addr + obj_size), mr.end());
  }
  return true;
}
inline bool G1CMBitMapHR::set(size_t index) {
  if (!is_marked()) {
    // First object to be marked during this
    // cycle, reset marking information.
    initialize();
  }
  return true;
}

inline bool G1CMBitMap::is_marked(oop obj) const{
  return is_marked(cast_from_oop<HeapWord*>(obj));
}

inline bool G1CMBitMap::is_marked(HeapWord* addr) const {
  uint region_idx = _g1h->addr_to_region(addr);
  assert(addr <= _g1h->region_at(region_idx)->end(), "Out of bounds");
  return _region_livemaps[region_idx].is_marked() && _bitmap.is_marked(addr);
}

inline HeapWord* G1CMBitMap::get_next_marked_addr(const HeapWord* const addr,
                                                  HeapWord* const limit) const {
  assert(limit != nullptr, "limit must not be null");
  if (addr == limit) {
    return limit;
  }

  uint region_idx = _g1h->addr_to_region(addr);

  if (!_region_livemaps[region_idx].is_marked()) {
    return limit;
  }

  return _bitmap.get_next_marked_addr(addr, limit);
}

inline bool G1CMBitMap::par_mark(oop obj){
  return par_mark(cast_from_oop<HeapWord*>(obj));
}

inline bool G1CMBitMap::par_mark(HeapWord* addr) {
  uint region_idx = _g1h->addr_to_region(addr);
  assert(addr <= _g1h->region_at(region_idx)->end(), "Out of bounds");
  _region_livemaps[region_idx].set(region_idx);
  return _bitmap.par_mark(addr);
}

inline void G1CMBitMap::clear(HeapWord* addr) {
  uint region_idx = _g1h->addr_to_region(addr);
  assert(addr <= _g1h->region_at(region_idx)->end(), "Out of bounds");
  if (_region_livemaps[region_idx].is_marked()) {
    _bitmap.clear(addr);
  }
}

inline void G1CMBitMap::clear(oop obj) {
  clear(cast_from_oop<HeapWord*>(obj));
}



#endif // SHARE_GC_G1_G1CONCURRENTMARKBITMAP_INLINE_HPP
