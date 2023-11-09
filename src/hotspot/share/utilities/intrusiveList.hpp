/*
 * Copyright (c) 2015, 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_UTILITIES_INTRUSIVELIST_HPP
#define SHARE_UTILITIES_INTRUSIVELIST_HPP

#include "metaprogramming/enableIf.hpp"
#include "metaprogramming/logical.hpp"
#include "utilities/debug.hpp"
#include "utilities/macros.hpp"
#include <type_traits>


class IntrusiveListEntry;
class IntrusiveListImpl;

template<typename T>
using IntrusiveListEntryAccessor =
  const IntrusiveListEntry* (*)(std::add_const_t<T>*);

template<typename T, IntrusiveListEntryAccessor<T> entry_accessor>
class IntrusiveList;

/**
 * A class with an IntrusiveListEntry member can be used as an element
 * of a corresponding specialization of IntrusiveList.
 */
class IntrusiveListEntry {
  friend class IntrusiveListImpl;

public:
  /** Make an entry not attached to any list. */
  IntrusiveListEntry()
    : _prev(nullptr),
      _next(nullptr)
      DEBUG_ONLY(COMMA _list(nullptr))
   {}

  /**
   * Destroy the entry.
   *
   * precondition: not an element of a list.
   */
  ~IntrusiveListEntry() NOT_DEBUG(= default);

  NONCOPYABLE(IntrusiveListEntry);

  /** Test whether this entry is attached to some list. */
  bool is_linked() const {
    bool result = (_prev != nullptr);
    assert(result == (_next != nullptr), "inconsistent entry");
    return result;
  }

private:
  // _prev and _next are the links between elements / root entries in
  // an associated list.  The values of these members are type-erased
  // void*.  The IntrusiveListImpl::IteratorOperations class is used
  // to encode, decode, and manipulate the type-erased values.
  //
  // Members are mutable and we deal exclusively with pointers to
  // const to make const_references and const_iterators easier to use;
  // an object being const doesn't prevent modifying its list state.
  mutable const void* _prev;
  mutable const void* _next;
  // The list containing this entry, if any.
  // Debug-only, for use in validity checks.
  DEBUG_ONLY(mutable IntrusiveListImpl* _list;)
};

class IntrusiveListImpl {
public:
  struct TestSupport;            // For unit tests

private:
  using Entry = IntrusiveListEntry;
  using ListEntryPtr = const void*;

  // Nothing for clients to see here, everything is private.  Only
  // the IntrusiveList class template has access, via friendship.
  template<typename T, IntrusiveListEntryAccessor<T>>
  friend class IntrusiveList;

  using size_type = size_t;
  using difference_type = ptrdiff_t;

  Entry _root;

  IntrusiveListImpl()
    : _root()
  {
    _root._prev = add_tag_to_root_entry(&_root);
    _root._next = add_tag_to_root_entry(&_root);
  }

  ~IntrusiveListImpl() NOT_DEBUG(= default);

  NONCOPYABLE(IntrusiveListImpl);

  // Tag manipulation for encoded void*; see IteratorOperations.
  static const uintptr_t _tag_alignment = 2;

  static bool is_tagged_root_entry(ListEntryPtr ptr) {
    return !is_aligned(ptr, _tag_alignment);
  }

  static ListEntryPtr add_tag_to_root_entry(const Entry* entry) {
    assert(is_aligned(entry, _tag_alignment), "must be");
    ListEntryPtr untagged = entry;
    return static_cast<const char*>(untagged) + 1;
  }

  static const Entry* remove_tag_from_root_entry(ListEntryPtr ptr) {
    assert(is_tagged_root_entry(ptr), "precondition");
    ListEntryPtr untagged = static_cast<const char*>(ptr) - 1;
    assert(is_aligned(untagged, _tag_alignment), "must be");
    return static_cast<const Entry*>(untagged);
  }

  // Iterator support.  IntrusiveList defines its iterator types as
  // specializations of this class.
  template<typename T,
           IntrusiveListEntryAccessor<T> entry_accessor,
           bool is_forward>
  class IteratorImpl;

