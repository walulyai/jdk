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
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "compiler/oopMap.hpp"
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1FullCollector.inline.hpp"
#include "gc/g1/g1FullGCAdjustTask.hpp"
#include "gc/g1/g1FullGCCompactTask.hpp"
#include "gc/g1/g1FullGCMarker.inline.hpp"
#include "gc/g1/g1FullGCMarkTask.hpp"
#include "gc/g1/g1FullGCPrepareTask.inline.hpp"
#include "gc/g1/g1FullGCScope.hpp"
#include "gc/g1/g1OopClosures.hpp"
#include "gc/g1/g1Policy.hpp"
#include "gc/g1/g1RegionMarkStatsCache.inline.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/preservedMarks.hpp"
#include "gc/shared/referenceProcessor.hpp"
#include "gc/shared/verifyOption.hpp"
#include "gc/shared/weakProcessor.inline.hpp"
#include "gc/shared/workerPolicy.hpp"
#include "logging/log.hpp"
#include "runtime/handles.inline.hpp"
#include "utilities/debug.hpp"

static void clear_and_activate_derived_pointers() {
#if COMPILER2_OR_JVMCI
  DerivedPointerTable::clear();
#endif
}

static void deactivate_derived_pointers() {
#if COMPILER2_OR_JVMCI
  DerivedPointerTable::set_active(false);
#endif
}

static void update_derived_pointers() {
#if COMPILER2_OR_JVMCI
  DerivedPointerTable::update_pointers();
#endif
}

G1CMBitMap* G1FullCollector::mark_bitmap() {
  return _heap->concurrent_mark()->mark_bitmap();
}

ReferenceProcessor* G1FullCollector::reference_processor() {
  return _heap->ref_processor_stw();
}

uint G1FullCollector::calc_active_workers() {
  G1CollectedHeap* heap = G1CollectedHeap::heap();
  uint max_worker_count = heap->workers()->max_workers();
  // Only calculate number of workers if UseDynamicNumberOfGCThreads
  // is enabled, otherwise use max.
  if (!UseDynamicNumberOfGCThreads) {
    return max_worker_count;
  }

  // Consider G1HeapWastePercent to decide max number of workers. Each worker
  // will in average cause half a region waste.
  uint max_wasted_regions_allowed = ((heap->num_regions() * G1HeapWastePercent) / 100);
  uint waste_worker_count = MAX2((max_wasted_regions_allowed * 2) , 1u);
  uint heap_waste_worker_limit = MIN2(waste_worker_count, max_worker_count);

  // Also consider HeapSizePerGCThread by calling WorkerPolicy to calculate
  // the number of workers.
  uint current_active_workers = heap->workers()->active_workers();
  uint active_worker_limit = WorkerPolicy::calc_active_workers(max_worker_count, current_active_workers, 0);

  // Finally consider the amount of used regions.
  uint used_worker_limit = heap->num_used_regions();
  assert(used_worker_limit > 0, "Should never have zero used regions.");

  // Update active workers to the lower of the limits.
  uint worker_count = MIN3(heap_waste_worker_limit, active_worker_limit, used_worker_limit);
  log_debug(gc, task)("Requesting %u active workers for full compaction (waste limited workers: %u, "
                      "adaptive workers: %u, used limited workers: %u)",
                      worker_count, heap_waste_worker_limit, active_worker_limit, used_worker_limit);
  worker_count = heap->workers()->set_active_workers(worker_count);
  log_info(gc, task)("Using %u workers of %u for full compaction", worker_count, max_worker_count);

  return worker_count;
}

