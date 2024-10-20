/*
 * Copyright (c) 2019, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1CollectionSetCandidates.inline.hpp"
#include "gc/g1/g1CollectionSetChooser.hpp"
#include "gc/g1/g1HeapRegion.inline.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/growableArray.hpp"

G1CollectionCandidateGroupsList::G1CollectionCandidateGroupsList() : _groups(8, mtGC), _num_regions(0) { }

void G1CollectionCandidateGroupsList::append(G1CollectionGroup* group) {
  assert(group->length() > 0, "Do not add empty groups");
  assert(!_groups.contains(group), "Already added to list");
  _groups.append(group);
  _num_regions += group->length();
  log_error(gc)("Added " PTR_FORMAT, p2i(group));
}

G1CollectionGroup* G1CollectionCandidateGroupsList::at(uint index) {
  return _groups.at(index);
}

void G1CollectionCandidateGroupsList::clear() {
  for (int i = 0; i < _groups.length(); i++) {
    G1CollectionGroup* gr = _groups.at(i);
    gr->clear();
    delete gr;
  }
  _groups.clear();
  _num_regions = 0;
}

void G1CollectionCandidateGroupsList::abandon() {
  for (int i = 0; i < _groups.length(); i++) {
    G1CollectionGroup* gr = _groups.at(i);
    gr->abandon();
    log_error(gc)("Deleted " PTR_FORMAT, p2i(gr));
    delete gr;
  }
  _groups.clear();
  _num_regions = 0;
}

void G1CollectionCandidateGroupsList::prepare_for_scan() {
  for (int i = 0; i < _groups.length(); i++) {
    _groups.at(i)->card_set()->reset_table_scanner();
  }
}

void G1CollectionCandidateGroupsList::remove_selected(uint count, uint num_regions) {
  _groups.remove_till(count);
  _num_regions -= num_regions;
}

void G1CollectionCandidateGroupsList::remove(G1CollectionCandidateGroupsList* other) {
  //FIXME: 
  // guarantee((uint)_groups.length() >= other->length(), "must be");

  // FIXME:
  if (other->length() == 0 || length() == 0) {
    // Nothing to remove or nothing in the original set.
    return;
  }

  // Create a list from scratch, copying over the elements from the candidate
  // list not in the other list. Finally deallocate and overwrite the old list.
  int new_length = _groups.length() - other->length();
  _num_regions = num_regions() - other->num_regions();
  GrowableArray<G1CollectionGroup*> new_list(new_length, mtGC);

  uint other_idx = 0;

  for (uint gr_idx = 0; gr_idx < (uint)_groups.length(); gr_idx++) {
    if ((other_idx == other->length()) || _groups.at(gr_idx) != other->at(other_idx)) {
      new_list.append(_groups.at(gr_idx));
    } else {
      other_idx++;
    }
  }
  _groups.swap(&new_list);

  verify();
  assert(_groups.length() == new_length, "must be %u %u", _groups.length(), new_length);
}

int G1CollectionCandidateGroupsList::compare_gc_efficiency(G1CollectionGroup** gr1, G1CollectionGroup** gr2) {

  double gc_eff1 = (*gr1)->gc_efficiency();
  double gc_eff2 = (*gr2)->gc_efficiency();

  if (gc_eff1 > gc_eff2) {
    return -1;
  } else if (gc_eff1 < gc_eff2) {
    return 1;
  } else {
    return 0;
  }
}

void G1CollectionCandidateGroupsList::sort_by_efficiency() {
  _groups.sort(compare_gc_efficiency);
}

#ifndef PRODUCT
void G1CollectionCandidateGroupsList::verify() const {
  G1CollectionGroup* prev = nullptr;

  for (uint i = 0; i < (uint)_groups.length(); i++) {
    G1CollectionGroup* gr = _groups.at(i);
    assert(prev == nullptr || prev->gc_efficiency() >= gr->gc_efficiency(),
           "Stored gc efficiency must be descending");
    prev = gr;
  }
}
#endif


G1CollectionSetCandidates::G1CollectionSetCandidates() :
  _contains_map(nullptr),
  _candidate_groups(),
  _retained_groups(),
  _max_regions(0),
  _last_marking_candidates_length(0)
{ }

G1CollectionSetCandidates::~G1CollectionSetCandidates() {
  FREE_C_HEAP_ARRAY(CandidateOrigin, _contains_map);
  _candidate_groups.clear();
}

bool G1CollectionSetCandidates::is_from_marking(G1HeapRegion* r) const {
  assert(contains(r), "must be");
  return _contains_map[r->hrm_index()] == CandidateOrigin::Marking;
}

void G1CollectionSetCandidates::initialize(uint max_regions) {
  assert(_contains_map == nullptr, "already initialized");
  _max_regions = max_regions;
  _contains_map = NEW_C_HEAP_ARRAY(CandidateOrigin, max_regions, mtGC);
  clear();
}

void G1CollectionSetCandidates::clear() {
  _retained_groups.abandon();
  _candidate_groups.abandon();
  for (uint i = 0; i < _max_regions; i++) {
    _contains_map[i] = CandidateOrigin::Invalid;
  }
  _last_marking_candidates_length = 0;
}

void G1CollectionSetCandidates::sort_marking_by_efficiency() {
  for(G1CollectionGroup* gr : _candidate_groups){
    gr->calculate_efficiency();
  }
  _candidate_groups.sort_by_efficiency();

  _candidate_groups.verify();
}

void G1CollectionSetCandidates::set_candidates_from_marking(G1CollectionSetCandidateInfo* candidate_infos,
                                                            uint num_infos) {
  if(num_infos == 0) {
    log_debug(gc, ergo, cset) ("No regions selected from marking.");
    return;
  }

  assert(_candidate_groups.length() == 0, "must be empty at the start of a cycle");
  verify();

  G1Policy* p = G1CollectedHeap::heap()->policy();
  // During each Mixed GC, we must collect at least G1Policy::calc_min_old_cset_length regions to meet
  // the G1MixedGCCountTarget. For the first collection in a Mixed GC cycle, we can add all regions
  // required to meet this threshold to the same remset group. We are certain these will be collected in
  // the same MixedGC.
  uint group_limit = p->calc_min_old_cset_length(num_infos);

  uint num_added_to_group = 0;
  G1CollectionGroup* current = nullptr;

  current = new G1CollectionGroup(G1CollectedHeap::heap()->card_set_config());

  for (uint i = 0; i < num_infos; i++) {
    G1HeapRegion* r = candidate_infos[i]._r;
    assert(!contains(r), "must not contain region %u", r->hrm_index());
    _contains_map[r->hrm_index()] = CandidateOrigin::Marking;

    if (num_added_to_group == group_limit) {
      if (group_limit != G1CollectionGroup::GROUP_SIZE) {
        group_limit = G1CollectionGroup::GROUP_SIZE;
      }

      _candidate_groups.append(current);

      current = new G1CollectionGroup(G1CollectedHeap::heap()->card_set_config());
      num_added_to_group = 0;
    }
    current->add(candidate_infos[i]);
    num_added_to_group++;
  }

  _candidate_groups.append(current);

  assert(_candidate_groups.num_regions() == num_infos, "Must be!");

  log_debug(gc, ergo, cset) ("Finished creating %u collection groups from %u regions", _candidate_groups.length(), num_infos);
  _last_marking_candidates_length = num_infos;

  verify();
}

void G1CollectionSetCandidates::sort_by_efficiency() {
  // From marking regions must always be sorted so no reason to actually sort
  // them.
  _candidate_groups.verify();
  _retained_groups.sort_by_efficiency();
  _retained_groups.verify();
}

void G1CollectionSetCandidates::add_retained_region_unsorted(G1HeapRegion* r) {
  assert(!contains(r), "must not contain region %u", r->hrm_index());
  _contains_map[r->hrm_index()] = CandidateOrigin::Retained;

  G1CollectionGroup* gr = new G1CollectionGroup(G1CollectedHeap::heap()->card_set_config());
  gr->add(r);

  log_error(gc) ("Add to retained " PTR_FORMAT, p2i(gr));
  _retained_groups.append(gr);
}

void G1CollectionSetCandidates::reset_region(G1HeapRegion* r) {
  assert(contains(r), "must contain region %u", r->hrm_index());
  _contains_map[r->hrm_index()] = CandidateOrigin::Invalid;
}

bool G1CollectionSetCandidates::is_empty() const {
  return length() == 0;
}

bool G1CollectionSetCandidates::has_more_marking_candidates() const {
  return marking_groups_length() != 0;
}

uint G1CollectionSetCandidates::marking_groups_length() const {
  return _candidate_groups.num_regions();
}

uint G1CollectionSetCandidates::retained_regions_length() const {
  return _retained_groups.num_regions();
}

#ifndef PRODUCT
void G1CollectionSetCandidates::verify_region(G1HeapRegion* r, CandidateOrigin* verify_map, CandidateOrigin expected_origin) {
  const uint hrm_index = r->hrm_index();
  assert(_contains_map[hrm_index] == expected_origin,
        "must be %u is %u", hrm_index, (uint)_contains_map[hrm_index]);
  assert(verify_map[hrm_index] == CandidateOrigin::Invalid, "already added");

  verify_map[hrm_index] = CandidateOrigin::Verify;
}


void G1CollectionSetCandidates::verify_retained_regions(uint& from_marking, CandidateOrigin* verify_map) {
  for (G1CollectionGroup* group : _retained_groups) {
    const GrowableArray<G1CollectionSetCandidateInfo>* regions = group->regions();

    for (int i = 0; i < regions->length(); i++) {
      G1HeapRegion* r = regions->at(i)._r;

      if (is_from_marking(r)) {
        from_marking++;
      }
      verify_region(r, verify_map, CandidateOrigin::Retained);
    }
  }
}

void G1CollectionSetCandidates::verify_candidate_groups(uint& from_marking, CandidateOrigin* verify_map) {
  for (G1CollectionGroup* group : _candidate_groups) {
    const GrowableArray<G1CollectionSetCandidateInfo>* regions = group->regions();

    for (int i = 0; i < regions->length(); i++) {
      G1HeapRegion* r = regions->at(i)._r;

      if (is_from_marking(r)) {
        from_marking++;
      }
      verify_region(r, verify_map, CandidateOrigin::Marking);
    }
  }
}

void G1CollectionSetCandidates::verify() {
  CandidateOrigin* verify_map = NEW_C_HEAP_ARRAY(CandidateOrigin, _max_regions, mtGC);
  for (uint i = 0; i < _max_regions; i++) {
    verify_map[i] = CandidateOrigin::Invalid;
  }

  uint from_marking = 0;
  verify_candidate_groups(from_marking, verify_map);
  assert(from_marking == marking_groups_length(), "must be");

  uint from_marking_retained = 0;
  verify_retained_regions(from_marking_retained, verify_map);
  assert(from_marking_retained == 0, "must be");

  assert(length() >= marking_groups_length(), "must be");

  // Check whether the _contains_map is consistent with the list.
  for (uint i = 0; i < _max_regions; i++) {
    assert(_contains_map[i] == verify_map[i] ||
           (_contains_map[i] != CandidateOrigin::Invalid && verify_map[i] == CandidateOrigin::Verify),
           "Candidate origin does not match for region %u, is %u but should be %u",
           i,
           static_cast<std::underlying_type<CandidateOrigin>::type>(_contains_map[i]),
           static_cast<std::underlying_type<CandidateOrigin>::type>(verify_map[i]));
  }

  FREE_C_HEAP_ARRAY(CandidateOrigin, verify_map);
}
#endif

bool G1CollectionSetCandidates::contains(const G1HeapRegion* r) const {
  const uint index = r->hrm_index();
  assert(index < _max_regions, "must be");
  return _contains_map[index] != CandidateOrigin::Invalid;
}

const char* G1CollectionSetCandidates::get_short_type_str(const G1HeapRegion* r) const {
  static const char* type_strings[] = {
    "Ci",  // Invalid
    "Cm",  // Marking
    "Cr",  // Retained
    "Cv"   // Verification
  };

  uint8_t kind = static_cast<std::underlying_type<CandidateOrigin>::type>(_contains_map[r->hrm_index()]);
  return type_strings[kind];
}
