/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHARED_BUFFERNODEALLOCATOR_HPP
#define SHARE_GC_SHARED_BUFFERNODEALLOCATOR_HPP

#include "memory/padded.hpp"
#include "utilities/globalDefinitions.hpp"

template <class BufferNode, bool E>
class BufferNodeAllocatorBase {
protected:
  // Since we don't expect many instances, and measured >15% speedup
  // on stress gtest, padding seems like a good tradeoff here.
#define DECLARE_PADDED_MEMBER(Id, Type, Name) \
  Type Name; DEFINE_PAD_MINUS_SIZE(Id, DEFAULT_CACHE_LINE_SIZE, sizeof(Type))

  const size_t _buffer_size = 0;
  char _name[DEFAULT_CACHE_LINE_SIZE - sizeof(size_t)]; // Use name as padding.
  DECLARE_PADDED_MEMBER(1, typename BufferNode::NodeStack, _pending_list);
  DECLARE_PADDED_MEMBER(2, typename BufferNode::NodeStack, _free_list);
  DECLARE_PADDED_MEMBER(3, volatile size_t, _pending_count);
  DECLARE_PADDED_MEMBER(4, volatile size_t, _free_count);
  DECLARE_PADDED_MEMBER(5, volatile bool, _transfer_lock);
#undef DECLARE_PADDED_MEMBER

public:
  BufferNodeAllocatorBase(const char* name, size_t buffer_size);
  const char* name() const { return _name; }
};

template <class BufferNode>
class BufferNodeAllocatorBase<BufferNode, false>  {
protected:
  const size_t _buffer_size = 0;
  char _name[DEFAULT_CACHE_LINE_SIZE - sizeof(size_t)]; // Use name as padding.
  typename BufferNode::NodeStack _pending_list;
  typename BufferNode::NodeStack _free_list;
  volatile size_t _pending_count;
  volatile size_t _free_count;
  volatile bool _transfer_lock;
  BufferNodeAllocatorBase(size_t buffer_size);
  const char* name() const { return _name; }
};

template <class BufferNode, class Arena, bool padded = false>
class BufferNodeAllocator : private BufferNodeAllocatorBase<BufferNode, padded>  {
  friend class TestSupport;
  using BufferNodeAllocatorBase<BufferNode, padded>::_buffer_size;
  using BufferNodeAllocatorBase<BufferNode, padded>::_pending_list;
  using BufferNodeAllocatorBase<BufferNode, padded>::_free_list;
  using BufferNodeAllocatorBase<BufferNode, padded>::_pending_count;
  using BufferNodeAllocatorBase<BufferNode, padded>::_free_count;
  using BufferNodeAllocatorBase<BufferNode, padded>::_transfer_lock;

  void delete_list(BufferNode* list);
  bool try_transfer_pending();

  Arena _arena;
public:
  template<bool E = padded>
  BufferNodeAllocator(const char* name, size_t buffer_size, const Arena& arena, typename std::enable_if<E, int>::type = 0) :
    BufferNodeAllocatorBase<BufferNode, padded>(name, buffer_size),
    _arena(arena)
  { }

  template<bool E = padded>
  BufferNodeAllocator(size_t buffer_size, const Arena& arena, typename std::enable_if<!E, int>::type = 0):
  BufferNodeAllocatorBase<BufferNode, padded>(buffer_size),
  _arena(arena)
  { }

  ~BufferNodeAllocator();


  size_t buffer_size() const { return _buffer_size; }
  size_t free_count() const;
  size_t pending_count() const;

  BufferNode* allocate();
  void release(BufferNode* node);
  void reset();

  const Arena* arena() const { return &_arena; }

  size_t mem_size() const {
    return sizeof(*this) + _arena.mem_size();
  }

  size_t wasted_mem_size() {
    return _arena.wasted_mem_size(pending_count());
  }

  void print(outputStream* os) {
    _arena.print(os, (uint)pending_count());
  }

  // Deallocate some of the available buffers.  remove_goal is the target
  // number to remove.  Returns the number actually deallocated, which may
  // be less than the goal if there were fewer available.
  size_t reduce_free_list(size_t remove_goal);
};

#endif // SHARE_GC_SHARED_BUFFERNODEALLOCATOR_HPP
