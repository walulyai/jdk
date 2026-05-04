/*
 * Copyright (c) 2016, 2026, Oracle and/or its affiliates. All rights reserved.
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
 */

#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1IHOPControl.hpp"
#include "gc/g1/g1OldGenAllocationTracker.hpp"
#include "gc/g1/g1Predictions.hpp"
#include "unittest.hpp"

struct G1IHOPTestController {
  G1ConcurrentStartToMixedTimeTracker _cycle_time_tracker;
  G1OldGenAllocationTracker _alloc_tracker;
  G1Predictions _pred;
  G1IHOPControl _ihop_control;
  const double cycle_start_time = 1.0;
  size_t _last_humongous_bytes_after_gc = 0;

  G1IHOPTestController(bool adaptive, size_t ihop, size_t target_occupancy)
   : _cycle_time_tracker(),
     _alloc_tracker(&_cycle_time_tracker),
     _pred(0.50),
     _ihop_control(ihop, adaptive, &_pred, 0 /* heap_reserve_percent */, 0 /* heap_waste_percent */)
  {
    _ihop_control.update_target_occupancy(target_occupancy);
  }

  void set_post_gc_humongous_state(size_t desired_hum_after_gc) {
    if (desired_hum_after_gc > _last_humongous_bytes_after_gc) {
      _alloc_tracker.add_allocated_humongous_bytes_since_last_gc(desired_hum_after_gc - _last_humongous_bytes_after_gc);
    }
    _alloc_tracker.reset_after_gc(desired_hum_after_gc, false /* is_concurrent_start */);
    _last_humongous_bytes_after_gc = desired_hum_after_gc;
  }

  void start_cycle(size_t humongous_bytes_after_gc = 0) {
    if (_last_humongous_bytes_after_gc != humongous_bytes_after_gc) {
      set_post_gc_humongous_state(humongous_bytes_after_gc);
    }
    _cycle_time_tracker.record_concurrent_start_end(cycle_start_time);
    _alloc_tracker.reset_after_gc(humongous_bytes_after_gc, true /* is_concurrent_start*/);
  }

  void record_young_gc(double mutator_time_s, size_t young_reserve,
                       size_t non_hum_bytes, size_t hum_bytes, size_t hum_after_gc) {
    _alloc_tracker.add_allocated_bytes_since_last_gc(non_hum_bytes);
    _alloc_tracker.add_allocated_humongous_bytes_since_last_gc(hum_bytes);
    _alloc_tracker.reset_after_gc(hum_after_gc, false /* is_concurrent_start */);
    _last_humongous_bytes_after_gc = hum_after_gc;
    _ihop_control.record_last_mutator_period(mutator_time_s, _alloc_tracker.last_period_old_gen_growth(), young_reserve);
  }

  void complete_cycle(double cycle_duration) {
    _cycle_time_tracker.record_mixed_gc_start(cycle_start_time + cycle_duration);
    double last_cycle_time = _cycle_time_tracker.get_and_reset_last_marking_time();
    EXPECT_EQ(last_cycle_time, cycle_duration);
    _ihop_control.record_concurrent_cycle(last_cycle_time,
                                          _alloc_tracker.non_humongous_bytes(),
                                          _alloc_tracker.peak_extra_humongous_reserve_bytes());
  }

  void add_cycle_sample(double cycle_duration, size_t young_reserve, size_t non_hum_bytes, size_t peak_hum_bytes) {
    start_cycle();
    record_young_gc(1.0 /* mutator_time_s */, young_reserve, non_hum_bytes, peak_hum_bytes, peak_hum_bytes);
    complete_cycle(cycle_duration);
  }

  size_t threshold() {
    return _ihop_control.old_gen_threshold_for_conc_mark_start();
  }
};

static void add_identical_samples(G1IHOPTestController* ctrl,
                                  double cycle_duration_s,
                                  size_t young_reserve_bytes,
                                  size_t non_hum_bytes,
                                  size_t hum_alloc_bytes,
                                  size_t num_samples) {
  for (size_t i = 0; i < num_samples; i++) {
    ctrl->add_cycle_sample(cycle_duration_s,
                           young_reserve_bytes,
                           non_hum_bytes,
                           hum_alloc_bytes);
  }
}

