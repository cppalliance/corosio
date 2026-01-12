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

/** Type-erased buffer sequence interface.

    This class provides a virtual interface for iterating over
    buffer sequences without knowing the concrete type. It is
    used to fill WSABUF arrays from arbitrary buffer sequences
    without templating the socket implementation.
*/
class buffers_param
{
public:
    virtual ~buffers_param() = default;

    /** Fill an array with buffers from the sequence.

        @param dest Pointer to array of mutable buffer descriptors.
        @param n Maximum number of buffers to copy.

        @return The number of buffers actually copied.
    */
    virtual std::size_t copy_to(capy::mutable_buffer* dest, std::size_t n) = 0;
};

/** Concrete adapter for any buffer sequence.

    This class adapts any buffer sequence type to the
    buffers_param interface. It holds a reference to
    the original sequence and copies buffers on demand.

    @tparam Buffers The buffer sequence type.
*/
template<class Buffers>
class buffers_param_impl final
    : public buffers_param
{
    Buffers const& bufs_;

public:
    /** Construct from a buffer sequence reference.

        @param b The buffer sequence to adapt.
    */
    explicit buffers_param_impl(Buffers const& b) noexcept
        : bufs_(b)
    {
    }

    std::size_t copy_to(capy::mutable_buffer* dest, std::size_t n) override
    {
        std::size_t i = 0;
        auto it = capy::begin(bufs_);
        auto end_it = capy::end(bufs_);
        
        if constexpr (capy::mutable_buffer_sequence<Buffers>)
        {
            for (; it != end_it && i < n; ++it, ++i)
            {
                dest[i] = *it;
            }
        }
        else
        {
            for (; it != end_it && i < n; ++it, ++i)
            {
                auto const& buf = *it;
                dest[i] = capy::mutable_buffer(
                    const_cast<char*>(static_cast<char const*>(buf.data())),
                    buf.size());
            }
        }
        return i;
    }
};

// Deduction guide for CTAD
template<class Buffers>
buffers_param_impl(Buffers const&) -> buffers_param_impl<Buffers>;

} // namespace corosio
} // namespace boost

#endif
