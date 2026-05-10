/*
 * Copyright (c) 2001, 2026, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1CONCURRENTSTARTTOMIXEDTIMETRACKER_HPP
#define SHARE_GC_G1_G1CONCURRENTSTARTTOMIXEDTIMETRACKER_HPP

#include "gc/g1/g1CollectorState.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

struct MutatorPeriodStatsBytes {
    size_t _old_gen_growth;
    size_t _non_hum_allocated;
    size_t _hum_allocated;
    size_t _total_hum_before;
    size_t _total_hum_after;

    MutatorPeriodStatsBytes(size_t old_gen_growth,
                            size_t non_hum_allocated,
                            size_t hum_allocated,
                            size_t total_hum_before,
                            size_t total_hum_after)
      : _old_gen_growth(old_gen_growth),
      _non_hum_allocated(non_hum_allocated),
      _hum_allocated(hum_allocated),
      _total_hum_before(total_hum_before),
      _total_hum_after(total_hum_after)
    { }
};

// TODO: add comments on what we consider a Concurrent Cycle
struct ConcurrentCycleStats {
    double _cycle_duration_s;
    size_t _non_hum_allocated_bytes;
    size_t _peak_extra_humongous_allocated;
    ConcurrentCycleStats(double cycle_duration_s,
                         size_t non_hum_allocated_bytes,
                         size_t peak_extra_humongous_allocated)
    : _cycle_duration_s(cycle_duration_s),
      _non_hum_allocated_bytes(non_hum_allocated_bytes),
      _peak_extra_humongous_allocated(peak_extra_humongous_allocated)
    { }
};

class G1ConcurrentCycleTracker{
  using Pause = G1CollectorState::Pause;
  enum class CycleState {
    InActive,
    Active,
    Complete,
  };

  CycleState _state;
  double _cycle_start_time;
  double _cycle_end_time;
  double _total_gc_pauses_in_cycle;

  // allocation accounting
public:
  size_t _hum_bytes_at_start;
  size_t _non_hum_bytes_allocated;
  intptr_t _peak_extra_humongous_reserve_bytes;
private:

  void reset() {
    _state = CycleState::InActive;
    _total_gc_pauses_in_cycle = 0.0;
    _cycle_start_time = 1.0;
    _cycle_end_time = 1.0;

    _hum_bytes_at_start = 0;
    _non_hum_bytes_allocated = 0;
    _peak_extra_humongous_reserve_bytes = 0;
  }

  bool is_active() const {
    return _state == CycleState::Active;
  }
  void update_mutator_stats(double pause_duration, MutatorPeriodStatsBytes period_stats) {
    if (!is_active()) {
      return;
    }

    _total_gc_pauses_in_cycle += pause_duration;
    _non_hum_bytes_allocated += period_stats._non_hum_allocated;

    intptr_t delta_before_mutator_period = checked_cast<intptr_t>(period_stats._total_hum_before) -
                                           checked_cast<intptr_t>(_hum_bytes_at_start);

    intptr_t delta_after_mutator_period = delta_before_mutator_period +
                                          checked_cast<intptr_t>(period_stats._hum_allocated);

    if (delta_after_mutator_period > 0) {
      _peak_extra_humongous_reserve_bytes = MAX2(_peak_extra_humongous_reserve_bytes, delta_after_mutator_period);
    }
  }

 public:
  G1ConcurrentCycleTracker() :
   _state(CycleState::InActive),
   _cycle_start_time(0.0),
   _cycle_end_time(0.0),
   _total_gc_pauses_in_cycle(0.0),
   _hum_bytes_at_start(0),
   _non_hum_bytes_allocated(0),
   _peak_extra_humongous_reserve_bytes(0)
  { }

  void record_cycle_start(double start_time, size_t humongous_bytes_after_gc) {
    _cycle_start_time = start_time;
    _hum_bytes_at_start = humongous_bytes_after_gc;
    _state = CycleState::Active;
    postcond(is_active());
  }

  void add_allocated_non_hum(size_t bytes) {
    if (!is_active()) { return; }

    _non_hum_bytes_allocated += bytes;
  }

  void record_mutator_period(Pause gc_type,
                             double start,
                             double end,
                             MutatorPeriodStatsBytes period_stats) {
    // Manage the mutator time tracking from concurrent start to first mixed gc.
    update_mutator_stats(end - start, period_stats);

    switch (gc_type) {
      case Pause::Full:
      case Pause::ConcurrentStartUndo:
        abort_cycle();
        break;
      case Pause::Cleanup:
      case Pause::Remark:
      case Pause::Normal:
      case Pause::PrepareMixed:
        break;
      case Pause::ConcurrentStartFull:
        // Do not track time-to-mixed time for periodic collections as they are likely
        // to be not representative to regular operation as the mutators are idle at
        // that time. Also only track full concurrent mark cycles.
        // if (_g1h->gc_cause() != GCCause::_g1_periodic_collection)
        {
          record_cycle_start(end, period_stats._total_hum_after);
        }
        break;
      case Pause::Mixed:
        if (is_active()) {
          // TODO: we track the first mixed-gc
          complete_cycle(start, end - start);
        }
        break;
      default:
        ShouldNotReachHere();
    }

  }

  void complete_cycle(double cycle_end_time, double mixed_gc_duration) {
    precond(is_active());
    // TODO: add a comment
    _total_gc_pauses_in_cycle -= mixed_gc_duration;

    _cycle_end_time = cycle_end_time;
    _state = CycleState::Complete;
  }

  void abort_cycle() {
    reset();
  }

  bool has_completed_cycle() const {
    return _state == CycleState::Complete;
  }

  ConcurrentCycleStats get_and_reset_cycle_stats() {
    precond(has_completed_cycle());
    double cycle_duration = (_cycle_end_time - _cycle_start_time - _total_gc_pauses_in_cycle);

    ConcurrentCycleStats stats{
      cycle_duration,
      _non_hum_bytes_allocated,
      checked_cast<size_t>(_peak_extra_humongous_reserve_bytes)
    };

    reset();
    return stats;
  }

  size_t peak_extra_humongous_reserve_bytes() const {
    return checked_cast<size_t>(_peak_extra_humongous_reserve_bytes);
  }
};

// Used to track time from the end of concurrent start to the first mixed GC.
// After calling the concurrent start/mixed gc notifications, the result can be
// obtained in get_and_reset_last_marking_time() once, after which the tracking resets.
// Any pauses recorded by add_pause() will be subtracted from that results.
class G1ConcurrentStartToMixedTimeTracker {
  bool _active;
  double _concurrent_start_end_time;
  double _mixed_start_time;
  double _total_pause_time;

  double wall_time() const {
    return _mixed_start_time - _concurrent_start_end_time;
  }
public:
  G1ConcurrentStartToMixedTimeTracker() { reset(); }

  // Record concurrent start pause end, starting the time tracking.
  void record_concurrent_start_end(double end_time) {
    assert(!_active, "Concurrent start out of order.");
    _concurrent_start_end_time = end_time;
    _active = true;
  }

  // Record the first mixed gc pause start, ending the time tracking.
  void record_mixed_gc_start(double start_time) {
    if (_active) {
      _mixed_start_time = start_time;
      _active = false;
    }
  }

  double get_and_reset_last_marking_time() {
    assert(has_result(), "Do not have all measurements yet.");
    double result = (_mixed_start_time - _concurrent_start_end_time) - _total_pause_time;
    reset();
    return result;
  }

  void reset() {
    _active = false;
    _total_pause_time = 0.0;
    _concurrent_start_end_time = 0.0;
    _mixed_start_time = 0.0;
  }

  void add_pause(double time) {
    if (_active) {
      _total_pause_time += time;
    }
  }

  bool is_active() const { return _active; }

  // Returns whether we have a result that can be retrieved.
  bool has_result() const { return _mixed_start_time > 0.0 && _concurrent_start_end_time > 0.0; }
};

#endif // SHARE_GC_G1_G1CONCURRENTSTARTTOMIXEDTIMETRACKER_HPP
