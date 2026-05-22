//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

/* Portable temporary path helpers for Unix-domain socket tests.

   Replaces the POSIX-only mkdtemp/unlink/rmdir pattern with
   std::filesystem so tests can run on Windows (which supports
   AF_UNIX since Windows 10 build 17061).

   Paths are kept short — AF_UNIX limits sun_path to ~108 bytes
   on POSIX and ~108 bytes on Windows. The "co_t_" prefix and
   hex random suffix keep the total well under that limit on
   typical Windows installations.
*/

#ifndef BOOST_COROSIO_TEST_LOCAL_TEMP_HPP
#define BOOST_COROSIO_TEST_LOCAL_TEMP_HPP

#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace boost::corosio::test {

inline std::string
make_temp_socket_path(std::string_view prefix = "co_t_")
{
    namespace fs = std::filesystem;

    static std::random_device rd;
    static std::mt19937_64 gen{rd()};

    for (int attempt = 0; attempt < 16; ++attempt)
    {
        std::string name(prefix);
        name += std::to_string(gen());

        auto dir = fs::temp_directory_path() / name;

        std::error_code ec;
        if (fs::create_directory(dir, ec))
            return (dir / "s").string();
    }
    throw std::runtime_error("failed to create temp socket directory");
}

inline void
cleanup_temp_socket(std::string const& path) noexcept
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(path);
    fs::remove(p, ec);
    fs::remove(p.parent_path(), ec);
}

} // namespace boost::corosio::test

#endif
