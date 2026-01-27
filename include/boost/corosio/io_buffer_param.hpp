//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_BUFFER_PARAM_HPP
#define BOOST_COROSIO_IO_BUFFER_PARAM_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/buffers.hpp>

#include <cstddef>

namespace boost::corosio {

/** A type-erased buffer sequence for I/O system call boundaries.

    This class enables I/O objects to accept any buffer sequence type
    across a virtual function boundary, while preserving the caller's
    typed buffer sequence at the call site. The implementation can
    then unroll the type-erased sequence into platform-native
    structures (e.g., `iovec` on POSIX, `WSABUF` on Windows) for the
    actual system call.

    @par Purpose

    When building coroutine-based I/O abstractions, a common pattern
    emerges: a templated awaitable captures the caller's buffer
    sequence, and at `await_suspend` time, must pass it across a
    virtual interface to the I/O implementation. This class solves
    the type-erasure problem at that boundary without heap allocation.

    @par Restricted Use Case

    This is NOT a general-purpose composable abstraction. It exists
    solely for the final step in a coroutine I/O call chain where:

    @li A templated awaitable captures the caller's buffer sequence
    @li The awaitable's `await_suspend` passes buffers across a
        virtual interface to an I/O object implementation
    @li The implementation immediately unrolls the buffers into
        platform-native structures for the system call

    @par Lifetime Model

    The safety of this class depends entirely on coroutine parameter
    lifetime extension. When a coroutine is suspended, parameters
    passed to the awaitable remain valid until the coroutine resumes
    or is destroyed. This class exploits that guarantee by holding
    only a pointer to the caller's buffer sequence.

    The referenced buffer sequence is valid ONLY while the calling
    coroutine remains suspended at the exact suspension point where
    `io_buffer_param` was created. Once the coroutine resumes,
    returns, or is destroyed, all referenced data becomes invalid.

    @par Const Buffer Handling

    This class accepts both `ConstBufferSequence` and
    `MutableBufferSequence` types. However, `copy_to` always produces
    `mutable_buffer` descriptors, casting away constness for const
    buffer sequences. This design matches platform I/O structures
    (`iovec`, `WSABUF`) which use non-const pointers regardless of
    the operation direction.

    @warning The caller is responsible for ensuring the type system
    is not violated. When the original buffer sequence was const
    (e.g., for a write operation), the implementation MUST NOT write
    to the buffers obtained from `copy_to`. The const-cast exists
    solely to provide a uniform interface for platform I/O calls.

    @code
    // For write operations (const buffers):
    void submit_write(io_buffer_param p)
    {
        capy::mutable_buffer bufs[8];
        auto n = p.copy_to(bufs, 8);
        // bufs[] may reference const data - DO NOT WRITE
        writev(fd, reinterpret_cast<iovec*>(bufs), n);  // OK: read-only
    }

    // For read operations (mutable buffers):
    void submit_read(io_buffer_param p)
    {
        capy::mutable_buffer bufs[8];
        auto n = p.copy_to(bufs, 8);
        // bufs[] references mutable data - safe to write
        readv(fd, reinterpret_cast<iovec*>(bufs), n);  // OK: writing
    }
    @endcode

    @par Correct Usage

    The implementation receiving `io_buffer_param` MUST:

    @li Call `copy_to` immediately upon receiving the parameter
    @li Use the unrolled buffer descriptors for the I/O operation
    @li Never store the `io_buffer_param` object itself
    @li Never store pointers obtained from `copy_to` beyond the
        immediate I/O operation

    @par Example: Correct Usage

    @code
    // Templated awaitable at the call site
    template<class Buffers>
    struct write_awaitable
    {
        Buffers bufs;
        io_stream* stream;

        bool await_ready() { return false; }

        void await_suspend(std::coroutine_handle<> h)
        {
            // CORRECT: Pass to virtual interface while suspended.
            // The buffer sequence 'bufs' remains valid because
            // coroutine parameters live until resumption.
            stream->async_write_some_impl(bufs, h);
        }

        io_result await_resume() { return stream->get_result(); }
    };

    // Virtual implementation - unrolls immediately
    void stream_impl::async_write_some_impl(
        io_buffer_param p,
        std::coroutine_handle<> h)
    {
        // CORRECT: Unroll immediately into platform structure
        iovec vecs[16];
        std::size_t n = p.copy_to(
            reinterpret_cast<capy::mutable_buffer*>(vecs), 16);

        // CORRECT: Use unrolled buffers for system call now
        submit_to_io_uring(vecs, n, h);

        // After this function returns, 'p' must not be used again.
        // The iovec array is safe because it contains copies of
        // the pointer/size pairs, not references to 'p'.
    }
    @endcode

    @par UNSAFE USAGE: Storing io_buffer_param

    @warning Never store `io_buffer_param` for later use.

    @code
    class broken_stream
    {
        io_buffer_param saved_param_;  // UNSAFE: member storage

        void async_write_impl(io_buffer_param p, ...)
        {
            saved_param_ = p;  // UNSAFE: storing for later
            schedule_write_later();
        }

        void do_write_later()
        {
            // UNSAFE: The calling coroutine may have resumed
            // or been destroyed. saved_param_ now references
            // invalid memory!
            capy::mutable_buffer bufs[8];
            saved_param_.copy_to(bufs, 8);  // UNDEFINED BEHAVIOR
        }
    };
    @endcode

    @par UNSAFE USAGE: Storing Unrolled Pointers

    @warning The pointers obtained from `copy_to` point into the
    caller's buffer sequence. They become invalid when the caller
    resumes.

    @code
    class broken_stream
    {
        capy::mutable_buffer saved_bufs_[8];  // UNSAFE
        std::size_t saved_count_;

        void async_write_impl(io_buffer_param p, ...)
        {
            // This copies pointer/size pairs into saved_bufs_
            saved_count_ = p.copy_to(saved_bufs_, 8);

            // UNSAFE: scheduling for later while storing the
            // buffer descriptors. The pointers in saved_bufs_
            // will dangle when the caller resumes!
            schedule_for_later();
        }

        void later()
        {
            // UNSAFE: saved_bufs_ contains dangling pointers
            for(std::size_t i = 0; i < saved_count_; ++i)
                write(fd_, saved_bufs_[i].data(), ...);  // UB
        }
    };
    @endcode

    @par UNSAFE USAGE: Using Outside a Coroutine

    @warning This class relies on coroutine lifetime semantics.
    Using it with callbacks or non-coroutine async patterns is
    undefined behavior.

    @code
    // UNSAFE: No coroutine lifetime guarantee
    void bad_callback_pattern(std::vector<char>& data)
    {
        capy::mutable_buffer buf(data.data(), data.size());

        // UNSAFE: In a callback model, 'buf' may go out of scope
        // before the callback fires. There is no coroutine
        // suspension to extend the lifetime.
        stream.async_write(buf, [](error_code ec) {
            // 'buf' is already destroyed!
        });
    }
    @endcode

    @par UNSAFE USAGE: Passing to Another Coroutine

    @warning Do not pass `io_buffer_param` to a different coroutine
    or spawn a new coroutine that captures it.

    @code
    void broken_impl(io_buffer_param p, std::coroutine_handle<> h)
    {
        // UNSAFE: Spawning a new coroutine that captures 'p'.
        // The original coroutine may resume before this new
        // coroutine uses 'p'.
        co_spawn([p]() -> task<void> {
            capy::mutable_buffer bufs[8];
            p.copy_to(bufs, 8);  // UNSAFE: original caller may
                                 // have resumed already!
            co_return;
        });
    }
    @endcode

    @par UNSAFE USAGE: Multiple Virtual Hops

    @warning Minimize indirection. Each virtual call that passes
    `io_buffer_param` without immediately unrolling it increases
    the risk of misuse.

    @code
    // Risky: multiple hops before unrolling
    void layer1(io_buffer_param p) {
        layer2(p);  // Still haven't unrolled...
    }
    void layer2(io_buffer_param p) {
        layer3(p);  // Still haven't unrolled...
    }
    void layer3(io_buffer_param p) {
        // Finally unrolling, but the chain is fragile.
        // Any intermediate layer storing 'p' breaks everything.
    }
    @endcode

    @par UNSAFE USAGE: Fire-and-Forget Operations

    @warning Do not use with detached or fire-and-forget async
    operations where there is no guarantee the caller remains
    suspended.

    @code
    task<void> caller()
    {
        char buf[1024];
        // UNSAFE: If async_write is fire-and-forget (doesn't
        // actually suspend the caller), 'buf' may be destroyed
        // before the I/O completes.
        stream.async_write_detached(capy::mutable_buffer(buf, 1024));
        // Returns immediately - 'buf' goes out of scope!
    }
    @endcode

    @par Passing Convention

    Pass by value. The class contains only two pointers (16 bytes
    on 64-bit systems), making copies trivial and clearly
    communicating the lightweight, transient nature of this type.

    @code
    // Preferred: pass by value
    void process(io_buffer_param buffers);

    // Also acceptable: pass by const reference
    void process(io_buffer_param const& buffers);
    @endcode

    @see capy::ConstBufferSequence, capy::MutableBufferSequence
*/
class io_buffer_param
{
public:
    /** Construct from a const buffer sequence.

        @param bs The buffer sequence to adapt.
    */
    template<capy::ConstBufferSequence BS>
    io_buffer_param(BS const& bs) noexcept
        : bs_(&bs)
        , fn_(&copy_impl<BS>)
    {
    }