  // Provides (static) functions for manipulating
  // List Entries.  These are used to implement list that
  // are not part of the public API for iterators.
  template<typename T,
           IntrusiveListEntryAccessor<T> entry_accessor>
  struct ListOperations;

  // Predicate metafunction for determining whether T is a non-const
  // IntrusiveList type.
  template<typename T>
  struct IsListType : public std::false_type {};

#ifdef ASSERT
  // Get entry's containing list; null if entry not in a list.
  static const IntrusiveListImpl* entry_list(const Entry& entry);
  // Set entry's containing list; list may be null.
  static void set_entry_list(const Entry& entry, IntrusiveListImpl* list);
#endif // ASSERT
};

template<typename T, IntrusiveListEntryAccessor<T> accessor>
struct IntrusiveListImpl::IsListType<IntrusiveList<T, accessor>>
  : public std::true_type
{};

template<typename T, IntrusiveListEntryAccessor<T> get_entry>
struct IntrusiveListImpl::ListOperations : AllStatic {
  using value_type = T;
  using ListEntryPtr = IntrusiveListImpl::ListEntryPtr;
  using const_pointer = std::add_pointer_t<std::add_const_t<value_type>>;
  using const_reference = std::add_lvalue_reference_t<std::add_const_t<value_type>>;

  static ListEntryPtr make_encoded_value(const_reference value) {
    return &value;
  }

  static ListEntryPtr make_encoded_value(const Entry* entry) {
    return add_tag_to_root_entry(entry);
  }

  static const Entry* list_entry(ListEntryPtr ptr) {

    if (is_tagged_root_entry(ptr)) {
      return (remove_tag_from_root_entry(ptr));
    } else {
      return get_entry(list_element(ptr));
    }
  }

  // Get the list element from an encoded pointer to list element.
  static const_pointer list_element(ListEntryPtr ptr) {
    assert(!is_tagged_root_entry(ptr),  "invalid cast");
    return static_cast<const_pointer>(ptr);
  }

  static ListEntryPtr next(ListEntryPtr entry) {
    return list_entry(entry)->_next;
  }

  static ListEntryPtr prev(ListEntryPtr entry) {
    return list_entry(entry)->_prev;
  }

  static void set_next(ListEntryPtr cur, ListEntryPtr next) {
    list_entry(cur)->_next = next;
  }

  static void set_prev(ListEntryPtr cur, ListEntryPtr prev) {
    list_entry(cur)->_prev = prev;
  }

  static void link_before(ListEntryPtr cur, ListEntryPtr entry) {
    ListEntryPtr prev_entry = prev(cur);

    set_prev(entry, prev_entry);
    set_next(entry, cur);

    set_prev(cur, entry);
    set_next(prev_entry, entry);
    assert(list_entry(cur)->is_linked(), "post-condition");
  }

  static void link_after(ListEntryPtr cur, ListEntryPtr entry) {
    ListEntryPtr next_entry = next(cur);

    set_prev(entry, cur);
    set_next(entry, next_entry);

    set_next(cur, entry);
    set_prev(next_entry, entry);
  }

  static void link_range_before(ListEntryPtr cur, ListEntryPtr entry) {
    ListEntryPtr prev_entry = prev(cur);
    set_next(entry, cur);
    set_prev(cur, entry);
  }

  static void link_range_after(ListEntryPtr cur, ListEntryPtr entry) {
    ListEntryPtr next_entry = next(cur);
    set_prev(entry, cur);
    set_next(cur, entry);
  }

  static ListEntryPtr unlink(ListEntryPtr cur) {
    assert(!is_tagged_root_entry(cur), "should not unlink root from the list");

    ListEntryPtr next_entry = next(cur);
    ListEntryPtr prev_entry = prev(cur);

    set_next(prev_entry, next_entry);
    set_prev(next_entry, prev_entry);

#ifdef ASSERT
    set_next(cur, nullptr);
    set_prev(cur, nullptr);
#endif
    return next_entry;
  }

  static void unlink(ListEntryPtr from, ListEntryPtr to) {
    if (from == to) {
      return;
    }

    ListEntryPtr prev_entry = prev(from);
    set_prev(to, prev_entry);
    set_next(prev_entry, to);
  }

};

