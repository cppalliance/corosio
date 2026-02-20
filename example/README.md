# Boost.Corosio Examples

This directory contains example programs demonstrating Boost.Corosio usage.

## Examples

### client/

HTTP client examples demonstrating socket operations and coroutine-based I/O.

- **http_client** - Simple command-line HTTP client that sends a GET request

## Building

### CMake

```bash
cmake -B build -DBOOST_COROSIO_BUILD_EXAMPLES=ON
cmake --build build
```

### B2 (BJam)

```bash
b2 example
```
