#ifndef SHARE_UTILITIES_INTRUSIVELIST_INLINE_HPP
#define SHARE_UTILITIES_INTRUSIVELIST_INLINE_HPP

#include "utilities/intrusiveList.hpp"

template<typename T, IntrusiveListEntryAccessor<T> get_entry>
inline typename IntrusiveList<T, get_entry>::iterator IntrusiveList<T, get_entry>::begin() {
  return iterator(Ops::next(root_entry()));
}

template<typename T, IntrusiveListEntryAccessor<T> get_entry>
inline typename IntrusiveList<T, get_entry>::const_iterator IntrusiveList<T, get_entry>::begin() const {
  return cbegin();
}

template<typename T, IntrusiveListEntryAccessor<T> get_entry>
inline typename IntrusiveList<T, get_entry>::const_iterator IntrusiveList<T, get_entry>::cbegin() const {
  return const_iterator(Ops::next(root_entry()));
}

template<typename T, IntrusiveListEntryAccessor<T> get_entry>
inline typename IntrusiveList<T, get_entry>::iterator IntrusiveList<T, get_entry>::end() {
  return iterator(root_entry());
}

template<typename T, IntrusiveListEntryAccessor<T> get_entry>
inline typename IntrusiveList<T, get_entry>::const_iterator IntrusiveList<T, get_entry>::end() const {
  return cend();
}

template<typename T, IntrusiveListEntryAccessor<T> get_entry>
inline typename IntrusiveList<T, get_entry>::const_iterator IntrusiveList<T, get_entry>::cend() const {
  return const_iterator(root_entry());
}

template<typename T, IntrusiveListEntryAccessor<T> get_entry>
inline typename IntrusiveList<T, get_entry>::reverse_iterator IntrusiveList<T, get_entry>::rbegin() {
  return reverse_iterator(Ops::prev(root_entry()));
}

template<typename T, IntrusiveListEntryAccessor<T> get_entry>
inline typename IntrusiveList<T, get_entry>::const_reverse_iterator IntrusiveList<T, get_entry>::rbegin() const {
  return crbegin();
}

template<typename T, IntrusiveListEntryAccessor<T> get_entry>
inline typename IntrusiveList<T, get_entry>::const_reverse_iterator IntrusiveList<T, get_entry>::crbegin() const {
  return const_reverse_iterator(Ops::prev(root_entry()));
}

template<typename T, IntrusiveListEntryAccessor<T> get_entry>
inline typename IntrusiveList<T, get_entry>::reverse_iterator IntrusiveList<T, get_entry>::rend() {
  return reverse_iterator(root_entry());
}

template<typename T, IntrusiveListEntryAccessor<T> get_entry>
inline typename IntrusiveList<T, get_entry>::const_reverse_iterator IntrusiveList<T, get_entry>::rend() const {
  return crend();
}

template<typename T, IntrusiveListEntryAccessor<T> get_entry>
inline typename IntrusiveList<T, get_entry>::const_reverse_iterator IntrusiveList<T, get_entry>::crend() const {
  return const_reverse_iterator(root_entry());
}

#ifdef ASSERT
inline IntrusiveListEntry::~IntrusiveListEntry() {
  assert(_list == nullptr, "deleting list entry while in list");
  assert(_prev == nullptr, "invariant");
  assert(_next == nullptr, "invariant");
}

IntrusiveListImpl::~IntrusiveListImpl() {
  assert(is_tagged_root_entry(_root._prev), "deleting non-empty list");
  assert(is_tagged_root_entry(_root._next), "deleting non-empty list");
  // Clear _root's information before running its asserting destructor.
  _root._prev = nullptr;
  _root._next = nullptr;
  _root._list = nullptr;
}
#endif // ASSERT

#endif // SHARE_UTILITIES_INTRUSIVELIST_INLINE_HPP