template<typename T, IntrusiveListEntryAccessor<T> get_entry>
class IntrusiveList : public IntrusiveListImpl {
  // Give access to other instantiations, for splice().
  template<typename U, IntrusiveListEntryAccessor<U>>
  friend class IntrusiveList;

  // Give access for unit testing.
  friend struct IntrusiveListImpl::TestSupport;

  using ListEntryPtr = IntrusiveListImpl::ListEntryPtr;
  using Impl = IntrusiveListImpl;
  using Ops  = ListOperations<T, get_entry>;


  // Relevant type aliases.  A corresponding specialization is used directly
  // by IntrusiveList, and by the list's iterators to obtain their
  // corresponding nested types.
  static_assert(std::is_class<T>::value, "precondition");
  // May be const, but not volatile.
  static_assert(!std::is_volatile<T>::value, "precondition");

public:
  /** Type of the size of the list. */
  using size_type = typename Impl::size_type;

  /** The difference type for iterators. */
  using difference_type = typename Impl::difference_type;

  /** Type of list elements. */
  using value_type = T;

  /** Type of a pointer to a list element. */
  using pointer = std::add_pointer_t<value_type>;

  /** Type of a pointer to a const list element. */
  using const_pointer = std::add_pointer_t<std::add_const_t<value_type>>;

  /** Type of a reference to a list element. */
  using reference = std::add_lvalue_reference_t<value_type>;

  /** Type of a reference to a const list element. */
  using const_reference = std::add_lvalue_reference_t<std::add_const_t<value_type>>;

private:
  // Since c++11 size() must be constant time; O(1)
  size_type _size;
  void adjust_size(difference_type n) { _size += n; }

  ListEntryPtr root_entry() const {
    return Ops::make_encoded_value(&_root);
  }

public:
  /** Make an empty list. */
  IntrusiveList() : _size(0) {}

  /**
   * Destroy the list.
   *
   * precondition: empty()
   */
  ~IntrusiveList() = default;

  NONCOPYABLE(IntrusiveList);

  /**
   * Inserts value at the front of the list.  Does not affect the
   * validity of iterators or element references for this list.
   *
   * precondition: value must not already be in a list using the same entry.
   * complexity: constant.
   */
  void push_front(pointer value) {
    Ops::link_after(root_entry(), value);
    adjust_size(1);
  }

  /**
   * Inserts value at the back of the list.  Does not affect the
   * validity of iterators or element references for this list.
   *
   * precondition: value must not already be in a list using the same entry.
   * complexity: constant.
   */
  void push_back(pointer value) {
    Ops::link_before(root_entry(), value);
    adjust_size(1);
  }

  /**
   * Removes the front element from the list, returns the removed element.
   * Invalidates iterators for the removed element.
   *
   * complexity: constant.
   */
  pointer pop_front() {
    pointer value = front();
    if (value != nullptr) {
      Ops::unlink(value);
      adjust_size(-1);
    }
    return value;
  }

  /**
   * Removes the back element from the list, returns the removed element.
   * Invalidates iterators for the removed element.
   *
   * complexity: constant.
   */
  pointer pop_back() {
    pointer value = back();
    if (value != nullptr) {
      Ops::unlink(value);
      adjust_size(-1);
    }
    return value;
  }

  /**
   * Returns a [const_]reference to the front element of the list.
   *
   * complexity: constant.
   */
  pointer front() {
    ListEntryPtr first = Ops::next(root_entry());
    return empty() ? nullptr : const_cast<pointer>(Ops::list_element(first));
  }

  const_pointer front() const {
    ListEntryPtr first = Ops::next(root_entry());
    return empty() ? nullptr : Ops::list_element(first);
  }

  /**
   * Returns a [const_]reference to the back element of the list.
   *
   * precondition: !empty()
   * complexity: constant.
   */
  pointer back() {
    ListEntryPtr last = Ops::prev(root_entry());
    return empty() ? nullptr : const_cast<pointer>(Ops::list_element(last));
  }

