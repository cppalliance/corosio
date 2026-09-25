//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//
// Fixture for selftest.mjs. Lives under detail/, so extract-docstrings.mjs must
// skip the whole file — covering the directory half of the exclusion, which the
// `namespace detail` fixture does not reach.
#ifndef BOOST_COROSIO_FIXTURE_DETAIL_EXCLUDED_HPP
#define BOOST_COROSIO_FIXTURE_DETAIL_EXCLUDED_HPP

namespace boost::corosio::detail {

/** This whole file is under detail/ and must not reach the linted corpus. */
void excluded_by_directory();

} // namespace boost::corosio::detail

#endif
