// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "../object/object.h"
#include "ds/hashmap.h"
#include "externalreference.h"
#include "immutable.h"

#include <snmalloc/snmalloc.h>

namespace verona::rt
{
  using namespace snmalloc;

  class RememberedSet
  {
    friend class RegionTrace;
    friend class RegionArena;

  private:
    using HashSet = ObjectMap<Object*>;
    HashSet* hash_set;

  public:
    RememberedSet() : hash_set(HashSet::create()) {}

    inline void dealloc()
    {
      discard(false);
      hash_set->dealloc();
      heap::dealloc<sizeof(HashSet)>(hash_set);
    }

    /**
     * Add the objects from another set to this set.
     */
    void merge(RememberedSet* that)
    {
      for (auto* e : *that->hash_set)
      {
        // If q is already present in this, decref, otherwise insert.
        // No need to call release, as the rc will not drop to zero.
        if (!hash_set->insert(e).first)
        {
          e->decref();
        }
      }
    }

    /**
     * Add an object into the set. If the object is not already present, incref
     * and add it to the set.
     */
    template<TransferOwnership transfer>
    void insert(Object* o)
    {
      assert(o->debug_is_rc() || o->debug_is_shared());

      // If the caller is not transfering ownership of a refcount, i.e., the
      // object is being added to the region but not dropped from somewhere,
      // we need to incref it.
      if constexpr (transfer == NoTransfer)
        o->incref();

      if (!hash_set->insert(o).first)
      {
        // If the caller is transfering ownership of a refcount, i.e., the
        // object is being moved from somewhere to this region, but the object
        // is already here, we need to decref it.
        if constexpr (transfer == YesTransfer)
          o->decref();
      }
    }

    /**
     * Mark the given object. If the object is not in the set, incref and add it
     * to the set.
     */
    void mark(Object* o)
    {
      assert(o->debug_is_rc() || o->debug_is_shared());

      auto r = hash_set->insert(o);
      if (r.first)
        o->incref();

      r.second.mark();
    }

    /**
     * Erase all unmarked entries from the set and unmark the remaining entries.
     */
    void sweep()
    {
      for (auto it = hash_set->begin(); it != hash_set->end(); ++it)
      {
        if (!it.is_marked())
        {
          RememberedSet::release_internal(*it);
          hash_set->erase(it);
        }
        else
        {
          it.unmark();
        }
      }
    }

    /**
     * Erase all entries from the set. If `release` is true, the remaining
     * objects will be released.
     */
    void discard(bool release = true)
    {
      for (auto it = hash_set->begin(); it != hash_set->end(); ++it)
      {
        if (release)
          RememberedSet::release_internal(*it);

        hash_set->erase(it);
      }
      hash_set->clear();
    }

  private:
    static void release_internal(Object* o)
    {
      switch (o->get_class())
      {
        case Object::RC:
        {
          assert(o->debug_is_immutable());
          Logging::cout() << "RS releasing: immutable: " << o << Logging::endl;
          Immutable::release(o);
          break;
        }

        case Object::SHARED:
        {
          Logging::cout() << "RS releasing: cown: " << o << Logging::endl;
          shared::release(o);
          break;
        }

        default:
          abort();
      }
    }
  };
} // namespace verona::rt