  const_pointer back() const {
    ListEntryPtr last = Ops::prev(root_entry());
    return empty() ? nullptr : Ops::list_element(last);
  }

  /**
   * Returns the number of elements in the list.
   *
   * complexity: constant.
   */
  size_t size() const {
    return _size;
  }

  /**
   * Returns true if list contains no elements.
   *
   * complexity: constant.
   */
  bool empty() const {
    assert(Ops::next(root_entry()) == Ops::prev(root_entry()) || _size > 0, "invariant");
    return _size == 0;
  }

public:

  /** Forward iterator type. */
  using iterator = IteratorImpl<T, get_entry, true>;

  /** Forward iterator type with const elements. */
  using const_iterator = IteratorImpl<std::add_const_t<T>, get_entry, true>;

  /** Reverse iterator type. */
  using reverse_iterator = IteratorImpl<T, get_entry, false>;

  /** Reverse iterator type with const elements. */
  using const_reverse_iterator = IteratorImpl<std::add_const_t<T>, get_entry, false>;

  inline iterator begin();
  inline const_iterator begin() const;
  inline const_iterator cbegin() const;

  inline iterator end();
  inline const_iterator end() const;
  inline const_iterator cend() const;

  inline reverse_iterator rbegin();
  inline const_reverse_iterator rbegin() const;
  inline const_reverse_iterator crbegin() const;

  inline reverse_iterator rend();
  inline const_reverse_iterator rend() const;
  inline const_reverse_iterator crend() const;

  iterator iterator_to(reference value) {
    return iterator(Ops::make_encoded_value(value));
  }

  const_iterator iterator_to(const_reference value) const {
    return const_iterator(Ops::make_encoded_value(value));
  }

  const_iterator const_iterator_to(const_reference value) const {
    return const_iterator(Ops::make_encoded_value(value));
  }

  reverse_iterator reverse_iterator_to(reference value) {
    return reverse_iterator(Ops::make_encoded_value(value));
  }

  const_reverse_iterator reverse_iterator_to(const_reference value) const {
    return const_reverse_iterator(Ops::make_encoded_value(value));
  }

  const_reverse_iterator const_reverse_iterator_to(const_reference value) const {
    return const_reverse_iterator(Ops::make_encoded_value(value));
  }

  iterator insert(const_iterator pos, reference value) {
    Ops::link_before(pos.cur_entry(), &value);
    adjust_size(1);
    return iterator_to(value);
  }

  template<class Iterator>
  void insert(const_iterator p, Iterator b, Iterator e) {
    for (; b != e; ++b) {
      insert(p, *b);
    }
  }

#ifdef ASSERT
/*
  void set_list(const_reference value, Impl* list) {
    set_entry_list(get_entry(value), list);
  }*/
#endif
  struct NopDisposer {
    void operator()(pointer) const {}
  };

  // The pointer type is const-qualified per to the elements of the list, so
  // it's okay to possibly cast away const when disposing.
  static pointer disposer_arg(const_pointer value) {
    return const_cast<pointer>(value);
  }

  iterator erase(const_reference v) {
    return erase(iterator_to(v));
  }

  iterator erase(const_iterator i) {
    return erase_and_dispose(i, NopDisposer());
  }

  reverse_iterator erase(const_reverse_iterator i) {
    return erase_and_dispose(i, NopDisposer());
  }

  iterator erase(const_iterator start, const_iterator end ) {
    return erase_and_dispose(start, end, NopDisposer());
  }

  reverse_iterator erase(const_reverse_iterator from, const_reverse_iterator to) {
    return erase_and_dispose(from, to, NopDisposer());
  }

  template<typename Disposer>
  reverse_iterator erase_and_dispose(const_reverse_iterator i, Disposer disposer) {
    return erase_one_and_dispose<reverse_iterator>(i, disposer);
  }

  template<typename Disposer>
  iterator erase_and_dispose(const_iterator i, Disposer disposer) {
    return erase_one_and_dispose<iterator>(i, disposer);
  }

