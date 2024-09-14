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
#include "logging/log.hpp"
#include "utilities/growableArray.hpp"


class G1CollectionGroup : public CHeapObj<mtGCCardSet>{
  GrowableArray<G1HeapRegion*> _regions;

  G1CardSetMemoryManager _card_set_mm;

  // The set of cards in the Java heap
  G1CardSet _card_set;

  //
  volatile uint _num_regions;

  //
  double _gc_efficiency;

  double predict_group_copy_time_ms() const;

public:
  G1CollectionGroup(G1CardSetConfiguration* config);
  ~G1CollectionGroup() { assert(length() == 0, "post condition!"); }

  void add(G1HeapRegion* hr);

  uint length() const {
    return (uint)_regions.length();
  }

  const GrowableArray<G1HeapRegion*>* regions() const {
    return &_regions;
  }

  G1CardSet* card_set() {
    return &_card_set;
  }

  void calculate_efficiency();

  double gc_efficiency() { return _gc_efficiency; }

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
