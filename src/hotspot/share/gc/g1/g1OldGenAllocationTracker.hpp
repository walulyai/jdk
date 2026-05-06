/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
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

#ifndef SHARE_VM_GC_G1_G1OLDGENALLOCATIONTRACKER_HPP
#define SHARE_VM_GC_G1_G1OLDGENALLOCATIONTRACKER_HPP

#include "gc/g1/g1ConcurrentStartToMixedTimeTracker.hpp"
#include "gc/g1/g1HeapRegion.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"

enum class ConcurrentCycleMode {
  NotInCycle,
  StartCycle,
  InCycle,
  EndCycle,
};

// Track allocation details in the old generation.
class G1OldGenAllocationTracker : public CHeapObj<mtGC> {
  // Total number of bytes allocated in the old generation at the end
  // of the last gc.
  size_t _last_period_old_gen_bytes;
  // Total growth of the old geneneration since the last gc,
  // taking eager-reclaim into consideration.
  size_t _last_period_old_gen_growth;

  // Total size of humongous objects for last gc.
  size_t _humongous_bytes_after_last_gc;

  // Non-humongous old generation allocations since the last gc.
  size_t _allocated_bytes_since_last_gc;
  // Humongous allocations during last mutator period.
  size_t _allocated_humongous_bytes_since_last_gc;

  // Track allocations during a Concurrent cycle [Concurrent Mark Start - First Mixed GC]
  class ConcurrentCycleState {
    size_t _non_humongous_bytes = 0;
    size_t _humongous_bytes_at_start = 0;
    intptr_t _peak_extra_humongous_reserve_bytes = 0;
    bool _active = false;

    void start_cycle(size_t humongous_bytes_after_gc) {
      precond(!_active);
      _humongous_bytes_at_start = humongous_bytes_after_gc;
      _non_humongous_bytes = 0;
      _peak_extra_humongous_reserve_bytes = 0;
      _active = true;
    }

    void record_in_cycle_allocations(size_t non_humongous_allocated_bytes,
                                     size_t humongous_allocated_bytes,
                                     size_t humongous_bytes_after_previous_gc,
                                     size_t humongous_bytes_after_gc) {
      precond(is_active());
      _non_humongous_bytes += non_humongous_allocated_bytes;

      intptr_t delta_after_previous_gc = checked_cast<intptr_t>(humongous_bytes_after_previous_gc) -
                                         checked_cast<intptr_t>(_humongous_bytes_at_start);

      intptr_t delta_before_this_gc = delta_after_previous_gc +
                                      checked_cast<intptr_t>(humongous_allocated_bytes);

      if (delta_before_this_gc > 0) {
        _peak_extra_humongous_reserve_bytes = MAX2(_peak_extra_humongous_reserve_bytes, delta_before_this_gc);
      }
    }

    void end_cycle() {
      precond(_active);
      _active = false;
    }

    bool is_active() const { return _active; }

  public:
    void record_mutator_period(ConcurrentCycleMode mode,
                               size_t non_humongous_allocated_bytes,
                               size_t humongous_allocated_bytes,
                               size_t humongous_bytes_after_previous_gc,
                               size_t humongous_bytes_after_gc) {
      if (mode == ConcurrentCycleMode::StartCycle) {
        start_cycle(humongous_bytes_after_gc);
      } else if (mode == ConcurrentCycleMode::InCycle || mode == ConcurrentCycleMode::EndCycle) {
        record_in_cycle_allocations(non_humongous_allocated_bytes,
                                    humongous_allocated_bytes,
                                    humongous_bytes_after_previous_gc,
                                    humongous_bytes_after_gc);
        if (mode == ConcurrentCycleMode::EndCycle) {
          end_cycle();
        }
      } else {
        precond(!is_active());
      }
    }

    void abort_cycle() {
      _active = false;
    }

    size_t peak_extra_humongous_reserve_bytes() const {
      return checked_cast<size_t>(_peak_extra_humongous_reserve_bytes);
    }

    size_t non_humongous_bytes() const {
      return _non_humongous_bytes;
    }
  } _conc_cycle;

public:
  G1OldGenAllocationTracker();

  void add_allocated_bytes_since_last_gc(size_t bytes) {
    _allocated_bytes_since_last_gc += bytes;
  }
  void add_allocated_humongous_bytes_since_last_gc(size_t bytes) {
    _allocated_humongous_bytes_since_last_gc += bytes;
  }

  // Record a humongous allocation in a collection pause. This allocation
  // is accounted to the previous mutator period.
  void record_collection_pause_humongous_allocation(size_t bytes) {
    _humongous_bytes_after_last_gc += bytes;
  }


  size_t last_period_old_gen_bytes() const { return _last_period_old_gen_bytes; }
  size_t last_period_old_gen_growth() const { return _last_period_old_gen_growth; }

  // Calculates and resets stats after a collection.
  void reset_after_gc(size_t humongous_bytes_after_gc, ConcurrentCycleMode cycle_mode);

  size_t peak_extra_humongous_reserve_bytes() const {
    return _conc_cycle.peak_extra_humongous_reserve_bytes();
  }

  size_t non_humongous_bytes() const { return _conc_cycle.non_humongous_bytes(); }

  void abort_concurrent_cycle() { _conc_cycle.abort_cycle(); }
};

#endif // SHARE_VM_GC_G1_G1OLDGENALLOCATIONTRACKER_HPP