  template<typename Result, typename Iterator, typename Disposer>
  Result erase_one_and_dispose(Iterator i, Disposer disposer) {
    const_reference to_erase = *i;
    ++i;
    Ops::unlink(&to_erase);
    disposer(disposer_arg(&to_erase));
    adjust_size(-1);
    return Result(i.cur_entry());
  }

  template<typename Disposer>
  iterator erase_and_dispose(const_iterator from, const_iterator to, Disposer disposer) {
    return erase_range_and_dispose<iterator>(from, to, disposer);
  }

  template<typename Disposer>
  reverse_iterator erase_and_dispose(const_reverse_iterator from,
                                     const_reverse_iterator to,
                                     Disposer disposer) {
    return erase_range_and_dispose<reverse_iterator>(from, to, disposer);
  }

  template<typename Result, typename Iterator, typename Disposer>
  Result erase_range_and_dispose(Iterator start, Iterator end, Disposer disposer) {

    while (start != end) {
      erase_and_dispose(start++, disposer);
    }

    return Result(end.cur_entry());
  }

  void clear() {
    erase(begin(), end());
  }

  template<typename Disposer>
  void clear_and_dispose(Disposer disposer) {
    erase_and_dispose(begin(), end(), disposer);
  }

  void remove(const_reference v) {
    return erase(iterator_to(v));
  }

  template<typename Predicate>
  size_type remove_if(Predicate predicate) {
    return erase_if(predicate);
  }

  template<typename Predicate>
  size_type erase_if(Predicate predicate) {
    return erase_and_dispose_if(predicate, NopDisposer());
  }

  template<typename Predicate, typename Disposer>
  size_type erase_and_dispose_if(Predicate predicate, Disposer disposer) {
    const_iterator pos = cbegin();
    const_iterator end = cend();

    size_t removed = 0;
    while (pos != end) {
      const_reference v = *pos;
      if (predicate(v)) {
        pos = erase(pos);
        disposer(disposer_arg(&v));
        ++removed;
      } else {
        ++pos;
      }
    }
    return removed;
  }

  template<typename Iterator1, typename Iterator2>
  difference_type distance(Iterator1 from, Iterator2 to) {
    difference_type result = 0;
    for ( ; from != to; ++result, ++from) {}
    return result;
  }

private:
  template<typename FromList>
  void transfer(const_iterator pos, FromList& from_list, typename FromList::iterator start, typename FromList::const_iterator last) {

    iterator prev_itr(pos.cur_entry());
    --prev_itr;

    typename FromList::const_iterator other_last(last.cur_entry());
    --other_last;

    const_reference from_value = *start;
    const_reference last_value = *other_last;
    Ops::unlink(start.cur_entry(), last.cur_entry());
    Ops::link_range_before(pos.cur_entry(), &last_value);
    Ops::link_range_after(prev_itr.cur_entry(), &from_value);
  }

  // Helper for can_splice_from, delaying instantiation that includes
  // "Other::iterator" until Other is known to be a List type.
  template<typename Other, typename Iterator>
  struct HasConvertibleIterator
    : public BoolConstant<std::is_convertible<typename Other::iterator, Iterator>::value> {};

  // A subsequence of one list can be transferred to another list via splice
  // if the lists have the same (ignoring const qualifiers) element type, use
  // the same entry member, and either the receiver is a const-element list
  // or neither is a const-element list.  A const element of a list cannot be
  // transferred to a list with non-const elements.  That would effectively be
  // a quiet casting away of const.  Assuming Other is a List, these
  // constraints are equivalent to the constraints on conversion of
  // Other::iterator -> iterator.
  template<typename Other>
  static constexpr bool can_splice_from() {
    return Conjunction<Impl::IsListType<Other>,
                       HasConvertibleIterator<Other, iterator>>::value;
  }

public:
  // The use of SFINAE with splice() and swap() is done for two reasons.
  //
  // It provides better compile-time error messages for certain kinds of usage
  // mistakes.  For example, if a splice from_list is not actually a list, or
  // a list with a different get_entry function, we get some kind of "no
  // applicable function" failure at the call site, rather than some obscure
  // access failure deep inside the implementation of the operation.
  //
  // It ensures const-correctness at the API boundary, permitting the
  // implementation to be simpler by decaying to const iterators and
  // references in various places.

