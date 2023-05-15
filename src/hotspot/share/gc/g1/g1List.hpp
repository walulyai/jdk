/*
 * Copyright (c) 2015, 2020, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1LIST_HPP
#define SHARE_GC_G1_G1LIST_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"

template <typename T> class G1List;

// Element in a doubly linked list
template <typename T>
class G1ListNode {
  friend class G1List<T>;

private:
  G1ListNode<T>* _next;
  G1ListNode<T>* _prev;

  NONCOPYABLE(G1ListNode);

  void verify_links() const;
  void verify_links_linked() const;
  void verify_links_unlinked() const;

public:
  G1ListNode();
  ~G1ListNode();
};

// Doubly linked list
template <typename T>
class G1List {
private:
  G1ListNode<T> _head;
  size_t       _size;

  NONCOPYABLE(G1List);

  void verify_head() const;

  void insert(G1ListNode<T>* before, G1ListNode<T>* node);

  G1ListNode<T>* cast_to_inner(T* elem) const;
  T* cast_to_outer(G1ListNode<T>* node) const;

public:
  G1List();

  size_t size() const;
  bool is_empty() const;

  T* first() const;
  T* last() const;
  T* next(T* elem) const;
  T* prev(T* elem) const;

  void insert_first(T* elem);
  void insert_last(T* elem);
  void insert_before(T* before, T* elem);
  void insert_after(T* after, T* elem);

  void remove(T* elem);
  T* remove_first();
  T* remove_last();
};

template <typename T, bool Forward>
class G1ListIteratorImpl : public StackObj {
private:
  const G1List<T>* const _list;
  T*                    _next;

public:
  G1ListIteratorImpl(const G1List<T>* list);

  bool next(T** elem);
};

template <typename T, bool Forward>
class G1ListRemoveIteratorImpl : public StackObj {
private:
  G1List<T>* const _list;

public:
  G1ListRemoveIteratorImpl(G1List<T>* list);

  bool next(T** elem);
};

template <typename T> using G1ListIterator = G1ListIteratorImpl<T, true /* Forward */>;
template <typename T> using G1ListReverseIterator = G1ListIteratorImpl<T, false /* Forward */>;
template <typename T> using G1ListRemoveIterator = G1ListRemoveIteratorImpl<T, true /* Forward */>;

template <typename T>
class G1Locker : public StackObj {
private:
  T* const _lock;

public:
  G1Locker(T* lock);
  ~G1Locker();
};
#endif // SHARE_GC_G1_G1LIST_HPP
