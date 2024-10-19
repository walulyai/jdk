/*
 * Copyright (c) 24, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1COLLECTIONGROUP_HPP
#define SHARE_GC_G1_G1COLLECTIONGROUP_HPP

#include "gc/g1/g1CardSet.hpp"
#include "gc/g1/g1CardSetMemory.hpp"
#include "gc/g1/g1MonotonicArenaFreePool.hpp"
#include "gc/g1/g1HeapRegion.hpp"
#include "gc/shared/gc_globals.hpp"
#include "logging/log.hpp"
#include "utilities/growableArray.hpp"

struct G1CollectionSetCandidateInfo {
  G1HeapRegion* _r;
  double _gc_efficiency;
  uint _num_unreclaimed;          // Number of GCs this region has been found unreclaimable.

  G1CollectionSetCandidateInfo() : G1CollectionSetCandidateInfo(nullptr, 0.0) { }
  G1CollectionSetCandidateInfo(G1HeapRegion* r, double gc_efficiency) : _r(r), _gc_efficiency(gc_efficiency), _num_unreclaimed(0) { }

  bool update_num_unreclaimed() {
    ++_num_unreclaimed;
    return _num_unreclaimed < G1NumCollectionsKeepPinned;
  }
};

class G1CollectionGroup : public CHeapObj<mtGCCardSet>{
  GrowableArray<G1CollectionSetCandidateInfo> _candidates;

  G1CardSetMemoryManager _card_set_mm;

  // The set of cards in the Java heap
  G1CardSet _card_set;

  //
  double _gc_efficiency;

  double predict_group_copy_time_ms() const;

public:
  G1CollectionGroup(G1CardSetConfiguration* config);
  ~G1CollectionGroup() { assert(length() == 0, "post condition!"); }

  void add(G1HeapRegion* hr);
  void add(G1CollectionSetCandidateInfo &hr_info);

  uint length() const { return (uint)_candidates.length(); }

  const GrowableArray<G1CollectionSetCandidateInfo>* regions() const {
    return &_candidates;
  }

  G1CardSet* card_set() { return &_card_set; }

  void calculate_efficiency();

  // Comparison function to order regions in decreasing GC efficiency order. This
  // will cause regions with a lot of live objects and large remembered sets to end
  // up at the end of the list.
  static int compare_gc_efficiency(G1CollectionSetCandidateInfo* ci1, G1CollectionSetCandidateInfo* ci2);

  static int compare_reclaimble_bytes(G1CollectionSetCandidateInfo* ci1, G1CollectionSetCandidateInfo* ci2);

  double gc_efficiency() { return _gc_efficiency; }

  G1HeapRegion* region_at(uint i) const { return _candidates.at(i)._r; }

  G1CollectionSetCandidateInfo* at(uint i) { return &_candidates.at(i); }

  double predict_group_total_time_ms() const;

  G1MonotonicArenaMemoryStats card_set_memory_stats() const {
    return _card_set_mm.memory_stats();
  }

  void clear();

  void abandon();

  // Limit to the number regions in a collection group. We make an exception
  // for the first collection group to be as large as G1Policy::calc_min_old_cset_length
  // because we are certain that these regions have to be collected together.
  static const int GROUP_SIZE = 5;
};

#endif // SHARE_GC_G1_G1COLLECTIONGROUP_HPP