  /**
   * Transfers the elements of from_list in the range designated by
   * from and to to this list, inserted before pos.  Returns an
   * iterator referring to the head of the spliced in range.  Does
   * not invalidate any iterators.
   *
   * precondition: pos must be a valid iterator for this list.
   * precondition: from and to must form a valid range for from_list.
   * precondition: pos is not in the range to transfer, i.e. either
   * - this != &from_list, or
   * - pos is reachable from to, or
   * - pos is not reachable from from.
   *
   * postcondition: iterators referring to elements in the transferred range
   * are valid iterators for this list rather than from_list.
   *
   * complexity: constant if either (a) this == &from_list, (b) neither
   * this nor from_list has a constant-time size() operation, or (c)
   * from_list has a constant-time size() operation and is being
   * transferred in its entirety; otherwise O(number of elements
   * transferred).
   */

  template<typename FromList, ENABLE_IF(can_splice_from<FromList>())>
  void splice(const_iterator pos,
              FromList& from_list,
              typename FromList::iterator from,
              typename FromList::const_iterator to) {
    // TODO: assert that first and last are in same list.
    // TODO: assert that first and last in the other list.
    if (from == to) {
      return; // Done if empty range
    }

    size_t transferred = 0;
    if (from == from_list.cbegin() && to == from_list.cend()) {
      transferred = from_list.size();
    } else {
      transferred = from_list.distance(from, to);
    }
    transfer(pos, from_list, from, to);
    this->adjust_size(transferred);
    from_list.adjust_size(-transferred);
  }

  /**
   * Transfers all elements of from_list to this list, inserted before
   * pos.  Returns an iterator referring to the head of the spliced in
   * range.  Does not invalidate any iterators.
   *
   * precondition: pos must be a valid iterator for this list.
   * precondition: this != &from_list.
   *
   * postcondition: iterators referring to elements that were in
   * from_list are valid iterators for this list rather than
   * from_list.
   *
   * Complexity: constant if either (a) this does not have a
   * constant-time size() operation, or (b) from_list has a
   * constant-time size() operation; otherwise O(number of elements
   * transferred).
   */
  template<typename FromList, ENABLE_IF(can_splice_from<FromList>())>
  void splice(const_iterator pos, FromList& from_list) {
    splice(pos, from_list, from_list.begin(), from_list.end());
    assert(from_list.begin() == from_list.end(), "check that list is moved from");
  }

  /**
   * Transfers the element of from_list referred to by from to this
   * list, inserted before pos.  Returns an iterator referring to the
   * inserted element.  Does not invalidate any iterators.
   *
   * precondition: pos must be a valid iterator for this list.
   * precondition: from must be a dereferenceable iterator of from_list.
   * precondition: pos is not in the range to transfer, i.e. if
   * this == &from_list then pos != from.
   *
   * postcondition: iterators referring to the transferred element are
   * valid iterators for this list rather than from_list.
   *
   * complexity: constant.
   */
  template<typename FromList, ENABLE_IF(can_splice_from<FromList>())>
  void splice(const_iterator pos,
              FromList& from_list,
              typename FromList::const_iterator from) {
    ListEntryPtr to_unlink = from.cur_entry();
    Ops::unlink(to_unlink);
    Ops::link_before(pos.cur_entry(), to_unlink);
    from_list.adjust_size(-1);
    this->adjust_size(1);
  }

private:
  // Helper for can_swap, delaying instantiation that includes
  // "Other::iterator" until Other is known to be a List type.
  template<typename Other, typename Iterator>
  struct HasSameIterator
    : public BoolConstant<std::is_same<typename Other::iterator, Iterator>::value>
  {};

  // Swapping can be thought of as bi-directional slicing (see
  // can_splice_from).  So Other::iterator must be the same as iterator.
  template<typename Other>
  static constexpr bool can_swap() {
    return Conjunction<Impl::IsListType<Other>,
                       HasSameIterator<Other, iterator>>::value;
  }
public:

