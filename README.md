| Branch | Docs | GitHub Actions | Drone | Coverage (Linux) | Coverage (macOS) | Coverage (Windows) |
|:---|:---|:---|:---|:---|:---|:---|
| [`master`](https://github.com/cppalliance/corosio/tree/master) | [![Documentation](https://img.shields.io/badge/docs-master-brightgreen.svg)](https://master.corosio.cpp.al/) | [![CI](https://github.com/cppalliance/corosio/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/cppalliance/corosio/actions/workflows/ci.yml?query=branch%3Amaster) | [![Build Status](https://drone.cpp.al/api/badges/cppalliance/corosio/status.svg?ref=refs/heads/master)](https://drone.cpp.al/cppalliance/corosio/branches) | [![Lines](https://cppalliance.org/corosio/master/gcovr-linux/badges/coverage-lines.svg)](https://cppalliance.org/corosio/master/gcovr-linux/index.html) | [![Lines](https://cppalliance.org/corosio/master/gcovr-macos/badges/coverage-lines.svg)](https://cppalliance.org/corosio/master/gcovr-macos/index.html) | [![Lines](https://cppalliance.org/corosio/master/gcovr-windows/badges/coverage-lines.svg)](https://cppalliance.org/corosio/master/gcovr-windows/index.html) |
| [`develop`](https://github.com/cppalliance/corosio/tree/develop) | [![Documentation](https://img.shields.io/badge/docs-develop-brightgreen.svg)](https://develop.corosio.cpp.al/) | [![CI](https://github.com/cppalliance/corosio/actions/workflows/ci.yml/badge.svg?branch=develop)](https://github.com/cppalliance/corosio/actions/workflows/ci.yml?query=branch%3Adevelop) | [![Build Status](https://drone.cpp.al/api/badges/cppalliance/corosio/status.svg?ref=refs/heads/develop)](https://drone.cpp.al/cppalliance/corosio/branches) | [![Lines](https://cppalliance.org/corosio/develop/gcovr-linux/badges/coverage-lines.svg)](https://cppalliance.org/corosio/develop/gcovr-linux/index.html) | [![Lines](https://cppalliance.org/corosio/develop/gcovr-macos/badges/coverage-lines.svg)](https://cppalliance.org/corosio/develop/gcovr-macos/index.html) | [![Lines](https://cppalliance.org/corosio/develop/gcovr-windows/badges/coverage-lines.svg)](https://cppalliance.org/corosio/develop/gcovr-windows/index.html) |

# Boost.Corosio

Boost.Corosio is a coroutine-only I/O library for C++20 that provides asynchronous networking primitives with automatic executor affinity propagation. Every operation returns an awaitable that integrates with the _IoAwaitable_ protocol, ensuring your coroutines resume on the correct executor without manual dispatch.

## Quick Start

Clone and build with CMake (dependencies are fetched automatically):

```bash
git clone https://github.com/cppalliance/corosio.git
cd corosio
cmake --preset standalone
cmake --build --preset standalone
```

This downloads Boost 1.90 and Capy automatically. The library is built to `out/standalone/`.

## Requirements

- CMake 3.25 or later
- C++20 compiler (GCC 12+, Clang 17+, MSVC 14.34+)
- Ninja (recommended) or other CMake generator

## License

Distributed under the Boost Software License, Version 1.0.
(See accompanying file [LICENSE_1_0.txt](LICENSE_1_0.txt) or copy at
https://www.boost.org/LICENSE_1_0.txt)
