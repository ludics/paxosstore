#pragma once

namespace certain {

// Intrusive doubly-linked list with the same sentinel rules as LIGHTLIST:
//   * first_/last_ are typed T* but may point at *this (the list header)
//   * an unlinked hook is next == prev == nullptr
//   * end nodes' next/prev may point at the header
//
// The pointer stored in a T* slot is written with reinterpret_cast, not
// *(void**). Callers must not dereference first()/last() when empty().

template <typename T>
struct LightListHook {
  T* next;
  T* prev;

  LightListHook() : next(nullptr), prev(nullptr) {}

  void reset() {
    next = nullptr;
    prev = nullptr;
  }

  bool linked() const { return next != nullptr && prev != nullptr; }
};

// Hook is a non-type pointer-to-member, so the compiler folds it to a
// constant offset (same codegen as elt->field in the macros).
template <typename T, LightListHook<T> T::* Hook>
class LightList {
 public:
  LightList() { clear(); }

  LightList(const LightList&) = delete;
  LightList& operator=(const LightList&) = delete;

  void clear() {
    first_ = AsElt(this);
    last_ = AsElt(this);
  }

  bool empty() const { return IsSentinel(first_); }

  // Undefined when empty() — same as LIGHTLIST_FIRST / LAST.
  T* first() const { return first_; }
  T* last() const { return last_; }

  bool is_sentinel(const T* p) const { return IsSentinel(p); }

  static LightListHook<T>& hook(T* elt) { return elt->*Hook; }
  static const LightListHook<T>& hook(const T* elt) { return elt->*Hook; }

  static bool contains(const T* elt) { return hook(elt).linked(); }

  T* next(T* elt) const { return hook(elt).next; }
  T* prev(T* elt) const { return hook(elt).prev; }

  void insert_head(T* elt) {
    LightListHook<T>& h = hook(elt);
    h.next = first_;
    h.prev = AsElt(this);
    if (empty()) {
      last_ = elt;
    } else {
      hook(first_).prev = elt;
    }
    first_ = elt;
  }

  void insert_tail(T* elt) {
    LightListHook<T>& h = hook(elt);
    h.next = AsElt(this);
    h.prev = last_;
    if (empty()) {
      first_ = elt;
    } else {
      hook(last_).next = elt;
    }
    last_ = elt;
  }

  void remove(T* elt) {
    LightListHook<T>& h = hook(elt);
    if (IsSentinel(h.next)) {
      last_ = h.prev;
    } else {
      hook(h.next).prev = h.prev;
    }
    if (IsSentinel(h.prev)) {
      first_ = h.next;
    } else {
      hook(h.prev).next = h.next;
    }
    h.reset();
  }

 private:
  static T* AsElt(void* p) { return reinterpret_cast<T*>(p); }

  static const T* AsElt(const void* p) {
    return reinterpret_cast<const T*>(p);
  }

  bool IsSentinel(const T* p) const {
    return static_cast<const void*>(p) == static_cast<const void*>(this);
  }

  T* first_;
  T* last_;
};

}  // namespace certain