    /** Fill an array with buffers from the sequence.

        Copies buffer descriptors from the sequence into the
        destination array, skipping any zero-size buffers.
        This ensures the output contains only buffers with
        actual data, suitable for direct use with system calls.

        @param dest Pointer to array of mutable buffer descriptors.
        @param n Maximum number of buffers to copy.

        @return The number of non-zero buffers copied.
    */
    std::size_t
    copy_to(
        capy::mutable_buffer* dest,
        std::size_t n) const noexcept
    {
        return fn_(bs_, dest, n);
    }

private:
    template<capy::ConstBufferSequence BS>
    static std::size_t
    copy_impl(
        void const* p,
        capy::mutable_buffer* dest,
        std::size_t n)
    {
        auto const& bs = *static_cast<BS const*>(p);
        auto it = capy::begin(bs);
        auto const end_it = capy::end(bs);

        std::size_t i = 0;
        if constexpr (capy::MutableBufferSequence<BS>)
        {
            for(; it != end_it && i < n; ++it)
            {
                capy::mutable_buffer buf(*it);
                if(buf.size() == 0)
                    continue;
                dest[i++] = buf;
            }
        }
        else
        {
            for(; it != end_it && i < n; ++it)
            {
                capy::const_buffer buf(*it);
                if(buf.size() == 0)
                    continue;
                dest[i++] = capy::mutable_buffer(
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

} // namespace boost::corosio

#endif
