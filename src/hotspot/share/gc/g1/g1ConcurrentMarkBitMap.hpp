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

#ifndef SHARE_GC_G1_G1CONCURRENTMARKBITMAP_HPP
#define SHARE_GC_G1_G1CONCURRENTMARKBITMAP_HPP

#include "gc/g1/g1RegionToSpaceMapper.hpp"
#include "gc/shared/markBitMap.hpp"
#include "memory/memRegion.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/bitMap.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"

class G1CollectedHeap;
class G1CMBitMap;
class G1CMBitMapNew;
class G1CMTask;
class G1ConcurrentMark;
class HeapRegion;

// Closure for iteration over bitmaps
class G1CMBitMapClosure {
  G1ConcurrentMark* const _cm;
  G1CMTask* const _task;
public:
  G1CMBitMapClosure(G1CMTask *task, G1ConcurrentMark* cm) : _cm(cm), _task(task) { }

  bool do_addr(HeapWord* const addr);
};

class G1CMBitMapMappingChangedListener : public G1MappingChangedListener {
  G1CMBitMapNew* _bm;
public:
  G1CMBitMapMappingChangedListener() : _bm(nullptr) {}

  void set_bitmap(G1CMBitMapNew* bm) { _bm = bm; }

  virtual void on_commit(uint start_idx, size_t num_regions, bool zero_filled);
};

// A generic mark bitmap for concurrent marking.  This is essentially a wrapper
// around the BitMap class that is based on HeapWords, with one bit per (1 << _shifter) HeapWords.
class G1CMBitMapNew : public MarkBitMap {
  G1CMBitMapMappingChangedListener _listener;

public:
  G1CMBitMapNew();

  // Initializes the underlying BitMap to cover the given area.
  void initialize(MemRegion heap, G1RegionToSpaceMapper* storage);

  // Apply the closure to the addresses that correspond to marked bits in the bitmap.
  inline bool iterate(G1CMBitMapClosure* cl, MemRegion mr);
};


class G1CMBitMapHR {
  using idx_t = BitMap::idx_t;
  CHeapBitMap       _bitmap;
public:
  static size_t BitsPerRegion;
private:
  const size_t      _size;
  volatile bool     _is_marked;
  volatile bool     _is_initialized;
  HeapWord* _base;

  void do_clear(idx_t beg, idx_t end, bool large);

  void resize(size_t size);
public:
  G1CMBitMapHR();

  ~G1CMBitMapHR() = default;

  inline bool get(idx_t index) const;
  inline bool set(idx_t index);

  bool is_marked() const {
    return Atomic::load_acquire(&_is_marked);
  }

  void print_livemap(outputStream* st) const;

  void clear();
  inline void clear(idx_t index);
  inline void clear(idx_t beg, idx_t end, bool large);
  void reset();
  void initialize();

  BitMap::idx_t find_first_set_bit(BitMap::idx_t beg, BitMap::idx_t end) const;
};

class G1CMBitMap {
  G1CMBitMapHR*    _region_livemaps;
  G1CollectedHeap* _g1h;           // The heap
  const uint       _max_regions;
  
  // FIXME: remove
  MemRegion _covered;    // The heap area covered by this bitmap.

      // Convert from address to bit offset.
  size_t addr_to_offset(const HeapWord* base, const HeapWord* addr) const {
    return pointer_delta(align_up(addr, HeapWordSize << LogMinObjAlignment), base) >> LogMinObjAlignment;
  }

  // Convert from bit offset to address.
  HeapWord* offset_to_addr(HeapWord* base, size_t offset) const {
    return base + (offset << LogMinObjAlignment);
  }
public:
  G1CMBitMap(G1CollectedHeap* g1h);
  ~G1CMBitMap();

  void initialize(MemRegion heap) {
    _covered = heap;
  }

  inline HeapWord* next_live_in_unparsable(const HeapWord* p, HeapWord* const limit) const;

  // Apply the closure to the addresses that correspond to marked bits in the bitmap.
  inline bool iterate(G1CMBitMapClosure* cl, MemRegion mr);

  inline bool is_marked(HeapWord* addr) const;
  inline bool is_marked(oop obj) const;

  static size_t heap_map_factor() {
    return MinObjAlignmentInBytes * BitsPerByte;
  }

  //bool is_marked() { return _livemap.is_marked(); }

  inline bool par_mark(oop obj);
  inline bool par_mark(HeapWord* addr);

  void clear_bitmap_for_region(HeapRegion* hr);

  void clear_range(MemRegion mr, bool large = false);

  void clear(HeapWord* addr);
  void clear(oop obj);

  // TODO: fix the name
  inline void reset_livemap(HeapRegion* hr);

  inline HeapWord* get_next_marked_addr(const HeapWord* const addr,
                                        HeapWord* const limit) const;

  inline bool iterate_livemap(G1CMBitMapClosure* cl, MemRegion mr);
};

#endif // SHARE_GC_G1_G1CONCURRENTMARKBITMAP_HPP