static size_t old_gen_threshold(size_t target_occupancy,
                                size_t young_reserve,
                                size_t non_hum_bytes,
                                size_t peak_hum_bytes) {
  size_t needed_during_cycle = young_reserve + non_hum_bytes + peak_hum_bytes;
  return needed_during_cycle < target_occupancy ?
         target_occupancy - needed_during_cycle : 0;
}

// @requires UseG1GC
TEST_VM(G1IHOPControl, allocation_tracker_incr) {
  // Test requires G1
  if (!UseG1GC) {
    return;
  }

  size_t initial_ihop = InitiatingHeapOccupancyPercent;
  G1IHOPTestController ctrl(false /* adaptive */, initial_ihop, 100 /* target_occupancy */);

  ctrl.start_cycle();

  ctrl.record_young_gc(1.0, 0, 20, 30, 25);

  EXPECT_EQ(20u, ctrl._alloc_tracker.non_humongous_bytes());
  EXPECT_EQ(30u, ctrl._alloc_tracker.peak_extra_humongous_reserve_bytes());

  // Peak Humongous should be:
  //  hum_after_gc (from previous gc) + hum_alloc ()
  ctrl.record_young_gc(1.0, 0, 5, 10, 10);

  EXPECT_EQ(25u, ctrl._alloc_tracker.non_humongous_bytes());
  EXPECT_EQ(35u, ctrl._alloc_tracker.peak_extra_humongous_reserve_bytes());
}

// @requires UseG1GC
TEST_VM(G1IHOPControl, non_adaptive_ihop) {
  // Test requires G1
  if (!UseG1GC) {
    return;
  }

  size_t initial_ihop = InitiatingHeapOccupancyPercent;

  G1IHOPTestController ctrl(false /* adaptive */, initial_ihop, 100 /* target_occupancy */);

  add_identical_samples(&ctrl,
                        1.0 /* cycle_duration_s */,
                        10  /* young_reserve */,
                        20  /* non_hum_bytes */,
                        0   /* hum_alloc_bytes */,
                        100 /* num_samples */);

  EXPECT_EQ(initial_ihop, ctrl._ihop_control.old_gen_threshold_for_conc_mark_start());
}

TEST_VM(G1IHOPControl, adaptive_ihop_not_enough_samples) {
  // Test requires G1
  if (!UseG1GC) {
    return;
  }

  size_t initial_ihop = InitiatingHeapOccupancyPercent;

  G1IHOPTestController ctrl(true /* adaptive */, initial_ihop, 100 /* target_occupancy */);

  add_identical_samples(&ctrl,
                        1.0 /* cycle_duration_s */,
                        10  /* young_reserve */,
                        20  /* non_hum_bytes */,
                        0   /* hum_alloc_bytes */,
                        G1AdaptiveIHOPNumInitialSamples - 1 /* num_samples */);

  EXPECT_EQ(initial_ihop, ctrl._ihop_control.old_gen_threshold_for_conc_mark_start());
}


TEST_VM(G1IHOPControl, adaptive_ihop_non_humongous_only) {
  // Test requires G1
  if (!UseG1GC) {
    return;
  }

  size_t initial_ihop = InitiatingHeapOccupancyPercent;
  // G1Predictions require 5 or more samples to skip special considerations for
  // small samples.
  size_t num_samples = 5;

  G1IHOPTestController ctrl(true /* adaptive */, initial_ihop, 100 /* target_occupancy */);

  add_identical_samples(&ctrl,
                        1.0 /* cycle_duration_s */,
                        10  /* young_reserve */,
                        20  /* non_hum_bytes */,
                        0   /* hum_alloc_bytes */,
                        num_samples);

  size_t expected_threshold = old_gen_threshold(100 /* target_occupancy */,
                                                10  /* young_reserve */,
                                                20  /* non_hum_bytes */,
                                                0   /* peak_hum_bytes */);

  EXPECT_EQ(expected_threshold, ctrl._ihop_control.old_gen_threshold_for_conc_mark_start());
}

