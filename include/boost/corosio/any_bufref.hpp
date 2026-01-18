//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_ANY_BUFREF_HPP
#define BOOST_COROSIO_ANY_BUFREF_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/buffers.hpp>

#include <cstddef>

namespace boost {
namespace corosio {

/** A type-erased buffer sequence I/O parameter.

    This class provides a type-erased interface for iterating
    over buffer sequences without knowing the concrete type.
    It allows asynchronous operations to efficiently type-erase
    the buffer sequence parameter, avoiding the need to
    templatize the implementation.

    @par Example
    The following shows the minimal form of an awaitable, templated on the
    buffer sequence type, with only an `await_suspend` method. The example
    demonstrates that you can construct an `any_bufref` in the parameter
    list when calling a virtual interface; there is no need to create a
    separate variable if not desired.

    @code
    template<class Buffers>
    struct awaitable
    {
        Buffers b;

        void await_suspend(std::coroutine_handle<>)
        {
            my_virtual_engine_submit(any_bufref(b));
        }
    };

    // Example virtual interface accepting any_bufref
    void my_virtual_engine_submit(any_bufref p)
    {
        capy::mutable_buffer temp[8];
        std::size_t n = p.copy_to(temp, 8);
        // ... handle the buffers ...
    }
    @endcode
*/
class any_bufref
{
public:
    /** Construct from a const buffer sequence.

        @param bs The buffer sequence to adapt.
    */
    template<capy::ConstBufferSequence BufferSequence>
    explicit
    any_bufref(BufferSequence const& bs) noexcept
        : bs_(&bs)
        , fn_(&copy_impl<BufferSequence>)
    {
    }

    /** Fill an array with buffers from the sequence.

        @param dest Pointer to array of mutable buffer descriptors.
        @param n Maximum number of buffers to copy.

        @return The number of buffers actually copied.
    */
    std::size_t
    copy_to(
        capy::mutable_buffer* dest,
        std::size_t n) const noexcept
    {
        return fn_(bs_, dest, n);
    }

private:
    template<capy::ConstBufferSequence BufferSequence>
    static std::size_t
    copy_impl(
        void const* p,
        capy::mutable_buffer* dest,
        std::size_t n)
    {
        auto const& bs = *static_cast<BufferSequence const*>(p);
        auto it = capy::begin(bs);
        auto const end_it = capy::end(bs);

        std::size_t i = 0;
        if constexpr (capy::MutableBufferSequence<BufferSequence>)
        {
            for (; it != end_it && i < n; ++it, ++i)
                dest[i] = *it;
        }
        else
        {
            for (; it != end_it && i < n; ++it, ++i)
            {
                auto const& buf = *it;
                dest[i] = capy::mutable_buffer(
                    const_cast<char*>(
                        static_cast<char const*>(buf.data())),
                    buf.size());
            }
        }
        return i;
    }

    using fn_t = std::size_t(*)(void const*,
        capy::mutable_buffer*, std::size_t);

    void const* bs_;
    fn_t fn_;
};

} // namespace corosio
} // namespace boost

#endif
