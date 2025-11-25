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
public:
  void add_child(cown_ptr<Child> c)
  {
    children.push_back(c);
  }

  ~Parent()
  {
    Logging::cout() << "Parent destroyed" << Logging::endl;
  }

  void access_children_directly()
  {
    // TBD
    // I should be able to do
    // children[0].fn();
    // children[1].fn();
    // how does it look like?
    // Is there a nested_cown_ptr?
  }

  void access_children()
  {
    std::cout << "Parent will schedule on chidren asynchronously\n";
    when(children[0]) << [=](auto c) { c->fn(); };
    when(children[1]) << [=](auto c) { c->fn(); };
  }
};

using namespace verona::cpp;

void test_body()
{
  Logging::cout() << "test_body()" << Logging::endl;

  auto pcown = make_cown<Parent>();
  auto ccown1 = make_cown<Child>(1);
  auto ccown2 = make_cown<Child>(2);

  when(pcown) << [=](auto p) { p->add_child(ccown1); };
  when(pcown) << [=](auto p) { p->add_child(ccown2); };

  // I shouldn't be able to do that directly
  when(ccown1) << [=](auto c) { c->fn(); };
  when(ccown2) << [=](auto c) { c->fn(); };

  // Or it should be transformed to something like that under the hood
  when(ccown1, read(pcown)) << [=](auto c, auto p) { c->fn(); };
  when(ccown2, read(pcown)) << [=](auto c, auto p) { c->fn(); };

  auto nccown1 = make_nested_cown<Child, Parent>(pcown, 1);
  nccown1.foo();
  when(nccown1) << [=](auto c) { c->fn(); };
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  harness.run(test_body);

  return 0;
}
