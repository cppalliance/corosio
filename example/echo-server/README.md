# Echo Server Example

A TCP echo server using a preallocated worker pool. Each worker contains a socket and buffer that persist for the lifetime of the server.

## Usage

```bash
echo_server <port> <max-workers>
```

### Example

```bash
# Start server on port 8080 with 10 workers
echo_server 8080 10
```

## What It Does

1. Parses the port and max-workers from command-line arguments
2. Preallocates a pool of workers, each with its own socket and buffer
3. Listens for incoming TCP connections
4. Accepts connections into free workers
5. Each worker reads data and echoes it back until the client disconnects
6. When a client disconnects, the worker becomes available for a new connection

## Design

- **Preallocated resources**: All workers are created at startup with reserved buffers
- **No dynamic allocation**: During normal operation, no memory allocation occurs
- **Worker reuse**: Sockets are closed and reused for new connections
- **Simple scheduling**: First available worker handles the next connection

## Testing

You can test the echo server using netcat or telnet:

```bash
# In one terminal, start the server
./echo_server 8080 10

# In another terminal, connect with netcat
nc localhost 8080
# Type messages and see them echoed back
```

## Building

### CMake

```bash
cmake -B build -DBOOST_COROSIO_BUILD_EXAMPLES=ON
cmake --build build --target corosio_example_echo_server
./build/example/echo-server/corosio_example_echo_server 8080 10
```

### B2 (BJam)

```bash
b2 example/echo-server
./bin/echo_server 8080 10
```