  /**
   * Exchange the elements of this list and other, maintaining the order of
   * the elements.  TODO: Does not invalidate any iterators.
   *
   * precondition: this and other are different lists.
   *
   * TODO: postcondition: iterators referring to elements in this list become valid
   * iterators for other, and vice versa.
   *
   * constant as the lists have constant-time size
   */

  void swap(IntrusiveList& other) {
    // TODO: assert(!is_same_list(other), "self-swap");
    IntrusiveList temp{};
    temp.splice(temp.begin(), other);
    other.splice(other.begin(), *this);
    splice(begin(), temp);
  }

};

/**
 * Bi-directional constant (e.g. not output) iterator for iterating
 * over the elements of an IntrusiveList.  The IntrusiveList class
 * uses specializations of this class as its iterator types.
 *
 * An iterator may be either const-element or non-const-element.  The value
 * type of a const-element iterator is const-qualified, and a const-element
 * iterator only provides access to const-qualified elements.  Similarly, a
 * non-const-element iterator provides access to unqualified elements.  A
 * non-const-element iterator can be converted to a const-element iterator,
 * but not vice versa.
 */
template<typename T,
         IntrusiveListEntryAccessor<T> accessor,
         bool is_forward>
class IntrusiveListImpl::IteratorImpl {

  friend class IntrusiveListImpl;

  static const bool _is_const_element = std::is_const<T>::value;

  using Impl = IntrusiveListImpl;
  using ListEntryPtr = typename Impl::ListEntryPtr;
  using Ops = Impl::ListOperations<T, accessor>;

public:
  /** Type of an iterator's value. */
  using value_type = T;

  /** Type of a reference to an iterator's value. */
  using reference = std::add_lvalue_reference_t<value_type>;

  /** Type of a pointer to an iterator's value. */
  using pointer = std::add_pointer_t<value_type>;

  /** Type for distance between iterators. */
  using difference_type = typename Impl::difference_type;

  // Test whether From is an iterator type different from this type that can
  // be implicitly converted to this iterator type.  A const_element iterator
  // type supports implicit conversion from the corresponding
  // non-const-element iterator type.
  template<typename From>
  static constexpr bool is_convertible_iterator() {
    using NonConst = IteratorImpl<std::remove_const_t<T>, accessor, is_forward>;
    return _is_const_element && std::is_same<From, NonConst>::value;
  }

private:
  // An iterator refers to either an object in the list, the root
  // entry of the list, or null if singular.  See ListOperations
  // for details of the encoding.
  ListEntryPtr _cur_entry;


  // Get the predecessor / successor (according to the iterator's
  // direction) of the argument.  Reference arguments are preferred;
  // the iterator form should only be used when the iterator is not
  // already known to be dereferenceable.  (The iterator form of
  // successor is not provided; for an iterator to have a successor,
  // the iterator must be dereferenceable.)
  ListEntryPtr successor(ListEntryPtr cur) {
    return is_forward ? Ops::next(cur) : Ops::prev(cur);
  }

  ListEntryPtr predecessor(ListEntryPtr cur) {
    return is_forward ? Ops::prev(cur) : Ops::next(cur);
  }

public:
  // TODO: should be made private
  // Allow explicit construction from an encoded const void*
  // value.  But require exactly that type, disallowing any implicit
  // conversions.  Without that restriction, certain kinds of usage
  // errors become both more likely and harder to diagnose the
  // resulting compilation errors.  [The remaining diagnostic
  // difficulties could be eliminated by making EncodedValue a non-public
  // class for carrying the encoded void* to iterator construction.]
  template<typename EncodedValue,
           ENABLE_IF(std::is_same<EncodedValue, ListEntryPtr>::value)>
  explicit IteratorImpl(EncodedValue encoded_value)
    : _cur_entry(encoded_value)
  {}
  /** Construct a singular iterator. */
  IteratorImpl() : _cur_entry(nullptr) {}

  ~IteratorImpl() = default;
  IteratorImpl(const IteratorImpl& other) = default;
  IteratorImpl& operator=(const IteratorImpl& other) = default;

