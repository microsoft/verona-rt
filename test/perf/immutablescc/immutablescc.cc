// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

// Compile runtime to measure the length of all find operations
#define VERONA_BENCHMARK_SCC_FIND_STATS

#include <iomanip>
#include <iostream>
#include <test/measuretime.h>
#include <verona.h>

using namespace snmalloc;
using namespace verona::rt;
using namespace verona::rt::api;

bool abort_on_dtor = false;

template<bool Order = false>
struct C1 : public V<C1<Order>>
{
  Object* f1{nullptr};
  Object* f2{nullptr};

  void trace(ObjectStack& st) const
  {
    if (Order)
    {
      if (f1 != nullptr)
        st.push(f1);
    }

    if (f2 != nullptr)
      st.push(f2);

    if (!Order)
    {
      if (f1 != nullptr)
        st.push(f1);
    }
  }
};

/**
 * Create a linked list of a given size.
 *
 * If Cyclic is true, then each node is linked to the previous
 * that is a doubly linked list.
 */
template<bool Cyclic = true, bool Order = false>
Object* make_list(size_t list_size)
{
  auto* curr = new C1<Order>;
  auto* root = curr;
  curr->f2 = nullptr;
  for (int i = 0; i < list_size; i++)
  {
    auto* next = new C1<Order>;
    curr->f1 = next;
    if constexpr (Cyclic)
      next->f2 = curr;
    else
      next->f2 = nullptr;
    curr = next;
  }
  curr->f1 = nullptr;
  return root;
}

/**
 * Creates a balanced binary tree where the leaves are `leaf`.
 */
Object* make_sub_tree(size_t tree_size, Object* leaf)
{
  if (tree_size == 0)
    return leaf;

  auto curr = new C1;
  tree_size--;

  size_t left_size = tree_size >> 1;

  curr->f1 = make_sub_tree(left_size, leaf);
  curr->f2 = make_sub_tree(tree_size - left_size, leaf);
  return curr;
}

/**
 * Creates a balanced binary tree.
 *
 * It has an additional root node that is used to create cycles from
 * each leaf to the root.  This cycle is only created if the Cyclic
 * parameter is true.
 */
template<bool Cyclic>
Object* make_tree(size_t tree_size)
{
  auto* root = new C1;
  root->f2 = nullptr;
  root->f1 = make_sub_tree(tree_size, Cyclic ? root : nullptr);
  return root;
}

/**
 * Creates a tree with a fixed width.  Each not is represented with
 * TreeWidth nodes forming a linked list. If the Cyclic parameter is true,
 * then the last node in the list is connected to the first.
 *
 * Each child is then given a balanced number of children.
 *
 * The empty node is represented by a nullptr.
 */
template<bool Cyclic, bool Order, size_t TreeWidth>
Object* make_wide_tree(size_t tree_size)
{
  if (tree_size == 0)
    return nullptr;

  size_t width = std::min(tree_size, TreeWidth);
  tree_size = tree_size - width;

  size_t sub_size = tree_size / TreeWidth;
  size_t sub_size_last = tree_size - (sub_size * (TreeWidth - 1));

  auto* root = new C1<Order>;
  auto* curr = root;

  // Make width children
  for (size_t i = 0; i < width - 1; i++)
  {
    auto* next = new C1<Order>;
    curr->f1 = next;
    curr->f2 = make_wide_tree<Cyclic, Order, TreeWidth>(sub_size);
    curr = next;
  }

  if (Cyclic)
    curr->f1 = root;
  else
    curr->f1 = nullptr;

  curr->f2 = make_wide_tree<Cyclic, Order, TreeWidth>(sub_size_last);

  return root;
}

/**
 * Create a graph with many cycles
 *
 * The function should always result in a chain from in to out.
 *
 * If size is 0, then they are directly connected.
 * If size is 1, then they are connected with a single object.
 *
 * If size is >=2, then they are connected with two objects in a chain.
 * The f1 fields are then used recursive to create cycles as well.
 *
 * This effectively creates a parent pointing tree, but using binary nodes.
 * i.e.
 * x->f1 - first child
 * x->f2->f1 - second child
 * x->f2->f2 - parent
 */
template<bool Order>
void make_parent_pointing_tree_inner(size_t size, Object** in, Object* out)
{
  if (size == 0)
  {
    *in = out;
    return;
  }

  size -= 1;
  auto* node1 = new C1<Order>;
  *in = node1;
  if (size == 0)
  {
    node1->f2 = out;
    return;
  }

  size -= 1;
  auto* node2 = new C1<Order>;
  node1->f2 = node2;
  node2->f2 = out;

  size_t right_size = size >> 1;
  size_t left_size = size - right_size;

  make_parent_pointing_tree_inner<Order>(right_size, &node1->f1, node1);
  make_parent_pointing_tree_inner<Order>(left_size, &node2->f1, node1);
}

template<bool Order>
Object* make_parent_pointing_tree(size_t size)
{
  auto* root = new C1<Order>;
  make_parent_pointing_tree_inner<Order>(size, &root->f1, root);
  return root;
}

