// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

namespace verona::rt
{
  namespace detail
  {
    inline bool& behaviour_rerun_requested()
    {
      static thread_local bool requested = false;
      return requested;
    }
  }

  inline void request_behaviour_rerun()
  {
    detail::behaviour_rerun_requested() = true;
  }

  inline bool take_behaviour_rerun_request()
  {
    bool requested = detail::behaviour_rerun_requested();
    detail::behaviour_rerun_requested() = false;
    return requested;
  }
}