TEST_VM(G1IHOPControl, adaptive_ihop_peak_humongous_only) {
  // Test requires G1
  if (!UseG1GC) {
    return;
  }

  size_t initial_ihop = InitiatingHeapOccupancyPercent;
  // G1Predictions require 5 or more samples to skip special considerations for
  // small samples.
  size_t num_samples = 5;

  G1IHOPTestController ctrl(true /* adaptive */, initial_ihop, 100 /* target_occupancy */);

  add_identical_samples(&ctrl,
                        1.0 /* cycle_duration_s */,
                        10  /* young_reserve */,
                        0  /* non_hum_bytes */,
                        30   /* hum_alloc_bytes */,
                        num_samples);

  size_t expected_threshold = old_gen_threshold(100 /* target_occupancy */,
                                                10  /* young_reserve */,
                                                0  /* non_hum_bytes */,
                                                30 /* peak_hum_bytes */);

  EXPECT_EQ(expected_threshold, ctrl._ihop_control.old_gen_threshold_for_conc_mark_start());
}

TEST_VM(G1IHOPControl, adaptive_ihop_combined) {
  // Test requires G1
  if (!UseG1GC) {
    return;
  }

  size_t initial_ihop = InitiatingHeapOccupancyPercent;
  // G1Predictions require 5 or more samples to skip special considerations for
  // small samples.
  size_t num_samples = 5;

  G1IHOPTestController ctrl(true /* adaptive */, initial_ihop, 100 /* target_occupancy */);

  add_identical_samples(&ctrl,
                        1.0 /* cycle_duration_s */,
                        10  /* young_reserve */,
                        20  /* non_hum_bytes */,
                        30  /* hum_alloc_bytes */,
                        num_samples);

  size_t expected_threshold = old_gen_threshold(100 /* target_occupancy */,
                                                10  /* young_reserve */,
                                                20  /* non_hum_bytes */,
                                                30   /* peak_hum_bytes */);

  EXPECT_EQ(expected_threshold, ctrl._ihop_control.old_gen_threshold_for_conc_mark_start());
}

TEST_VM(G1IHOPControl, adaptive_ihop_high_alloc_pressure) {
  // Test requires G1
  if (!UseG1GC) {
    return;
  }

  size_t initial_ihop = InitiatingHeapOccupancyPercent;
  // G1Predictions require 5 or more samples to skip special considerations for
  // small samples.
  size_t num_samples = 5;

  G1IHOPTestController ctrl(true /* adaptive */, initial_ihop, 100 /* target_occupancy */);

  add_identical_samples(&ctrl,
                        1.0 /* cycle_duration_s */,
                        10  /* young_reserve */,
                        70  /* non_hum_bytes */,
                        35  /* hum_alloc_bytes */,
                        num_samples);

  size_t expected_threshold = 0;

  EXPECT_EQ(expected_threshold, ctrl._ihop_control.old_gen_threshold_for_conc_mark_start());
}

TEST_VM(G1IHOPControl, adaptive_ihop_young_reserve) {
  // Test requires G1
  if (!UseG1GC) {
    return;
  }

  size_t initial_ihop = InitiatingHeapOccupancyPercent;
  // G1Predictions require 5 or more samples to skip special considerations for
  // small samples.
  size_t num_samples = 5;

  G1IHOPTestController ctrl_small(true /* adaptive */, initial_ihop, 100 /* target_occupancy */);

  add_identical_samples(&ctrl_small,
                        1.0 /* cycle_duration_s */,
                        10  /* young_reserve */,
                        20  /* non_hum_bytes */,
                        30  /* hum_alloc_bytes */,
                        num_samples);

  size_t expected_small_young = old_gen_threshold(100 /* target_occupancy */,
                                                  10  /* young_reserve */,
                                                  20  /* non_hum_bytes */,
                                                  30  /* peak_hum_bytes */);

  G1IHOPTestController ctrl_large(true /* adaptive */, initial_ihop, 100 /* target_occupancy */);

  add_identical_samples(&ctrl_large,
                        1.0 /* cycle_duration_s */,
                        25  /* young_reserve */,
                        20  /* non_hum_bytes */,
                        30  /* hum_alloc_bytes */,
                        num_samples);

  size_t expected_large_young = old_gen_threshold(100 /* target_occupancy */,
                                                  25  /* young_reserve */,
                                                  20  /* non_hum_bytes */,
                                                  30  /* peak_hum_bytes */);

  EXPECT_EQ(expected_small_young, ctrl_small._ihop_control.old_gen_threshold_for_conc_mark_start());
  EXPECT_EQ(expected_large_young, ctrl_large._ihop_control.old_gen_threshold_for_conc_mark_start());

  EXPECT_LT(expected_large_young, expected_small_young);
}

