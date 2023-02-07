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
#include "gc/g1/g1FullCollector.inline.hpp"
#include "gc/g1/g1FullGCCompactionPoint.hpp"
#include "gc/g1/heapRegion.hpp"
#include "gc/shared/preservedMarks.inline.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/debug.hpp"

G1FullGCCompactionPoint::G1FullGCCompactionPoint(G1FullCollector* collector) :
    _collector(collector),
    _current_region(nullptr),
    _compaction_top(nullptr) {
  _compaction_regions = new (mtGC) GrowableArray<HeapRegion*>(32, mtGC);
  _compaction_region_iterator = _compaction_regions->begin();
}

G1FullGCCompactionPoint::~G1FullGCCompactionPoint() {
  delete _compaction_regions;
}

void G1FullGCCompactionPoint::update() {
  if (is_initialized()) {
    _collector->set_compaction_top(_current_region, _compaction_top);
  }
}

void G1FullGCCompactionPoint::initialize_values() {
  _compaction_top = _collector->compaction_top(_current_region);
}

bool G1FullGCCompactionPoint::has_regions() {
  return !_compaction_regions->is_empty();
}

bool G1FullGCCompactionPoint::is_initialized() {
  return _current_region != NULL;
}

void G1FullGCCompactionPoint::initialize(HeapRegion* hr) {
  _current_region = hr;
  initialize_values();
}

HeapRegion* G1FullGCCompactionPoint::current_region() {
  return *_compaction_region_iterator;
}

HeapRegion* G1FullGCCompactionPoint::next_region() {
  HeapRegion* next = *(++_compaction_region_iterator);
  assert(next != NULL, "Must return valid region");
  return next;
}

GrowableArray<HeapRegion*>* G1FullGCCompactionPoint::regions() {
  return _compaction_regions;
}

 void G1FullGCCompactionPoint::sort_regions(){
    regions()->sort([](HeapRegion** a, HeapRegion** b) {
      return static_cast<int>((*a)->hrm_index() - (*b)->hrm_index());
    });
  }

bool G1FullGCCompactionPoint::object_will_fit(size_t size) {
  size_t space_left = pointer_delta(_current_region->end(), _compaction_top);
  return size <= space_left;
}

void G1FullGCCompactionPoint::switch_region() {
  // Save compaction top in the region.
  _collector->set_compaction_top(_current_region, _compaction_top);
  // Get the next region and re-initialize the values.
  _current_region = next_region();
  initialize_values();
}

void G1FullGCCompactionPoint::forward(oop object, size_t size) {
  assert(_current_region != NULL, "Must have been initialized");

  // Ensure the object fit in the current region.
  while (!object_will_fit(size)) {
    switch_region();
  }

  // Store a forwarding pointer if the object should be moved.
  if (cast_from_oop<HeapWord*>(object) != _compaction_top) {
    object->forward_to(cast_to_oop(_compaction_top));
    assert(object->is_forwarded(), "must be forwarded");
  } else {
    assert(!object->is_forwarded(), "must not be forwarded");
  }

  // Update compaction values.
  _compaction_top += size;
  _current_region->update_bot_for_block(_compaction_top - size, _compaction_top);
}

void G1FullGCCompactionPoint::add(HeapRegion* hr) {
  _compaction_regions->append(hr);
}

HeapRegion* G1FullGCCompactionPoint::remove_last() {
  return _compaction_regions->pop();
}

void G1FullGCCompactionPoint::truncate_from_current(G1FullGCCompactionPoint* serial_cp) {
  HeapRegion* cur = current_region();
  int index_cur = _compaction_regions->find(cur);

  for (; _compaction_region_iterator != _compaction_regions->end(); ++_compaction_region_iterator) {
    serial_cp->add(*_compaction_region_iterator);
  }
  _compaction_regions->trunc_to(index_cur);
}

bool G1FullGCCompactionPoint::copy_after_current(G1FullGCCompactionPoint* cp) {
  if (_current_region == _compaction_regions->last()) {
    return false; // No regions left
  }

  switch_region();

  for (; _compaction_region_iterator != _compaction_regions->end(); ++_compaction_region_iterator) {
    cp->add(*_compaction_region_iterator);
  }
  return true;
}

void G1FullGCCompactionPoint::forward_humongous(HeapRegion* hr) {
  assert(_current_region != NULL, "Must have been initialized");
  assert(hr->is_starts_humongous(), "Must be!");

  oop obj = cast_to_oop(hr->bottom());
  size_t obj_size = obj->size();
  int num_regions = (int) G1CollectedHeap::humongous_obj_size_in_regions(obj_size);

  Pair<uint, uint> range = find_contiguous_before(hr, num_regions);

  uint range_begin = range.first;
  uint range_end = range.second;

  if (range_begin != range_end) { // Object can be relocated
    // Region was initially not compacting, so we didn't preserve the mark.
   _collector->marker(0)->preserved_stack()->push_if_necessary(obj, obj->mark());

    HeapRegion* start = _compaction_regions->at(range_begin);

    obj->forward_to(cast_to_oop(start->bottom()));
    assert(obj->is_forwarded(), "Must be!");
    _collector->update_from_skip_compacting_to_compacting(hr->hrm_index());

    log_trace(gc, region) ("Forward Region: from %u to %u - %u num_regions %u ",
                           hr->hrm_index(), start->hrm_index(), _compaction_regions->at(range_begin + num_regions - 1)->hrm_index(), num_regions);
  } else {
    log_trace(gc, region) ("Region Not Moving: %u num_regions %u ", hr->hrm_index(), num_regions);
  }

  // Remove covered regions from candidancy
  // Note that range_end doesn't imply the object end, object can be relocated
  // and overlap with its previous regions.
  _compaction_regions->erase(range_begin, (range_begin + num_regions));
}

Pair<uint, uint> G1FullGCCompactionPoint::find_contiguous_before(HeapRegion* hr, uint num_regions) {
  assert(num_regions > 0, "Must be");

  if (num_regions == 1) {
    return (_compaction_regions->at(0) == hr) ? Pair<uint, uint> (0, 0) : Pair<uint, uint> (0, 1);
  }

  uint length = 1;
  uint range_end = 1;
  uint range_limit = (uint)_compaction_regions->find(hr);

  for (; range_end <= range_limit; range_end++) {
    if (length == num_regions) {
      break;
    }
    if (_compaction_regions->at(range_end)->hrm_index() - _compaction_regions->at(range_end - 1)->hrm_index() != 1) {
      length = 1;
    } else {
      length++;
    }
  }
 return Pair<uint, uint> (range_end - length, range_end -1);
}