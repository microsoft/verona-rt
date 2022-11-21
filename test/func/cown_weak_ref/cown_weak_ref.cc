// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
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

struct MyCown : VCown<MyCown>
{
  MyCown* parent; // Weak

  MyCown* left; // Strong
  MyCown* right; // Strong

  size_t up_count = 0;

  void trace(ObjectStack& os) const
  {
    if (left != nullptr)
      os.push(left);
    if (right != nullptr)
      os.push(right);

    // Do not push parent, as this is a weak reference.
  }

  ~MyCown()
  {
    if (parent != nullptr)
      parent->weak_release(ThreadAlloc::get());

    Logging::cout() << "Destroying " << this << " up_count " << up_count
                    << std::endl;
  }
};

const char* spaces[] = {
  "               ",
  "              ",
  "             ",
  "            ",
  "           ",
  "          ",
  "         ",
  "        ",
  "       ",
  "      ",
  "     ",
  "    ",
  "   ",
  "  ",
  " ",
  "",
};

MyCown* make_tree(int n, MyCown* p)
{
  if (n == 0)
    return nullptr;

  auto c = new MyCown;

  Logging::cout() << "Cown " << spaces[n] << c << std::endl;

  c->left = make_tree(n - 1, c);
  c->right = make_tree(n - 1, c);
  if (p != nullptr)
    p->weak_acquire();
  c->parent = p;
  return c;
}

struct Up
{
  MyCown* m;
  Up(MyCown* m) : m(m) {}

  static void weak_send(MyCown* m)
  {
    if (m->parent != nullptr)
    {
      if (m->parent->acquire_strong_from_weak())
      {
        Behaviour::schedule<Up, YesTransfer>(m->parent, m->parent);
      }
    }
  }

  void operator()()
  {
    Logging::cout() << "Up on " << m << std::endl;

    m->up_count++;

    Up::weak_send(m);
  }
};

struct Down
{
  MyCown* m;
  Down(MyCown* m) : m(m) {}

  void operator()()
  {
    Logging::cout() << "Down on " << m << std::endl;

    Up::weak_send(m);

    if (m->left != nullptr)
    {
      Behaviour::schedule<Down>(m->left, m->left);
    }

    if (m->right != nullptr)
    {
      Behaviour::schedule<Down>(m->right, m->right);
    }
  }
};

void run_test()
{
  auto t = make_tree(9, nullptr);

  Behaviour::schedule<Down>(t, t);
  Behaviour::schedule<Down, YesTransfer>(t, t);
}

int main(int argc, char** argv)
{
  SystematicTestHarness h(argc, argv);

  h.run(run_test);
}
