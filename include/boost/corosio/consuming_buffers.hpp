//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_CONSUMING_BUFFERS_HPP
#define BOOST_COROSIO_CONSUMING_BUFFERS_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/buffers.hpp>

#include <cstddef>
#include <iterator>
#include <ranges>
#include <type_traits>

namespace boost {
namespace corosio {

/** Wrapper for consuming a buffer sequence incrementally.

    This class wraps a mutable buffer sequence and tracks the
    current read position. It provides a `consume(n)` function
    that advances through the sequence as bytes are read.

    @tparam MutableBufferSequence The buffer sequence type.
*/
template<capy::mutable_buffer_sequence MutableBufferSequence>
class consuming_buffers
{
    using iterator_type = decltype(capy::begin(std::declval<MutableBufferSequence const&>()));
    using end_iterator_type = decltype(capy::end(std::declval<MutableBufferSequence const&>()));

    MutableBufferSequence const& bufs_;
    iterator_type it_;
    end_iterator_type end_;
    std::size_t consumed_ = 0;  // Bytes consumed in current buffer

public:
    /** Construct from a buffer sequence.

        @param bufs The buffer sequence to wrap.
    */
    explicit consuming_buffers(MutableBufferSequence const& bufs) noexcept
        : bufs_(bufs)
        , it_(capy::begin(bufs))
        , end_(capy::end(bufs))
    {
    }

    /** Consume n bytes from the buffer sequence.

        Advances the current position by n bytes, moving to the
        next buffer when the current one is exhausted.

        @param n The number of bytes to consume.
    */
    void consume(std::size_t n) noexcept
    {
        while (n > 0 && it_ != end_)
        {
            auto const& buf = *it_;
            std::size_t const buf_size = buf.size();
            std::size_t const remaining = buf_size - consumed_;

            if (n < remaining)
            {
                // Consume part of current buffer
                consumed_ += n;
                n = 0;
            }
            else
            {
                // Consume rest of current buffer and move to next
                n -= remaining;
                consumed_ = 0;
                ++it_;
            }
        }
    }

    /** Iterator for the consuming buffer sequence.

        Returns buffers starting from the current position,
        with the first buffer adjusted for consumed bytes.
    */
    class const_iterator
    {
        iterator_type it_;
        end_iterator_type end_;
        std::size_t consumed_;

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = capy::mutable_buffer;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type;

        const_iterator(
            iterator_type it,
            end_iterator_type end,
            std::size_t consumed) noexcept
            : it_(it)
            , end_(end)
            , consumed_(consumed)
        {
        }

        bool operator==(const_iterator const& other) const noexcept
        {
            return it_ == other.it_ && consumed_ == other.consumed_;
        }

        value_type operator*() const noexcept
        {
            auto const& buf = *it_;
            return capy::mutable_buffer(
                static_cast<char*>(buf.data()) + consumed_,
                buf.size() - consumed_);
        }

        const_iterator& operator++() noexcept
        {
            consumed_ = 0;
            ++it_;
            return *this;
        }

        const_iterator operator++(int) noexcept
        {
            const_iterator tmp = *this;
            ++*this;
            return tmp;
        }

        const_iterator& operator--() noexcept
        {
            if (consumed_ == 0)
            {
                --it_;
                // Set consumed_ to the size of the previous buffer
                // This is a simplified implementation for bidirectional requirement
                if (it_ != end_)
                {
                    auto const& buf = *it_;
                    consumed_ = buf.size();
                }
            }
            else
            {
                consumed_ = 0;
            }
            return *this;
        }

        const_iterator operator--(int) noexcept
        {
            const_iterator tmp = *this;
            --*this;
            return tmp;
        }
    };

    /** Return iterator to beginning of remaining buffers.

        @return Iterator pointing to the first remaining buffer,
            adjusted for consumed bytes in the current buffer.
    */
    const_iterator begin() const noexcept
    {
        return const_iterator(it_, end_, consumed_);
    }

    /** Return iterator to end of buffer sequence.

        @return End iterator.
    */
    const_iterator end() const noexcept
    {
        return const_iterator(end_, end_, 0);
    }
};

// ADL helpers for capy::begin and capy::end
template<capy::mutable_buffer_sequence MutableBufferSequence>
auto begin(consuming_buffers<MutableBufferSequence> const& cb) noexcept
{
    return cb.begin();
}

template<capy::mutable_buffer_sequence MutableBufferSequence>
auto end(consuming_buffers<MutableBufferSequence> const& cb) noexcept
{
    return cb.end();
}

} // namespace corosio
} // namespace boost

#endif