TEST_VM(G1IHOPControl, adaptive_ihop_recovers_after_spike) {
  // Test requires G1
  if (!UseG1GC) {
    return;
  }

  size_t initial_ihop = InitiatingHeapOccupancyPercent;
  // G1Predictions require 5 or more samples to skip special considerations for
  // small samples.

  G1IHOPTestController ctrl(true /* adaptive */, initial_ihop, 100 /* target_occupancy */);

  add_identical_samples(&ctrl,
                        1.0 /* cycle_duration_s */,
                        10  /* young_reserve */,
                        20  /* non_hum_bytes */,
                        0   /* hum_alloc_bytes */,
                        20);

  size_t before_spike = ctrl._ihop_control.old_gen_threshold_for_conc_mark_start();

  add_identical_samples(&ctrl,
                        1.0 /* cycle_duration_s */,
                        10  /* young_reserve */,
                        200 /* non_hum_bytes */,
                        100 /* hum_alloc_bytes */,
                        5);

  size_t during_spike = ctrl._ihop_control.old_gen_threshold_for_conc_mark_start();


  add_identical_samples(&ctrl,
                        1.0 /* cycle_duration_s */,
                        10  /* young_reserve */,
                        20  /* non_hum_bytes */,
                        10  /* hum_alloc_bytes */,
                        20);

  size_t after_recovery = ctrl._ihop_control.old_gen_threshold_for_conc_mark_start();

  EXPECT_LT(during_spike, before_spike);
  EXPECT_GT(after_recovery, during_spike);
}

TEST_VM(G1IHOPControl, adaptive_ihop_peak_humongous_incr) {
  // Test requires G1
  if (!UseG1GC) {
    return;
  }

  size_t initial_ihop = InitiatingHeapOccupancyPercent;
  // G1Predictions require 5 or more samples to skip special considerations for
  // small samples.
  size_t num_samples = 5;

  size_t target_occupancy = 100;
  size_t young_reserve    = 10;
  double cycle_duration   = 1.0;

  G1IHOPTestController ctrl(true /* adaptive */, initial_ihop, target_occupancy);

  for (size_t i = 0; i < num_samples; i++) {
    ctrl.start_cycle();
    ctrl.record_young_gc(1.0 /* mutator_time_s */,
                         young_reserve,
                         20  /* non_hum_bytes */,
                         30  /* hum_alloc_bytes */,
                         25  /* hum_after_gc_bytes */);

    ctrl.record_young_gc(1.0 /* mutator_time_s */,
                         young_reserve,
                         5   /* non_hum_bytes */,
                         10  /* hum_alloc_bytes */,
                         10  /* hum_after_gc_bytes */);

    ctrl.complete_cycle(cycle_duration);
  }

  size_t expected_non_hum_bytes = 25;
  // hum_after_gc_bytes (GC 1) + hum_alloc_bytes (GC 2)
  size_t expected_peak_hum_bytes = 35;

  size_t needed_for_cycle = young_reserve +
                            expected_non_hum_bytes * cycle_duration +
                            expected_peak_hum_bytes;

  size_t expected_threshold = target_occupancy - needed_for_cycle;

  EXPECT_EQ(expected_threshold, ctrl._ihop_control.old_gen_threshold_for_conc_mark_start());
}