G1FullCollector::G1FullCollector(G1CollectedHeap* heap,
                                 bool explicit_gc,
                                 bool clear_soft_refs,
                                 bool do_maximal_compaction,
                                 G1FullGCTracer* tracer) :
    _heap(heap),
    _scope(heap->monitoring_support(), explicit_gc, clear_soft_refs, do_maximal_compaction, tracer),
    _num_workers(calc_active_workers()),
    _oop_queue_set(_num_workers),
    _array_queue_set(_num_workers),
    _preserved_marks_set(true),
    _serial_compaction_point(this),
    _humongous_compaction_point(this),
    _is_alive(this, heap->concurrent_mark()->mark_bitmap()),
    _is_alive_mutator(heap->ref_processor_stw(), &_is_alive),
    _always_subject_to_discovery(),
    _is_subject_mutator(heap->ref_processor_stw(), &_always_subject_to_discovery),
    _region_attr_table() {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at a safepoint");

  _preserved_marks_set.init(_num_workers);
  _markers = NEW_C_HEAP_ARRAY(G1FullGCMarker*, _num_workers, mtGC);
  _compaction_points = NEW_C_HEAP_ARRAY(G1FullGCCompactionPoint*, _num_workers, mtGC);

  _humongous_start_regions = new (mtGC) GrowableArray<HeapRegion*>(16, mtGC);

  _live_stats = NEW_C_HEAP_ARRAY(G1RegionMarkStats, _heap->max_regions(), mtGC);
  _compaction_tops = NEW_C_HEAP_ARRAY(HeapWord*, _heap->max_regions(), mtGC);
  for (uint j = 0; j < heap->max_regions(); j++) {
    _live_stats[j].clear();
    _compaction_tops[j] = nullptr;
  }

  for (uint i = 0; i < _num_workers; i++) {
    _markers[i] = new G1FullGCMarker(this, i, _preserved_marks_set.get(i), _live_stats);
    _compaction_points[i] = new G1FullGCCompactionPoint(this);
    _oop_queue_set.register_queue(i, marker(i)->oop_stack());
    _array_queue_set.register_queue(i, marker(i)->objarray_stack());
  }
  _region_attr_table.initialize(heap->reserved(), HeapRegion::GrainBytes);
}

G1FullCollector::~G1FullCollector() {
  for (uint i = 0; i < _num_workers; i++) {
    delete _markers[i];
    delete _compaction_points[i];
  }
  FREE_C_HEAP_ARRAY(G1FullGCMarker*, _markers);
  FREE_C_HEAP_ARRAY(G1FullGCCompactionPoint*, _compaction_points);
  FREE_C_HEAP_ARRAY(HeapWord*, _compaction_tops);
  FREE_C_HEAP_ARRAY(G1RegionMarkStats, _live_stats);
}

class PrepareRegionsClosure : public HeapRegionClosure {
  G1FullCollector* _collector;

public:
  PrepareRegionsClosure(G1FullCollector* collector) : _collector(collector) { }

  bool do_heap_region(HeapRegion* hr) {
    G1CollectedHeap::heap()->prepare_region_for_full_compaction(hr);
    _collector->before_marking_update_attribute_table(hr);
    return false;
  }
};

void G1FullCollector::prepare_collection() {
  _heap->policy()->record_full_collection_start();

  // Verification needs the bitmap, so we should clear the bitmap only later.
  bool in_concurrent_cycle = _heap->abort_concurrent_cycle();
  _heap->verify_before_full_collection(scope()->is_explicit_gc());
  if (in_concurrent_cycle) {
    GCTraceTime(Debug, gc) debug("Clear Bitmap");
    _heap->concurrent_mark()->clear_bitmap(_heap->workers());
  }

  _heap->gc_prologue(true);
  _heap->retire_tlabs();
  _heap->prepare_heap_for_full_collection();

  PrepareRegionsClosure cl(this);
  _heap->heap_region_iterate(&cl);

  reference_processor()->start_discovery(scope()->should_clear_soft_refs());

  // Clear and activate derived pointer collection.
  clear_and_activate_derived_pointers();
}

void G1FullCollector::collect() {
  G1CollectedHeap::start_codecache_marking_cycle_if_inactive();

  phase1_mark_live_objects();
  verify_after_marking();

  // Don't add any more derived pointers during later phases
  deactivate_derived_pointers();

  phase2_prepare_compaction();

  phase3_adjust_pointers();

  phase4_do_compaction();

  CodeCache::on_gc_marking_cycle_finish();
  CodeCache::arm_all_nmethods();
}

