// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <cpp/when.h>
#include <debug/harness.h>

using namespace verona::cpp;

class Child
{
  int id;
public:
  Child(int id_) : id(id_)
  {}

  ~Child()
  {
    Logging::cout() << "Child destroyed" << Logging::endl;
  }

  void fn()
  {
    std::cout << "Child : " << id << std::endl;
  }
};

class Parent
{
  std::vector<cown_ptr<Child>> children;
  std::vector<nested_cown_ptr<Child, Parent>> nested_children;

public:
  void add_child(cown_ptr<Child> c)
  {
    children.push_back(c);
  }

  void add_nested_child(nested_cown_ptr<Child, Parent> c)
  {
    nested_children.push_back(c);
  }

  ~Parent()
  {
    Logging::cout() << "Parent destroyed" << Logging::endl;
  }

  void access_nested_children(acquired_cown<Parent> &ac)
  {
    Child *c1 = nested_children[0].get_object_if_parent(ac);
    assert(c1 != nullptr);

    c1->fn();
  }

  void access_children()
  {
    std::cout << "Parent will schedule on chidren asynchronously\n";
    when(children[0]) << [=](auto c) { c->fn(); };
    when(children[1]) << [=](auto c) { c->fn(); };
  }
};


void test_body()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto pcown = make_cown<Parent>();
  auto ccown1 = make_cown<Child>(1);
  auto ccown2 = make_cown<Child>(2);

  when(pcown) << [=](auto p) { p->add_child(ccown1); };
  when(pcown) << [=](auto p) { p->add_child(ccown2); };

  when(pcown) << [=](auto p) { p->access_children(); };

  // I shouldn't be able to do that to the nested children though
  when(ccown1) << [=](auto c) { c->fn(); };
  when(ccown2) << [=](auto c) { c->fn(); };

  // instead it should be transformed to something like that under the hood
  when(ccown1, read(pcown)) << [=](auto c, auto p) { c->fn(); };
  when(ccown2, read(pcown)) << [=](auto c, auto p) { c->fn(); };

  // This is a sketch of the expected API
  auto nccown1 = make_nested_cown<Child, Parent>(pcown, 42);
  when(pcown) << [=](auto p) { p->add_nested_child(nccown1); };

  when(pcown) << [=](auto p) { p->access_nested_children(p); };

  auto pcown2 = make_cown<Parent>();
#if 0
  // The next should fail
  when(pcown2) << [=](auto p) mutable {
    Child *c = nccown1.get_object_if_parent(p);
    assert(c != nullptr);
    c->fn();
  };
#endif

  // I didn't want to have the parent there, but the compiler complained
   when(nccown1) << [=](auto c, auto p) { c->fn(); };
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(test_body);

  return 0;
}
