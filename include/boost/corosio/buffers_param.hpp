//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_BUFFERS_PARAM_HPP
#define BOOST_COROSIO_BUFFERS_PARAM_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/buffers.hpp>

#include <cstddef>
#include <type_traits>

namespace boost {
namespace corosio {

namespace detail {

template<class Buffers>
constexpr bool is_mutable_buffer_sequence_v = capy::mutable_buffer_sequence<Buffers>;

} // detail

/** Type-erased buffer sequence interface.

    This class provides a virtual interface for iterating over
    buffer sequences without knowing the concrete type. It is
    used to fill WSABUF arrays from arbitrary buffer sequences
    without templating the socket implementation.

    @tparam IsMutable If true, provides mutable buffers (for reading).
                      If false, provides const buffers (for writing).
*/
template<bool IsMutable>
class buffers_param
{
public:
    using buffer_type = std::conditional_t<IsMutable,
        capy::mutable_buffer, capy::const_buffer>;

    virtual ~buffers_param() = default;

    /** Fill an array with buffers from the sequence.

        @param dest Pointer to array of buffer descriptors.
        @param n Maximum number of buffers to copy.

        @return The number of buffers actually copied.
    */
    virtual std::size_t copy_to(buffer_type* dest, std::size_t n) = 0;
};

/** Concrete adapter for any buffer sequence.

    This class adapts any buffer sequence type to the
    buffers_param interface. It holds a reference to
    the original sequence and copies buffers on demand.

    @tparam Buffers The buffer sequence type.
*/
template<class Buffers>
class buffers_param_impl final
    : public buffers_param<detail::is_mutable_buffer_sequence_v<Buffers>>
{
    Buffers const& bufs_;

public:
    using buffer_type = typename buffers_param_impl::buffers_param::buffer_type;

    /** Construct from a buffer sequence reference.

        @param b The buffer sequence to adapt.
    */
    explicit buffers_param_impl(Buffers const& b) noexcept
        : bufs_(b)
    {
    }

    std::size_t copy_to(buffer_type* dest, std::size_t n) override
    {
        (void)n;
        (void)dest;
        std::size_t i = 0;
        return i;
    }
};

// Deduction guide for CTAD
template<class Buffers>
buffers_param_impl(Buffers const&) -> buffers_param_impl<Buffers>;

} // namespace corosio
} // namespace boost

#endif