void G1FullCollector::complete_collection() {
  // Restore all marks.
  restore_marks();

  // When the pointers have been adjusted and moved, we can
  // update the derived pointer table.
  update_derived_pointers();

  // Need completely cleared claim bits for the next concurrent marking or full gc.
  ClassLoaderDataGraph::clear_claimed_marks();

  // Prepare the bitmap for the next (potentially concurrent) marking.
  _heap->concurrent_mark()->clear_bitmap(_heap->workers());

  _heap->prepare_heap_for_mutators();

  _heap->resize_all_tlabs();

  _heap->policy()->record_full_collection_end();
  _heap->gc_epilogue(true);

  _heap->verify_after_full_collection();

  _heap->print_heap_after_full_collection();
}

void G1FullCollector::before_marking_update_attribute_table(HeapRegion* hr) {
  if (hr->is_free()) {
    _region_attr_table.set_free(hr->hrm_index());
  } else if (hr->is_closed_archive()) {
    _region_attr_table.set_skip_marking(hr->hrm_index());
  } else if (hr->is_pinned()) {
    _region_attr_table.set_skip_compacting(hr->hrm_index());
  } else {
    // Everything else should be compacted.
    _region_attr_table.set_compacting(hr->hrm_index());
  }
}

class G1FullGCRefProcProxyTask : public RefProcProxyTask {
  G1FullCollector& _collector;

public:
  G1FullGCRefProcProxyTask(G1FullCollector &collector, uint max_workers)
    : RefProcProxyTask("G1FullGCRefProcProxyTask", max_workers),
      _collector(collector) {}

  void work(uint worker_id) override {
    assert(worker_id < _max_workers, "sanity");
    G1IsAliveClosure is_alive(&_collector);
    uint index = (_tm == RefProcThreadModel::Single) ? 0 : worker_id;
    G1FullKeepAliveClosure keep_alive(_collector.marker(index));
    BarrierEnqueueDiscoveredFieldClosure enqueue;
    G1FollowStackClosure* complete_gc = _collector.marker(index)->stack_closure();
    _rp_task->rp_work(worker_id, &is_alive, &keep_alive, &enqueue, complete_gc);
  }
};

void G1FullCollector::phase1_mark_live_objects() {
  // Recursively traverse all live objects and mark them.
  GCTraceTime(Info, gc, phases) info("Phase 1: Mark live objects", scope()->timer());

  {
    // Do the actual marking.
    G1FullGCMarkTask marking_task(this);
    run_task(&marking_task);
  }

  {
    uint old_active_mt_degree = reference_processor()->num_queues();
    reference_processor()->set_active_mt_degree(workers());
    GCTraceTime(Debug, gc, phases) debug("Phase 1: Reference Processing", scope()->timer());
    // Process reference objects found during marking.
    ReferenceProcessorPhaseTimes pt(scope()->timer(), reference_processor()->max_num_queues());
    G1FullGCRefProcProxyTask task(*this, reference_processor()->max_num_queues());
    const ReferenceProcessorStats& stats = reference_processor()->process_discovered_references(task, pt);
    scope()->tracer()->report_gc_reference_stats(stats);
    pt.print_all_references();
    assert(marker(0)->oop_stack()->is_empty(), "Should be no oops on the stack");

    reference_processor()->set_active_mt_degree(old_active_mt_degree);
  }

  // Weak oops cleanup.
  {
    GCTraceTime(Debug, gc, phases) debug("Phase 1: Weak Processing", scope()->timer());
    WeakProcessor::weak_oops_do(_heap->workers(), &_is_alive, &do_nothing_cl, 1);
  }

  // Class unloading and cleanup.
  if (ClassUnloading) {
    GCTraceTime(Debug, gc, phases) debug("Phase 1: Class Unloading and Cleanup", scope()->timer());
    CodeCache::UnloadingScope unloading_scope(&_is_alive);
    // Unload classes and purge the SystemDictionary.
    bool purged_class = SystemDictionary::do_unloading(scope()->timer());
    _heap->complete_cleaning(purged_class);
  }

  scope()->tracer()->report_object_count_after_gc(&_is_alive);
#if TASKQUEUE_STATS
  oop_queue_set()->print_and_reset_taskqueue_stats("Oop Queue");
  array_queue_set()->print_and_reset_taskqueue_stats("ObjArrayOop Queue");
#endif
}

