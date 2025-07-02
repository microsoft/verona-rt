// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <cpp/when.h>
#include <debug/harness.h>

/**
 * This test case produces a binary tree with weak parent pointers.  The
 * root is sent two Down messages.
 *
 * When a node receives a Down message it sends an Up message to its parent if
 * it is still alive, and a Down message to each of its children.
 *
 * When a node receives an Up message it sends an Up Message to its parent if
 * it is still alive.
 *
 * This tests a race between the Up messages being sent, and the parent
 * finishing processing its down messages. It is possible for 0 to all of the Up
 * messages to be received.
 **/

using namespace verona::cpp;

struct MyCown
{
  cown_ptr<MyCown>::weak parent;

  cown_ptr<MyCown> left;
  cown_ptr<MyCown> right;

  size_t up_count = 0;

  MyCown(cown_ptr<MyCown>::weak&& parent) : parent(parent)
  {
    Logging::cout() << "Creating " << this << std::endl;
  }

  MyCown(const MyCown&) = default;
  MyCown(MyCown&&) = default;

  ~MyCown()
  {
    Logging::cout() << "Destroying " << this << " up_count " << up_count
                    << std::endl;
  }
};

cown_ptr<MyCown> make_tree(int n, cown_ptr<MyCown>::weak&& p)
{
  if (n == 0)
    return {};

  auto c = make_cown<MyCown>(std::move(p));

  when(c, [n](acquired_cown<MyCown> c) {
    c->left = make_tree(n - 1, c.cown());
    c->right = make_tree(n - 1, c.cown());
    Logging::cout() << "Creating " << c << " with n = " << n << std::endl;
  });

  return c;
}

void up(acquired_cown<MyCown>& c)
{
  auto parent = c->parent.promote();
  if (parent == nullptr)
    return;

  Logging::cout() << "Parent is alive" << std::endl;
  when(std::move(parent), [](acquired_cown<MyCown> c) {
    c->up_count++;
    Logging::cout() << "Up on " << c << std::endl;
    up(c);
  });
}

void down(cown_ptr<MyCown>& c)
{
  if (c == nullptr)
    return;

  when(c, [](acquired_cown<MyCown> c) {
    Logging::cout() << "Down on " << c << std::endl;

    up(c);
    down(c->left);
    down(c->right);
  });
}

void run_test()
{
  auto t = make_tree(9, {});

  down(t);
  down(t);
}

int main(int argc, char** argv)
{
  SystematicTestHarness h(argc, argv);

  h.run(run_test);
}
