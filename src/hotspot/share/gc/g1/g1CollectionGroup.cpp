/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1CollectionGroup.hpp"

G1CollectionGroup::G1CollectionGroup(G1CardSetConfiguration* config) :
  _candidates(4, mtGCCardSet),
  _card_set_mm(config, G1CollectedHeap::heap()->card_set_freelist_pool()),
  _card_set(config, &_card_set_mm),
  _gc_efficiency(0.0) {

}

void G1CollectionGroup::add(G1HeapRegion* hr) {
  G1CollectionSetCandidateInfo c(hr, hr->calc_gc_efficiency());
  add(c);
}

void G1CollectionGroup::add(G1CollectionSetCandidateInfo &hr_info) {
  G1HeapRegion* hr = hr_info._r;
  assert(!hr->is_young(), "should be flagged as survivor region");

  _candidates.append(hr_info);
  hr->install_group_cardset(&_card_set);
}

void G1CollectionGroup::calculate_efficiency() {
  size_t reclaimable_bytes = 0;
  uint num_candidates = _candidates.length();
  for (uint i = 0; i < num_candidates; i++) {
    G1HeapRegion* hr = region_at(i);
    reclaimable_bytes += hr->reclaimable_bytes();
  }

  double group_total_time_ms = predict_group_total_time_ms();
  _gc_efficiency = reclaimable_bytes / group_total_time_ms;
}

void G1CollectionGroup::clear() {
    _card_set.clear();
    _candidates.clear();
  }

void G1CollectionGroup::abandon() {
  uint num_candidates = _candidates.length();
  for (uint i = 0; i < num_candidates; i++) {
    G1HeapRegion* r = region_at(i);
    r->uninstall_group_cardset();
    r->rem_set()->clear(true /* only_cardset */);
  }
  clear();
  }

double G1CollectionGroup::predict_group_copy_time_ms() const {
  G1Policy* p = G1CollectedHeap::heap()->policy();

  double predicted_region_copy_time_ms = 0.0;
  double predict_region_code_root_scan_time = 0.0;

  uint num_candidates = _candidates.length();
  for (uint i = 0; i < num_candidates; i++) {
    G1HeapRegion* r = region_at(i);
    assert(r->rem_set()->card_set() == &_card_set, "Must be!");

    predicted_region_copy_time_ms += p->predict_region_copy_time_ms(r, false /* for_young_only_phase */);
    predict_region_code_root_scan_time += p->predict_region_code_root_scan_time(r, false /* for_young_only_phase */);
  }

  return predicted_region_copy_time_ms + predict_region_code_root_scan_time;
}

double G1CollectionGroup::predict_group_total_time_ms() const {
  G1Policy* p = G1CollectedHeap::heap()->policy();

  size_t card_rs_length = _card_set.occupied();

  double predicted_region_evac_time_ms = p->predict_merge_scan_time(card_rs_length);
  predicted_region_evac_time_ms += predict_group_copy_time_ms();
  predicted_region_evac_time_ms += p->predict_non_young_other_time_ms(length());

  return predicted_region_evac_time_ms;
}


int G1CollectionGroup::compare_gc_efficiency(G1CollectionSetCandidateInfo* ci1, G1CollectionSetCandidateInfo* ci2) {
  assert(ci1->_r != nullptr && ci2->_r != nullptr, "Should not be!");

  double gc_eff1 = ci1->_gc_efficiency;
  double gc_eff2 = ci2->_gc_efficiency;

  if (gc_eff1 > gc_eff2) {
    return -1;
  } else if (gc_eff1 < gc_eff2) {
    return 1;
  } else {
    return 0;
  }
}

int G1CollectionGroup::compare_reclaimble_bytes(G1CollectionSetCandidateInfo* ci1, G1CollectionSetCandidateInfo* ci2) {
  // Make sure that null entries are moved to the end.
  if (ci1->_r == nullptr) {
    if (ci2->_r == nullptr) {
      return 0;
    } else {
      return 1;
    }
  } else if (ci2->_r == nullptr) {
    return -1;
  }

  size_t reclaimable1 = ci1->_r->reclaimable_bytes();
  size_t reclaimable2 = ci2->_r->reclaimable_bytes();

  if (reclaimable1 > reclaimable2) {
    return -1;
  } else if (reclaimable1 < reclaimable2) {
    return 1;
  } else {
    return 0;
  }
}