void G1FullCollector::phase2_prepare_compaction() {
  GCTraceTime(Info, gc, phases) info("Phase 2: Prepare compaction", scope()->timer());

  phase2a_determine_worklists();

  bool has_free_compaction_targets = phase2b_forward_oops();

  // Try to avoid OOM immediately after Full GC in case there are no free regions
  // left after determining the result locations (i.e. this phase). Prepare to
  // maximally compact the tail regions of the compaction queues serially.
  if (scope()->do_maximal_compaction() || !has_free_compaction_targets) {
    phase2c_prepare_serial_compaction();
  }
}

void G1FullCollector::phase2a_determine_worklists() {
  GCTraceTime(Debug, gc, phases) debug("Phase 2: Determine work lists", scope()->timer());

  G1DetermineCompactionQueueClosure cl(this);
  _heap->heap_region_iterate(&cl);
}

bool G1FullCollector::phase2b_forward_oops() {
  GCTraceTime(Debug, gc, phases) debug("Phase 2: Prepare parallel compaction", scope()->timer());

  G1FullGCPrepareTask task(this);
  run_task(&task);

  return task.has_free_compaction_targets();
}

void G1FullCollector::phase2c_prepare_serial_compaction() {
  GCTraceTime(Debug, gc, phases) debug("Phase 2: Prepare serial compaction", scope()->timer());
  // FIXME: At this point we know that after parallel compaction there will be no
  // completely free regions. That means that the last region of
  // all compaction queues still have data in them. We try to compact
  // these regions in serial to avoid a premature OOM when the mutator wants
  // to allocate the first eden region after gc.
  G1FullGCCompactionPoint* serial_cp = serial_compaction_point();
  for (uint i = 0; i < workers(); i++) {
    G1FullGCCompactionPoint* cp = compaction_point(i);

    // FIXME: move leftover regions to the serial compaction point
    if (cp->has_regions()) {

      log_debug(gc, region)("Added to serial %u", cp->current_region()->hrm_index());
      cp->truncate_from_current(serial_cp);
    }
  }

  serial_cp->regions()->sort([](HeapRegion** a, HeapRegion** b) { return static_cast<int>((*a)->hrm_index() - (*b)->hrm_index()); });

  // Update the forwarding information for the regions in the serial
  // compaction point.
  HeapRegion* start_serial = nullptr;
  for (GrowableArrayIterator<HeapRegion*> it = serial_cp->regions()->begin(); it != serial_cp->regions()->end(); ++it) {
    HeapRegion* current = *it;
    // log_debug(gc, region)("Part of serial %u", current->hrm_index());
    if (!serial_cp->is_initialized()) {
      // Initialize the compaction point. Nothing more is needed for the first heap region
      // since it is already prepared for compaction.
      serial_cp->initialize(current);
      start_serial = current;
    } else if (!is_free(current->hrm_index())) {

      log_debug(gc, region)("Re-Compact to serial %u >> %u", current->hrm_index(), start_serial->hrm_index()); 
      assert(!current->is_humongous(), "Should be no humongous regions in compaction queue");
      G1SerialRePrepareClosure re_prepare(serial_cp, current, start_serial);
      set_compaction_top(current, current->bottom());
      current->apply_to_marked_objects(mark_bitmap(), &re_prepare);
    }
  }
  serial_cp->update();

  if (humongous_start_regions()->is_empty()) {
    log_debug(gc, region)("Don't bother, we have no humongous"); // FIXME remove
    return;
  }

  // FIXME: change name, this shouldn't truncate the serial points.
  // Just make the regions available for humoungous compaction, but the serial compaction 
  // Still has to remove objects that are exisiting in those regions, thus should not compact.
  serial_cp->copy_after_current(humongous_compaction_point());

  // FIXME: At this point, we know that all the regular regions have been compacted.
  // We also know the last region into which regular objects will be compacted. Beyond that, we can move humongous 
  // objects that can benefit from a move. But we also need to take not of Pinned Regions that are not humoungous.
  // This can be achieved since we have added all the available regions to the serial compaction point and also sorted it.
  // So for each serial compaction. 
  // So we can repeat the same procedure:
  //    1. Can humongous object be moved based on current regions in compaction point
  //    2. if yes, then add humoungous object regions to compaction point, move the humoungous object
  //    3. Update the compaction point accordingly.

  // _collector->humongous_start_regions();



  G1FullGCCompactionPoint* humongous_cp = humongous_compaction_point();

  if (humongous_cp->regions()->is_empty()) {
    log_debug(gc, region)("Don't bother, we have no humongous"); // FIXME remove
    return;
  }

  humongous_cp->initialize(humongous_cp->regions()->first());

  HeapRegion* target_region = humongous_compaction_point()->current_region();

  uint target_index = target_region->hrm_index();

  G1CollectedHeap* g1h = G1CollectedHeap::heap();

  for (GrowableArrayIterator<HeapRegion*> it = humongous_start_regions()->begin();
         it != humongous_start_regions()->end();
         ++it) {
    //Add humongous regions above current target to compaction point
    HeapRegion* hr = *it;
    if (hr->hrm_index() < target_index) {
      continue;
    }
    // else add all regions to compaction point
    oop object = cast_to_oop(hr->bottom());
    size_t obj_size = object->size(); // FIXME: remove
    uint num_regions = (uint) G1CollectedHeap::humongous_obj_size_in_regions(obj_size); // FIXME: remove
    uint start_index = hr->hrm_index();
    humongous_compaction_point()->add(hr);
    for(uint i = 1; i < num_regions; i++) {
        HeapRegion* r = g1h->region_at(start_index + i);
        assert(hr == r->humongous_start_region(), "Must be");
        humongous_compaction_point()->add(r);
    }
  }

  humongous_compaction_point()->regions()->sort([](HeapRegion** a, HeapRegion** b) { return static_cast<int>((*a)->hrm_index() - (*b)->hrm_index()); });

  assert(target_region == humongous_compaction_point()->regions()->first(), "Must still be the first region");

  // We didn't preserve marks during marking as initially humongous objects were not 
  // compacting.
  G1FullGCMarker* marker = this->marker(0);

  GrowableArray<HeapRegion *> *target_regions = humongous_compaction_point()->regions();
  for (GrowableArrayIterator<HeapRegion*> it = humongous_start_regions()->begin();
         it != humongous_start_regions()->end();
         ++it) {

    HeapRegion* hr = *it;
    if (hr->hrm_index() < target_index) {
      continue;
    }

    uint range_begin = 0;
    uint range_end   = 0;
    uint range_limit = target_regions->find(hr);

    oop obj = cast_to_oop(hr->bottom());
    size_t obj_size = obj->size(); // FIXME: remove
    uint num_regions = (uint) G1CollectedHeap::humongous_obj_size_in_regions(obj_size); // FIXME: remove

    HeapRegion* prev = nullptr;

    // Find the end of a run of contiguous free regions
    while (range_end < range_limit) {
      HeapRegion* r = target_regions->at(range_end);
      if (prev == nullptr || prev->hrm_index() == (r->hrm_index() - 1) ) {
        // regions are contiguous
        prev = r;
        range_end++;
        if (range_end - range_begin == num_regions) {
          break;
        }
        continue;
      }
      // need to restart search
      range_begin = ++range_end;
      prev = nullptr;
    }

    if (range_begin != range_end) {
      // Regions was initially not compacting, so we didn't preserve the mark.
      marker->preserved_stack()->push_if_necessary(obj, obj->mark());
      // Object can be relocated
      // we need to remove regions into which it will be relocated from the the compaction point.
      HeapRegion* start = target_regions->at(range_begin);
      HeapRegion* end   = target_regions->at((range_begin + num_regions-1));
      obj->forward_to(cast_to_oop(start->bottom()));
      assert(obj->is_forwarded(), "Must be!");
      _region_attr_table.set_compacting(hr->hrm_index());
      log_debug(gc, region) ("Forward Region: from %u to %u - %u num_regions %u ",
                             hr->hrm_index(), start->hrm_index(), end->hrm_index(), num_regions);

      // Change object/region attribute types to compacting

      // Remove covered regions from contention
      // FIXME: Remember range_end doesn't imply that object end, object can be relocated
      // and overlap with with previous.
      for (uint i = range_begin; i < (range_begin + num_regions); i++) {
        target_regions->delete_at(i);
      }
      // delete_at changes the order, so we need to re-sort
      target_regions->sort([](HeapRegion** a, HeapRegion** b) { return static_cast<int>((*a)->hrm_index() - (*b)->hrm_index()); });
    } else {
      // We can't move the object, so we need to remove it from the compaction point also.
      log_debug(gc, region) ("Region Not Moving: %u num_regions %u ",
                             hr->hrm_index(), num_regions);
      for (uint i = range_limit; i < (range_limit + num_regions); i++) {
        target_regions->delete_at(i);
      }
      // delete_at changes the order, so we need to resort
      target_regions->sort([](HeapRegion** a, HeapRegion** b) { return static_cast<int>((*a)->hrm_index() - (*b)->hrm_index()); });
    }
  }
}

