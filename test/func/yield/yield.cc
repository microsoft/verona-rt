// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT

#include <cpp/when.h>
#include <debug/harness.h>

#define BEHAVIOUR_YIELD(X) \
  { \
    verona::rt::Behaviour::behaviour_rerun() = true; \
    return X; \
  }

class Counter
{
public:
  int c;

  Counter() : c(0) {}
};

class ObjectWithState
{
public:
  enum State
  {
    StateA = 0,
    StateB,
    StateC
  };
  State s;

  ObjectWithState() : s(StateA) {}
};

using namespace verona::cpp;

void test_state_machine()
{
  Logging::cout() << "Yield state machine test" << Logging::endl;
  auto state_cown = make_cown<ObjectWithState>();

  when(state_cown) << [](acquired_cown<ObjectWithState> state) {
    switch (state->s)
    {
      case ObjectWithState::StateA:
        Logging::cout() << "In state A" << Logging::endl;
        state->s = ObjectWithState::StateB;
        BEHAVIOUR_YIELD();
        break;
      case ObjectWithState::StateB:
        Logging::cout() << "In state B" << Logging::endl;
        state->s = ObjectWithState::StateC;
        BEHAVIOUR_YIELD();
        break;
      case ObjectWithState::StateC:
        Logging::cout() << "In state C" << Logging::endl;
        break;
    }
  };
}

void test_counter()
{
  Logging::cout() << "Yield couter test" << Logging::endl;

  auto counter_cown = make_cown<Counter>();

  when(counter_cown) << [](acquired_cown<Counter> counter) {
    // Ensure that the next behaviour does not run
    assert(counter->c % 2 == 0);
    while (counter->c < 10)
    {
      counter->c += 2;
      Logging::cout() << "Yielding at counter = " << counter->c
                      << Logging::endl;
      BEHAVIOUR_YIELD();
    }
  };

  when(counter_cown) << [](acquired_cown<Counter> counter) {
    assert(counter->c == 10);
    Logging::cout() << "Incrementing counter by 1" << Logging::endl;
    counter->c++;
  };
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  Logging::cout() << "Yield test" << Logging::endl;

  harness.run(test_counter);
  harness.run(test_state_machine);

  return 0;
}
