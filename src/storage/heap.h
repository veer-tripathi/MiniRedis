#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// HeapItem — one slot in the min-heap array.
//
// val : the expiry timestamp in monotonic milliseconds (the sort key).
// ref : points to Entry::heap_idx inside the owning Entry.
//       When this item moves to a new array position, we write the new
//       position through ref so the Entry always knows where it lives.
// ---------------------------------------------------------------------------
struct HeapItem {
    uint64_t  val;   // expiry time — smaller = expires sooner = closer to root
    size_t   *ref;   // points to Entry::heap_idx
};

// ---------------------------------------------------------------------------
// Index arithmetic — the tree is implicit in the array layout
// ---------------------------------------------------------------------------
//   Parent of i  :  (i + 1) / 2 - 1
//   Left child   :  i * 2 + 1
//   Right child  :  i * 2 + 2
// ---------------------------------------------------------------------------
static inline size_t heap_parent(size_t i) { return (i + 1) / 2 - 1; }
static inline size_t heap_left  (size_t i) { return i * 2 + 1; }
static inline size_t heap_right (size_t i) { return i * 2 + 2; }

// ---------------------------------------------------------------------------
// heap_up
// ---------------------------------------------------------------------------
// Called when a[pos].val was just DECREASED (made smaller / sooner).
// The node may now be smaller than its parent, violating the heap property.
// Fix: keep swapping with the parent until the parent is smaller or we hit root.
//
// Each swap also updates *ref so the Entry's heap_idx stays correct.
// ---------------------------------------------------------------------------
static inline void heap_up(HeapItem *a, size_t pos) {
    HeapItem t = a[pos];   // save the item we're moving up
    while (pos > 0 && a[heap_parent(pos)].val > t.val) {
        // pull parent down into current slot
        a[pos] = a[heap_parent(pos)];
        *a[pos].ref = pos;             // tell the Entry its new position
        pos = heap_parent(pos);
    }
    a[pos] = t;
    *a[pos].ref = pos;                 // final resting place
}

// ---------------------------------------------------------------------------
// heap_down
// ---------------------------------------------------------------------------
// Called when a[pos].val was just INCREASED (made larger / later).
// The node may now be larger than one or both children.
// Fix: keep swapping with the SMALLER child until both children are larger.
//
// Why the smaller child? If we swapped with the larger one, the larger child
// would become the parent of the smaller child, breaking the heap property.
// ---------------------------------------------------------------------------
static inline void heap_down(HeapItem *a, size_t pos, size_t len) {
    HeapItem t = a[pos];   // save the item we're moving down
    while (true) {
        size_t l = heap_left(pos);
        size_t r = heap_right(pos);
        size_t min_pos = pos;
        uint64_t min_val = t.val;

        if (l < len && a[l].val < min_val) { min_pos = l; min_val = a[l].val; }
        if (r < len && a[r].val < min_val) { min_pos = r; }

        if (min_pos == pos) break;     // both children are larger — done

        a[pos] = a[min_pos];
        *a[pos].ref = pos;             // tell the moved child its new position
        pos = min_pos;
    }
    a[pos] = t;
    *a[pos].ref = pos;
}

// ---------------------------------------------------------------------------
// heap_update
// ---------------------------------------------------------------------------
// Call this after changing a[pos].val in either direction.
// Figures out whether to go up or down automatically.
// ---------------------------------------------------------------------------
static inline void heap_update(HeapItem *a, size_t pos, size_t len) {
    if (pos > 0 && a[heap_parent(pos)].val > a[pos].val) {
        heap_up(a, pos);
    } else {
        heap_down(a, pos, len);
    }
}

// ---------------------------------------------------------------------------
// heap_upsert
// ---------------------------------------------------------------------------
// Insert a new item OR update an existing one.
// If pos < a.size() — the item already exists at that position, just update.
// If pos >= a.size() — append to the end, then fix upward.
// ---------------------------------------------------------------------------
static inline void heap_upsert(std::vector<HeapItem> &a, size_t pos, HeapItem t) {
    if (pos < a.size()) {
        a[pos] = t;                        // update in place
    } else {
        pos = a.size();
        a.push_back(t);                    // append as new last item
    }
    heap_update(a.data(), pos, a.size()); // restore heap property
}

// ---------------------------------------------------------------------------
// heap_delete
// ---------------------------------------------------------------------------
// Remove the item at position pos.
// Strategy: swap it with the last item, shrink the array by 1,
// then fix the heap property for the swapped item (which might go up or down).
//
// Why swap with last? Removing from the middle of an array is O(N).
// Removing the last element is O(1). Swapping first makes it O(1) + O(log N).
// ---------------------------------------------------------------------------
static inline void heap_delete(std::vector<HeapItem> &a, size_t pos) {
    a[pos] = a.back();          // overwrite deleted slot with last item
    *a[pos].ref = pos;          // tell that item's Entry its new position
    a.pop_back();               // shrink array
    if (pos < a.size()) {
        heap_update(a.data(), pos, a.size());  // fix the moved item
    }
}