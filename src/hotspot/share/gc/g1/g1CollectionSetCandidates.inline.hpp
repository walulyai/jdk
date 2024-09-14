/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1COLLECTIONSETCANDIDATES_INLINE_HPP
#define SHARE_GC_G1_G1COLLECTIONSETCANDIDATES_INLINE_HPP

#include "gc/g1/g1CollectionSetCandidates.hpp"

#include "utilities/growableArray.hpp"

inline G1CollectionCandidateListIterator::G1CollectionCandidateListIterator(G1CollectionCandidateList* which, uint position) :
  _which(which), _position(position) { }

inline G1CollectionCandidateListIterator& G1CollectionCandidateListIterator::operator++() {
  assert(_position < _which->length(), "must be");
  _position++;
  return *this;
}

inline G1CollectionSetCandidateInfo* G1CollectionCandidateListIterator::operator*() {
  return &_which->_candidates.at(_position);
}

inline bool G1CollectionCandidateListIterator::operator==(const G1CollectionCandidateListIterator& rhs) {
  assert(_which == rhs._which, "iterator belongs to different array");
  return _position == rhs._position;
}

inline bool G1CollectionCandidateListIterator::operator!=(const G1CollectionCandidateListIterator& rhs) {
  return !(*this == rhs);
}

inline G1CollectionCandidateGroupsListIterator::G1CollectionCandidateGroupsListIterator(G1CollectionCandidateGroupsList* which, uint position) :
  _which(which), _position(position) { }

inline G1CollectionCandidateGroupsListIterator& G1CollectionCandidateGroupsListIterator::operator++() {
  assert(_position < _which->length(), "must be");
  _position++;
  return *this;
}

inline G1CollectionGroup* G1CollectionCandidateGroupsListIterator::operator*() {
  return _which->at(_position);
}

inline bool G1CollectionCandidateGroupsListIterator::operator==(const G1CollectionCandidateGroupsListIterator& rhs) {
  assert(_which == rhs._which, "iterator belongs to different array");
  return _position == rhs._position;
}

inline bool G1CollectionCandidateGroupsListIterator::operator!=(const G1CollectionCandidateGroupsListIterator& rhs) {
  return !(*this == rhs);
}

template<typename Func>
void G1CollectionSetCandidates::iterate_regions(Func&& f) {
  for (G1CollectionGroup* group : _candidate_groups) {
    const GrowableArray<G1HeapRegion*>* regions = group->regions();

    for (int i = 0; i < regions->length(); i++) {
      G1HeapRegion* r = regions->at(i);
      f(r);
    }
  }

  for (uint i = 0; i < (uint)_retained_regions.length(); i++) {
    G1HeapRegion* r =_retained_regions.at(i)._r;
    f(r);
  }
}

#endif /* SHARE_GC_G1_G1COLLECTIONSETCANDIDATES_INLINE_HPP */
