//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_DOC_WARNINGS_HPP
#define BOOST_COROSIO_TEST_DOC_WARNINGS_HPP

/* Warning suppressions shared by every compiled documentation fragment.

   Reference examples deliberately leave results and bindings unused. The
   reference explains those values in prose instead, and adding a use would put
   code on the page that teaches nothing. test/doc builds with -Wall -Wextra
   -Werror (test/doc/CMakeLists.txt and test/doc/Jamfile), so an unused result
   would otherwise fail the build.

   Include this first in a fragment, and always outside every tag:: region --
   the site renders those regions, and scaffolding must not appear on a page.

   The pragmas have no push/pop, so they apply to the rest of the translation
   unit. That is the intent: a fragment is scaffolding plus tagged regions, and
   both need them.

   Keep this list minimal. A warning that fires in one fragment belongs in that
   fragment, under a comment saying why, not here.
*/

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-lambda-capture"
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif
#if defined(_MSC_VER)
#pragma warning(disable : 4834) // discarding [[nodiscard]] return value
#pragma warning(disable : 4189) // local variable initialized but not referenced
#pragma warning(disable : 4100) // unreferenced formal parameter
#pragma warning(disable : 4101) // unreferenced local variable
#pragma warning(disable : 4456) // declaration hides previous local declaration
#pragma warning(disable : 4457) // declaration hides function parameter
#pragma warning(disable : 4458) // declaration hides class member
#pragma warning(disable : 4459) // declaration hides global declaration
#endif

#endif