TEST_VM(G1IHOPControl, adaptive_ihop_reuse_eagerly_reclaimed) {
  // Test requires G1
  if (!UseG1GC) {
    return;
  }

  size_t initial_ihop = InitiatingHeapOccupancyPercent;

  size_t target_occupancy = 100;
  size_t young_reserve    = 10;

  G1IHOPTestController ctrl(true /* adaptive */, initial_ihop, target_occupancy);

  size_t h_t0 = 100;
  ctrl.start_cycle(h_t0 /* humongous_bytes_after_gc */);

  // First mutator phase:
  // No new humongous allocations in this phase.
  // h_t1 < h_t0 (eager reclaim)
  size_t h_t1 = 60;
  ctrl.record_young_gc(1.0 /* mutator_time_s */,
                       young_reserve,
                       0    /* non_hum_bytes */,
                       0    /* hum_alloc_bytes */,
                       h_t1 /* hum_after_gc_bytes */);

  EXPECT_EQ(0ul, ctrl._alloc_tracker.peak_extra_humongous_reserve_bytes());

  // Second mutator phase:
  size_t hum_alloc_bytes = 50;
  size_t h_t2 = 60;
  ctrl.record_young_gc(1.0 /* mutator_time_s */,
                       young_reserve,
                       0    /* non_hum_bytes */,
                       hum_alloc_bytes,
                       h_t2 /* hum_after_gc_bytes */);
  // Expected:
  // delta_after_previous_gc = 60 - 100 = -40
  // delta_before_this_gc    = -40 + 50 = 10
  // peak extra reserve      = 10
  EXPECT_EQ(10ul, ctrl._alloc_tracker.peak_extra_humongous_reserve_bytes());

}

TEST_VM(G1IHOPControl, adaptive_ihop_eager_reclaim_reduces_extra_humongous_reserve) {
  // Test requires G1
  if (!UseG1GC) {
    return;
  }

  size_t initial_ihop = InitiatingHeapOccupancyPercent;
  // G1Predictions require 5 or more samples to skip special considerations for
  // small samples.
  size_t num_samples = 5;

  size_t target_occupancy = 100;
  size_t young_reserve    = 10;
  double cycle_duration   = 1.0;

  G1IHOPTestController ctrl(true /* adaptive */, initial_ihop, target_occupancy);

  for (size_t i = 0; i < num_samples; i++) {
    ctrl.start_cycle(100 /* humongous_bytes_after_gc */);
    ctrl.record_young_gc(1.0 /* mutator_time_s */,
                         young_reserve,
                         20  /* non_hum_bytes */,
                         0  /* hum_alloc_bytes */,
                         60  /* hum_after_gc_bytes */);

    ctrl.record_young_gc(1.0 /* mutator_time_s */,
                         young_reserve,
                         0   /* non_hum_bytes */,
                         50  /* hum_alloc_bytes */,
                         60  /* hum_after_gc_bytes */);

    ctrl.complete_cycle(cycle_duration);
  }

  // Expected:
  // predicted_needed = young_reserve + non_hum_bytes + peak_extra_hum_reserve
  //                  = 10 + 20 + 10
  // threshold        = target - predicted_needed
  //                  = 100 - 40
  EXPECT_EQ(60ul, ctrl._ihop_control.old_gen_threshold_for_conc_mark_start());

}

TEST_VM(G1IHOPControl, adaptive_ihop_cycle_duration) {
  // Test requires G1
  if (!UseG1GC) {
    return;
  }

  size_t initial_ihop = InitiatingHeapOccupancyPercent;
  // G1Predictions require 5 or more samples to skip special considerations for
  // small samples.
  size_t num_samples = 5;

  G1IHOPTestController ctrl_a(true /* adaptive */, initial_ihop, 100 /* target_occupancy */);
  G1IHOPTestController ctrl_b(true /* adaptive */, initial_ihop, 100 /* target_occupancy */);

  add_identical_samples(&ctrl_a,
                        1.0 /* cycle_duration_s */,
                        10  /* young_reserve */,
                        40  /* non_hum_bytes */,
                        30  /* peak_hum_bytes */,
                        num_samples);

  add_identical_samples(&ctrl_b,
                        2.0 /* cycle_duration_s */,
                        10  /* young_reserve */,
                        40  /* non_hum_bytes */,
                        30  /* peak_hum_bytes */,
                        num_samples);

  EXPECT_EQ(ctrl_a._ihop_control.old_gen_threshold_for_conc_mark_start(),
            ctrl_b._ihop_control.old_gen_threshold_for_conc_mark_start());

}
