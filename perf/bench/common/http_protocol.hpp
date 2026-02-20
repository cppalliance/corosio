//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_BENCH_HTTP_PROTOCOL_HPP
#define BOOST_COROSIO_BENCH_HTTP_PROTOCOL_HPP

#include <cstddef>
#include <cstring>
#include <string_view>

namespace bench::http {

/** Pre-formatted HTTP request. */
constexpr char const small_request[] = "GET /api/data HTTP/1.1\r\n"
                                       "Host: localhost\r\n"
                                       "Content-Length: 0\r\n"
                                       "\r\n";

constexpr std::size_t small_request_size = sizeof(small_request) - 1;

/** Pre-formatted HTTP response. */
constexpr char const small_response[] = "HTTP/1.1 200 OK\r\n"
                                        "Content-Length: 13\r\n"
                                        "\r\n"
                                        "Hello, World!";

constexpr std::size_t small_response_size = sizeof(small_response) - 1;

/** Simple HTTP request parser using a state machine.

    Parses HTTP/1.1 requests by looking for the \r\n\r\n sequence
    that terminates the headers. Content-Length handling is simplified
    since the benchmark uses zero-length bodies.
*/
class request_parser
{
    std::size_t header_end_ = 0;
    bool complete_          = false;

public:
    /** Reset the parser for a new request. */
    void reset()
    {
        header_end_ = 0;
        complete_   = false;
    }

    /** Return true if a complete request has been parsed. */
    bool complete() const
    {
        return complete_;
    }

    /** Return the number of bytes consumed by the complete request.

        Only valid when complete() returns true.
    */
    std::size_t bytes_consumed() const
    {
        return header_end_;
    }

    /** Parse incoming data and return number of bytes consumed.

        @param data Pointer to the buffer containing request data.
        @param size Number of bytes available in the buffer.
        @return Number of bytes consumed (0 if incomplete).
    */
    std::size_t parse(char const* data, std::size_t size)
    {
        if (complete_)
            return 0;

        // Search for \r\n\r\n sequence
        for (std::size_t i = 0; i + 3 < size; ++i)
        {
            if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' &&
                data[i + 3] == '\n')
            {
                header_end_ = i + 4;
                complete_   = true;
                return header_end_;
            }
        }
        return 0;
    }
};

/** Simple HTTP response parser.

    Parses HTTP/1.1 responses by finding the header end and extracting
    Content-Length to determine the total message size.
*/
class response_parser
{
    std::size_t header_end_     = 0;
    std::size_t content_length_ = 0;
    std::size_t total_size_     = 0;
    bool complete_              = false;

public:
    /** Reset the parser for a new response. */
    void reset()
    {
        header_end_     = 0;
        content_length_ = 0;
        total_size_     = 0;
        complete_       = false;
    }

    /** Return true if a complete response has been parsed. */
    bool complete() const
    {
        return complete_;
    }

    /** Return the total size of the complete response.

        Only valid when complete() returns true.
    */
    std::size_t total_size() const
    {
        return total_size_;
    }

    /** Parse incoming data and check for completion.

        @param data Pointer to the buffer containing response data.
        @param size Number of bytes available in the buffer.
        @return True if the response is complete.
    */
    bool parse(char const* data, std::size_t size)
    {
        if (complete_)
            return true;

        // Find header end if not yet found
        if (header_end_ == 0)
        {
            for (std::size_t i = 0; i + 3 < size; ++i)
            {
                if (data[i] == '\r' && data[i + 1] == '\n' &&
                    data[i + 2] == '\r' && data[i + 3] == '\n')
                {
                    header_end_ = i + 4;

                    // Extract Content-Length
                    std::string_view headers(data, header_end_);
                    auto pos = headers.find("Content-Length: ");
                    if (pos != std::string_view::npos)
                    {
                        pos += 16;
                        content_length_ = 0;
                        while (pos < headers.size() && headers[pos] >= '0' &&
                               headers[pos] <= '9')
                        {
                            content_length_ =
                                content_length_ * 10 + (headers[pos] - '0');
                            ++pos;
                        }
                    }
                    total_size_ = header_end_ + content_length_;
                    break;
                }
            }
        }

        // Check if we have the complete response
        if (header_end_ > 0 && size >= total_size_)
        {
            complete_ = true;
            return true;
        }

        return false;
    }
};

} // namespace bench::http

#endif