  /** Implicit conversion from non-const to const element type. */
  template<typename From, ENABLE_IF(is_convertible_iterator<From>())>
  IteratorImpl(const From& other)
    : _cur_entry(other.cur_entry())
  {}

  template<typename From, ENABLE_IF(is_convertible_iterator<From>())>
  IteratorImpl& operator=(const From& other) {
    return *this = IteratorImpl(other);
  }

  ListEntryPtr cur_entry() const { return _cur_entry; }

  /**
   * Return a reference to the iterator's value.
   *
   * precondition: this is dereferenceable.
   * complexity: constant.
   */
  reference operator*() const {
    return const_cast<reference>(*Ops::list_element(_cur_entry));
  }

  /**
   * Return a pointer to the iterator's value.
   *
   * precondition: this is dereferenceable.
   * complexity: constant.
   */
  pointer operator->() const {
    return &this->operator*();
  }

  /**
   * Change this iterator to refer to the successor element (per the
   * iterator's direction) in the list, or to the end of the list.
   * Return a reference to this iterator.
   *
   * precondition: this is dereferenceable.
   * postcondition: this is dereferenceable or end-of-list.
   * complexity: constant.
   */
  IteratorImpl& operator++() {
    _cur_entry = successor(_cur_entry);
    return *this;
  }

  /**
   * Make a copy of this iterator, then change this iterator to refer
   * to the successor element (per the iterator's direction) in the
   * list, or to the end of the list.  Return the copy.
   *
   * precondition: this is dereferenceable.
   * postcondition: this is dereferenceable or end-of-list.
   * complexity: constant.
   */
  IteratorImpl operator++(int) {
    IteratorImpl result = *this;
    _cur_entry = successor(_cur_entry);
    return result;
  }

  /**
   * Change this iterator to refer to the preceeding element (per the
   * iterator's direction) in the list.  Return a reference to this
   * iterator.
   *
   * precondition: There exists an iterator i such that ++i equals this.
   * postcondition: this is dereferenceable.
   * complexity: constant.
   */
  IteratorImpl& operator--() {
    //TODO: IOps::assert_is_in_some_list(*this);
    _cur_entry = predecessor(_cur_entry);
    // TODO: Must not have been (r)begin iterator.
    // TODO: assert(!IOps::is_root_entry(*this), "iterator decrement underflow");
    return *this;
  }

  /**
   * Make a copy of this iterator, then change this iterator to refer
   * to the preceeding element (per the iterator's direction) in the
   * list.  Return the copy.
   *
   * precondition: There exists an iterator i such that ++i equals this.
   * postcondition: this is dereferenceable.
   * complexity: constant.
   */
  IteratorImpl operator--(int) {
    IteratorImpl result = *this;
    this->operator--();
    return result;
  }

  /**
   * Return true if this and other refer to the same element of a list,
   * or both refer to end-of-list.
   *
   * precondition: this and other are both dereferenceable or end-of-list.
   * complexity: constant.
   */
  bool operator==(const IteratorImpl& other) const {
    return _cur_entry == other.cur_entry();
  }

  /**
   * Return true if this and other are not ==.
   *
   * precondition: this and other are both dereferenceable or end-of-list.
   * complexity: constant.
   */
  bool operator!=(const IteratorImpl& other) const {
    return !(*this == other);
  }

  // Add ConvertibleFrom OP IteratorImpl overloads, because these are
  // not handled by the corresponding member function plus implicit
  // conversions.  For example, const_iterator == iterator is handled
  // by const_iterator::operator==(const_iterator) plus implicit
  // conversion of iterator to const_iterator.  But we need an
  // additional overload to handle iterator == const_iterator when those
  // types are different.
  template<typename From, ENABLE_IF(is_convertible_iterator<From>())>
  friend bool operator==(const From& lhs, const IteratorImpl& rhs) {
    return rhs == lhs;
  }

  template<typename From, ENABLE_IF(is_convertible_iterator<From>())>
  friend bool operator!=(const From& lhs, const IteratorImpl& rhs) {
    return rhs != lhs;
  }
};

#endif // SHARE_UTILITIES_INTRUSIVELIST_HPP
