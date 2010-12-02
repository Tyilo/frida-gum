#include "gumpp.hpp"

#include "podwrapper.hpp"

#include <gum/gum-heap.h>
#include <iostream>

namespace Gum
{
  class SanityCheckerImpl : public PodWrapper<SanityCheckerImpl, SanityChecker, GumSanityChecker>
  {
  public:
    SanityCheckerImpl ()
    {
      assign_handle (gum_sanity_checker_new (output_to_stderr, NULL));
    }

    virtual void begin (unsigned int flags)
    {
      gum_sanity_checker_begin (handle, flags);
    }

    virtual bool end ()
    {
      return gum_sanity_checker_end (handle) != FALSE;
    }

  protected:
    virtual void destroy_handle ()
    {
      gum_sanity_checker_destroy (handle);
    }

    static void output_to_stderr (const gchar * text, gpointer user_data)
    {
      (void) user_data;

      std::cerr << text;
    }
  };

  extern "C" SanityChecker * SanityChecker_new (void) { gum_init (); return new SanityCheckerImpl; }
}