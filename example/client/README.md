# HTTP Client Example

A simple command-line HTTP client that sends a GET request to an IP address and prints the response.

## Usage

```bash
http_client <ip-address> <port>
```

### Example

```bash
# Connect to example.com (93.184.215.14) on port 80
http_client 93.184.215.14 80
```

## What It Does

1. Parses the IP address and port from command-line arguments
2. Opens a TCP socket and connects to the server
3. Sends a simple HTTP/1.1 GET request
4. Reads and prints the response to stdout

## Building

### CMake

```bash
cmake -B build -DBOOST_COROSIO_BUILD_EXAMPLES=ON
cmake --build build --target corosio_example_client_http
./build/example/client/corosio_example_client_http 93.184.215.14 80
```

### B2 (BJam)

```bash
b2 example/client
./bin/http_client 93.184.215.14 80
```
