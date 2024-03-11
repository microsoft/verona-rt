// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../object/object.h"
#include "linked_object_stack.h"

namespace verona::rt
{
  class Shared;
  namespace shared
  {
    // This is used only to break a dependency cycle.
    inline void release(Alloc& alloc, Object* o);
  } // namespace shared

  class Immutable
  {
  public:
    static void acquire(Object* o)
    {
      assert(o->debug_is_immutable());
      o->immutable()->incref();
    }

    static size_t release(Alloc& alloc, Object* o)
    {
      assert(o->debug_is_immutable());
      auto root = o->immutable();

      if (root->decref())
        return free(alloc, root);

      return 0;
    }

  private:
    static size_t free(Alloc& alloc, Object* o)
    {
      assert(o == o->immutable());
      size_t total = 0;

      // Free immutable graph.
      ObjectStack f(alloc);
      LinkedObjectStack fl;
      LinkedObjectStack scc;
      LinkedObjectStack dfs;

      dfs.push(o);

      while (!dfs.empty())
      {
        assert(f.empty());
        assert(fl.empty());
        assert(scc.empty());

        scc.push(dfs.pop());

        while (!scc.empty())
        {
          Object* w = scc.pop();
          fl.push(w);
          w->trace(f);

          while (!f.empty())
          {
            Object* u = f.pop();
            scc_classify(alloc, u, dfs, scc);
          }
        }

        // Run all finalisers for this SCC before deallocating.
        fl.forall<run_finaliser>();

        while (!fl.empty())
        {
          Object* w = fl.pop();
          total += w->size();
          w->destructor();
          w->dealloc(alloc);
        }
      }

      assert(f.empty());
      assert(fl.empty());
      assert(scc.empty());
      assert(dfs.empty());

      return total;
    }

    static inline void run_finaliser(Object* o)
    {
      // We don't need the actual subregions here, as they have been frozen.
      ObjectStack dummy(ThreadAlloc::get());
      o->finalise(nullptr, dummy);
    }

    static inline void scc_classify(
      Alloc& alloc, Object* w, LinkedObjectStack& dfs, LinkedObjectStack& scc)
    {
      Object::RegionMD c;
      Object* r = w->root_and_class(c);

      switch (c)
      {
        case Object::RC:
        {
          if (r->decref())
            dfs.push(r);
          break;
        }

        case Object::UNMARKED:
        {
          if (w != r)
          {
            scc.push(w);
          }
          break;
        }

        case Object::SHARED:
        {
          Logging::cout() << "Immutable releasing cown: " << w << Logging::endl;
          shared::release(alloc, w);
          break;
        }

        default:
          assert(0);
      }
    }
  };

  namespace immutable
  {
    // This is used only to break a dependency cycle.
    inline void release(Alloc& alloc, Object* o)
    {
      Immutable::release(alloc, o);
    }
  } // namespace cown
} // namespace verona::rt
