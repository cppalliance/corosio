//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//
// Fixture for selftest.mjs. Its ONLY doc comments are `///` runs, so it produces
// no output at all unless extract-docstrings.mjs's `///` branch works. The real
// header tree no longer contains such a file (every `///`-only header lived under
// detail/, which is excluded), which is why this is a fixture and not a probe.
#ifndef BOOST_COROSIO_FIXTURE_SLASH_ONLY_HPP
#define BOOST_COROSIO_FIXTURE_SLASH_ONLY_HPP

namespace boost::corosio {

/// Reports whether the fixture extracted.
/// A second line, folded into the same doc comment.
void slash_only_public();

namespace detail {

/// This one is inside namespace detail and must NOT be extracted.
void slash_only_detail();

} // namespace detail
} // namespace boost::corosio

#endif
