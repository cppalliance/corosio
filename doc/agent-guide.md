---
Boost.Corosio specific instructions
---

* Research:
  @research/tcp-ip-illustrated.txt
https://start-concurrent.github.io/full/index.html

* intro.adoc
  - Requirements: Familiarity with Boost.Capy and coroutines

# Introduction to TCP/IP Networking

## 1. What is a Network?
- Computers talking to each other
- Local vs remote communication
- The need for protocols

## 2. Physical Foundation
- Signals on wires (and wireless)
- NICs and MAC addresses
- Ethernet basics
- Hubs, switches, routers

## 3. Network Models
- Why layered architecture?
- OSI 7-layer model (overview)
- TCP/IP 4-layer model
- Encapsulation and decapsulation

## 4. Internet Protocol (IP)
- IP addresses (IPv4, IPv6)
- Subnets and CIDR notation
- Public vs private addresses
- NAT
- Packets and fragmentation
- Routing basics
- TTL and hop counts

## 5. TCP: Reliable Streams
- Connection-oriented communication
- Three-way handshake (SYN, SYN-ACK, ACK)
- Sequence numbers and acknowledgments
- Flow control (sliding window)
- Congestion control
- Retransmission
- Connection teardown (FIN, RST)
- TCP states (ESTABLISHED, TIME_WAIT, etc.)

## 6. UDP: Unreliable Datagrams
- Connectionless communication
- When to use UDP vs TCP
- Checksums

## 7. Ports and Sockets
- Port numbers (well-known, ephemeral)
- Socket as (IP, port) pair
- Listening vs connected sockets
- Socket API primitives (bind, listen, accept, connect)

## 8. DNS
- Hostname resolution
- A, AAAA, CNAME records
- DNS lookup flow

## 9. Practical Considerations
- Localhost and loopback
- Firewalls and port blocking
- Keep-alive
- Nagle's algorithm
- TCP_NODELAY
- SO_REUSEADDR / SO_REUSEPORT

# Introduction to Concurrent Programming

## 1. Why Concurrency?
- Sequential vs concurrent execution
- Latency hiding (do useful work while waiting)
- Throughput (handle many clients simultaneously)
- Concurrency vs parallelism

## 2. The Problem of Shared State
- Race conditions
- The "read-modify-write" hazard
- Why even correct-looking code can fail

## 3. Traditional Solutions
- Threads and their costs (memory, context switches)
- Mutexes and critical sections
- Deadlock basics
- Why mutexes are error-prone

## 4. The Event Loop Model
- Single-threaded concurrency
- Non-blocking I/O
- Run-to-completion semantics
- Reactor pattern

## 5. C++20 Coroutines
- Language mechanics (`co_await`, `co_return`)
- Suspension points as yield points
- Coroutines vs threads (cost, scheduling)
- Why coroutines excel for I/O

## 6. Executor Affinity
- What affinity means
- Resuming through the right executor
- The affine awaitable protocol

## 7. Strands: Synchronization Without Locks
- Sequential execution guarantees
- Implicit vs explicit synchronization
- When strands replace mutexes

## 8. Scaling Strategies
- Single-threaded: one thread, many coroutines
- Multi-threaded: thread pools
- Choosing the right model

## 9. Patterns
- One coroutine per connection
- Worker pools (bounded concurrency)
- Pipelines (multi-stage processing)

## 10. Common Mistakes
- Blocking in coroutines
- Dangling references in async code
- Cross-executor access