/**
 * Create a graph with many cycles
 *
 * The f1 field forms a linked list from the first returned value, and the
 * supplied last.
 *
 * The second returned value is an intermediate point in the chain.
 *
 * The f2 fields form back edges in a pattern to create a range of nested
 * cycles.
 */
template<bool Order>
std::pair<Object*, Object*>
make_horrible_cycles_two_inner(size_t size, Object* last)
{
  if (size == 0)
    return {last, last};

  size -= 1;
  auto* new_last = new C1<Order>;

  size_t right_size = size >> 1;
  size_t left_size = size - right_size;

  auto [mid, mid2] =
    make_horrible_cycles_two_inner<Order>(right_size, new_last);
  auto [start, start_mid] =
    make_horrible_cycles_two_inner<Order>(left_size, mid);
  new_last->f1 = start_mid;
  new_last->f2 = last;

  return {start, mid2};
}

template<bool Order>
Object* make_horrible_cycles_two(size_t size)
{
  return make_horrible_cycles_two_inner<Order>(size, nullptr).first;
}

template<typename Make>
void test_alloc_freeze_release(std::string ds, Make make, bool print)
{
  auto& alloc = ThreadAlloc::get();

#ifdef CI_BUILD
  size_t max_index = 10;
#else
  size_t max_index = 22;
#endif

  for (size_t m = 4; m < 8; m++)
    for (size_t index = 1; index < max_index; index++)
    {
      size_t list_size = m << index;
      for (size_t repeats = 0;
           repeats < std::max<size_t>(100000 / list_size, 50);
           repeats++)
      {
        std::vector<Object*> roots;
        size_t work = list_size;
        // Add one for leftover list.
        roots.reserve(1);
        {
          MeasureTime m(true);
          for (size_t i = 0; i < 1; i++)
          {
            auto* root = new (RegionType::Trace) C1;
            UsingRegion rr(root);
            root->f2 = nullptr;
            root->f1 = make(list_size);
            roots.push_back(root);
          }
          if (print)
            std::cout << ds << ",Alloc," << list_size << ","
                      << (double)m.get_time().count() / work << std::endl;
        }

        verona::rt::Object::reset_find_count();
        {
          MeasureTime m(true);
          for (auto root : roots)
            freeze(root);
          if (print)
          {
            std::cout << ds << ",Freeze," << list_size << ","
                      << (double)m.get_time().count() / work << std::endl;
            std::cout << ds << ",FreezeFind," << list_size << ","
                      << (double)verona::rt::Object::get_find_count() / work
                      << std::endl;
          }
        }

        verona::rt::Object::reset_find_count();
        // Free immutable graph.
        {
          MeasureTime m(true);
          for (auto root : roots)
            Immutable::release(alloc, root);
          if (print)
          {
            std::cout << ds << ",Dispose," << list_size << ","
                      << (double)m.get_time().count() / work << std::endl;
            std::cout << ds << ",DisposeFind," << list_size << ","
                      << (double)verona::rt::Object::get_find_count() / work
                      << std::endl;
          }
        }

        // Don't run multiple if not logging results.
        if (!print)
          break;
      }
    }

  snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
  //  std::cerr << std::endl;
}

int main(int, char**)
{
#ifdef CI_BUILD
  int repeats = 1;
#else
  int repeats = 2;
#endif
  for (int i = 0; i < repeats; i++)
  {
    test_alloc_freeze_release("Linked List", make_list<false>, i != 0);
    test_alloc_freeze_release("Doubly Linked List", make_list<true>, i != 0);
    test_alloc_freeze_release(
      "Doubly Linked List (Reverse field order)",
      make_list<true, true>,
      i != 0);
    test_alloc_freeze_release("Balanced Binary Tree", make_tree<false>, i != 0);
    test_alloc_freeze_release(
      "Balanced Binary Tree with Leaf to root cycle", make_tree<true>, i != 0);
    test_alloc_freeze_release(
      "Tree with Parent Pointers", make_parent_pointing_tree<false>, i != 0);
    test_alloc_freeze_release(
      "Tree with Parent Pointers (Reverse field order)",
      make_parent_pointing_tree<true>,
      i != 0);
    test_alloc_freeze_release(
      "Balanced 4-Tree", make_wide_tree<false, false, 4>, i != 0);
    test_alloc_freeze_release(
      "Balanced 4-Tree (Reverse field order)",
      make_wide_tree<false, true, 4>,
      i != 0);
    test_alloc_freeze_release(
      "Balanced 4-Tree with sibling cycle",
      make_wide_tree<true, false, 4>,
      i != 0);
    test_alloc_freeze_release(
      "Balanced 4-Tree with sibling cycle (Reverse field order)",
      make_wide_tree<true, true, 4>,
      i != 0);
    test_alloc_freeze_release(
      "Horrible Cycles II", make_horrible_cycles_two<false>, i != 0);
    test_alloc_freeze_release(
      "Horrible Cycles II (Reverse field order)",
      make_horrible_cycles_two<true>,
      i != 0);
  }
  return 0;
}
