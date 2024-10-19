/*
 * Copyright (c) 2019, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1COLLECTIONSETCANDIDATES_HPP
#define SHARE_GC_G1_G1COLLECTIONSETCANDIDATES_HPP

#include "gc/g1/g1CollectionGroup.hpp"
#include "gc/g1/g1CollectionSetCandidates.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/workerThread.hpp"
#include "memory/allocation.hpp"
#include "runtime/globals.hpp"
#include "utilities/bitMap.hpp"
#include "utilities/growableArray.hpp"

class G1CollectionSetCandidates;
class G1CollectionCandidateGroupsList;
class G1HeapRegion;
class G1HeapRegionClosure;

using G1CollectionCandidateGroupsListIterator = GrowableArrayIterator<G1CollectionGroup*>;

class G1CollectionCandidateGroupsList {
  GrowableArray<G1CollectionGroup*> _groups;
  volatile uint _num_regions;

  // Comparison function to order regions in decreasing GC efficiency order. This
  // will cause region groups with a lot of live objects and large remembered sets to end
  // up at the end of the list.
  static int compare_gc_efficiency(G1CollectionGroup** ci1, G1CollectionGroup** ci2);

public:
  G1CollectionCandidateGroupsList();
  void append(G1CollectionGroup* group);

  // Empty contents of the list.
  void clear();

  void abandon();

  G1CollectionGroup* at(uint index);

  uint length() const { return (uint)_groups.length(); }

  uint num_regions() const { return _num_regions; }

  void remove_selected(uint count, uint num_regions);

  void remove(G1CollectionCandidateGroupsList* other);

  void prepare_for_scan();

  void sort_by_efficiency();

  GrowableArray<G1CollectionGroup*>*  groups() {
    return &_groups;
  }

  void verify() const PRODUCT_RETURN;

  G1CollectionCandidateGroupsListIterator begin() const {
    return _groups.begin();
  }

  G1CollectionCandidateGroupsListIterator end() const {
    return _groups.end();
  }
};

// Tracks all collection set candidates, i.e. regions that could/should be evacuated soon.
//
// These candidate regions are tracked in two list of regions, sorted by decreasing
// "gc efficiency".
//
// * marking_regions: the set of regions selected by concurrent marking to be
//                    evacuated to keep overall heap occupancy stable.
//                    They are guaranteed to be evacuated and cleared out during
//                    the mixed phase.
//
// * retained_regions: set of regions selected for evacuation during evacuation
//                     failure.
//                     Any young collection will try to evacuate them.
//
class G1CollectionSetCandidates : public CHeapObj<mtGC> {

  enum class CandidateOrigin : uint8_t {
    Invalid,
    Marking,                   // This region has been determined as candidate by concurrent marking.
    Retained,                  // This region has been added because it has been retained after evacuation.
    Verify                     // Special value for verification.
  };

  CandidateOrigin* _contains_map;
  G1CollectionCandidateGroupsList _candidate_groups; // Set of regions selected by concurrent marking.
  G1CollectionCandidateGroupsList _retained_groups; // Set of regions selected by concurrent marking.
  uint _max_regions;

  // The number of regions from the last merge of candidates from the marking.
  uint _last_marking_candidates_length;

  bool is_from_marking(G1HeapRegion* r) const;

public:
  G1CollectionSetCandidates();
  ~G1CollectionSetCandidates();

  G1CollectionCandidateGroupsList& candidate_groups() { return _candidate_groups; }
  G1CollectionCandidateGroupsList& retained_groups() { return _retained_groups; }

  void initialize(uint max_regions);

  void clear();

  // Merge collection set candidates from marking into the current marking list
  // (which needs to be empty).
  void set_candidates_from_marking(G1CollectionSetCandidateInfo* candidate_infos,
                                   uint num_infos);
  // The most recent length of the list that had been merged last via
  // set_candidates_from_marking(). Used for calculating minimum collection set
  // regions.
  uint last_marking_candidates_length() const { return _last_marking_candidates_length; }

  void sort_by_efficiency();

  void sort_marking_by_efficiency();

  // Add the given region to the set of retained regions without regards to the
  // gc efficiency sorting. The retained regions must be re-sorted manually later.
  void add_retained_region_unsorted(G1HeapRegion* r);
  void reset_region(G1HeapRegion* r);

  bool contains(const G1HeapRegion* r) const;

  const char* get_short_type_str(const G1HeapRegion* r) const;

  bool is_empty() const;

  bool has_more_marking_candidates() const;
  uint marking_groups_length() const;
  uint retained_regions_length() const;

private:
  void verify_retained_regions(uint& from_marking, CandidateOrigin* verify_map) PRODUCT_RETURN;

  void verify_candidate_groups(uint& from_marking, CandidateOrigin* verify_map) PRODUCT_RETURN;

  void verify_region(G1HeapRegion* r, CandidateOrigin* verify_map, CandidateOrigin expected_origin) PRODUCT_RETURN;

public:
  void verify() PRODUCT_RETURN;

  uint length() const { return marking_groups_length() + retained_regions_length(); }

  template<typename Func>
  void iterate_regions(Func&& f);
};

#endif /* SHARE_GC_G1_G1COLLECTIONSETCANDIDATES_HPP */