void G1FullCollector::phase3_adjust_pointers() {
  // Adjust the pointers to reflect the new locations
  GCTraceTime(Info, gc, phases) info("Phase 3: Adjust pointers", scope()->timer());

  G1FullGCAdjustTask task(this);
  run_task(&task);
}

void G1FullCollector::phase4_do_compaction() {
  // Compact the heap using the compaction queues created in phase 2.
  GCTraceTime(Info, gc, phases) info("Phase 4: Compact heap", scope()->timer());
  G1FullGCCompactTask task(this);
  run_task(&task);

  // Serial compact to avoid OOM when very few free regions.
  if (serial_compaction_point()->has_regions()) {
    task.serial_compaction();
  }

  if (scope()->do_maximal_compaction() && !humongous_start_regions()->is_empty()) {
    assert(scope()->do_maximal_compaction(), "Must be!");
    log_error(gc)("Humongous Compaction Happenning");
    task.humongous_compaction();
  }


}

void G1FullCollector::restore_marks() {
  _preserved_marks_set.restore(_heap->workers());
  _preserved_marks_set.reclaim();
}

void G1FullCollector::run_task(WorkerTask* task) {
  _heap->workers()->run_task(task, _num_workers);
}

void G1FullCollector::verify_after_marking() {
  if (!VerifyDuringGC || !_heap->verifier()->should_verify(G1HeapVerifier::G1VerifyFull)) {
    // Only do verification if VerifyDuringGC and G1VerifyFull is set.
    return;
  }

#if COMPILER2_OR_JVMCI
  DerivedPointerTableDeactivate dpt_deact;
#endif
  _heap->prepare_for_verify();
  // Note: we can verify only the heap here. When an object is
  // marked, the previous value of the mark word (including
  // identity hash values, ages, etc) is preserved, and the mark
  // word is set to markWord::marked_value - effectively removing
  // any hash values from the mark word. These hash values are
  // used when verifying the dictionaries and so removing them
  // from the mark word can make verification of the dictionaries
  // fail. At the end of the GC, the original mark word values
  // (including hash values) are restored to the appropriate
  // objects.
  GCTraceTime(Info, gc, verify) tm("Verifying During GC (full)");
  _heap->verify(VerifyOption::G1UseFullMarking);
}
