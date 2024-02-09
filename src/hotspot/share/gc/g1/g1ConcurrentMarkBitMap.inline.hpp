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

inline G1HRLivemap* G1CMBitMap::get_livemap(const HeapWord* const addr) const {
  uint region_idx = _g1h->addr_to_region(addr);
  return &_region_livemaps[region_idx];
}

inline bool G1CMBitMap::iterate(G1CMBitMapClosure* cl, MemRegion mr) {

  assert(!mr.is_empty(), "Does not support empty memregion to iterate over");
  assert(_covered.contains(mr),
         "Given MemRegion from " PTR_FORMAT " to " PTR_FORMAT " not contained in heap area",
         p2i(mr.start()), p2i(mr.end()));

  return get_livemap(mr.start())->iterate(cl, mr);
}

inline HeapWord* G1CMBitMap::get_next_marked_addr(const HeapWord* const addr,
                                                  HeapWord* const limit) const {
  return get_livemap(addr)->get_next_marked_addr(addr, limit);
}

inline void G1CMBitMap::clear(HeapWord* addr) {
  get_livemap(addr)->clear(addr);
}

inline void G1CMBitMap::clear(oop obj) {
  clear(cast_from_oop<HeapWord*>(obj));
}

bool G1CMBitMap::is_marked(HeapWord* addr) const {
  return get_livemap(addr)->is_marked(addr);
}

inline bool G1CMBitMap::is_marked(oop obj) const{
  return is_marked(cast_from_oop<HeapWord*>(obj));
}

inline bool G1CMBitMap::par_mark(oop obj){
  return par_mark(cast_from_oop<HeapWord*>(obj));
}

inline bool G1CMBitMap::par_mark(HeapWord* addr) {  
  return get_livemap(addr)->par_mark(addr, this);
}

inline bool G1HRLivemap::is_marked(HeapWord* addr) const {
  return is_marked() && (is_humongous() || _bitmap.is_marked(addr));
}

inline bool G1HRLivemap::par_mark(HeapWord* addr, G1CMBitMap* _cm_bitmap) {
  bool success = false;
  if (!is_initialized()) {
    // First object to be marked during this
    // cycle, state information.
    success = initialize(_cm_bitmap);
  }

  if (!is_marked()) {
    Atomic::cmpxchg(&_state, BitmapState::Initialized, BitmapState::Marked);
  }
  return is_humongous() ? success : _bitmap.par_mark(addr);
}

inline void G1HRLivemap::clear(HeapWord* addr) {
  if (!is_marked()) {
    return;
  }

  if (is_humongous()) {
    assert(addr == G1CollectedHeap::heap()->region_at(_region_idx)->bottom(), "Out of bounds");
    reset();
  } else {
    _bitmap.clear(addr);
  }
}

inline bool G1HRLivemap::iterate(G1CMBitMapClosure* cl, MemRegion mr) {
  if (!is_marked()) {
    return true;
  }

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

inline HeapWord* G1HRLivemap::get_next_marked_addr(const HeapWord* const addr,
                                                   HeapWord* const limit) const {
  assert(limit != nullptr, "limit must not be null");
  if (addr == limit) {
    return limit;
  }

  if (!is_marked()) {
    return limit;
  }

  if (is_humongous()) {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    return (addr == g1h->region_at(_region_idx)->bottom()) ? g1h->region_at(_region_idx)->bottom() : limit;
  }

  return _bitmap.get_next_marked_addr(addr, limit);
}

#endif // SHARE_GC_G1_G1CONCURRENTMARKBITMAP_INLINE_HPP
