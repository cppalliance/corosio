//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_TLS_TEST_UTILS_HPP
#define BOOST_COROSIO_TEST_TLS_TEST_UTILS_HPP

#include <boost/corosio/delay.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/io/io_stream.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/corosio/tls_stream.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include "test_suite.hpp"

#include <chrono>
#include <tuple>
#include <stop_token>
#include <type_traits>
#include <vector>

#if BOOST_COROSIO_POSIX
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <boost/corosio/native/detail/iocp/win_windows.hpp>
#include <ws2tcpip.h>
#endif

// Valgrind slows execution ~10-20x; scale failsafe timeouts to avoid
// false failures when BOOST_NO_STRESS_TEST is defined.
#ifdef BOOST_NO_STRESS_TEST
inline constexpr int failsafe_scale = 20;
#else
inline constexpr int failsafe_scale = 1;
#endif

namespace boost::corosio::test {

/// Fail the current test if a setup call reports an error.
inline void
require_ok(std::error_code ec)
{
    BOOST_TEST(!ec);
}

//
// Raw native sockets for the assign()/release() adoption tests
//

#if BOOST_COROSIO_HAS_IOCP
inline constexpr native_handle_type invalid_native_socket =
    static_cast<native_handle_type>(~0ull);
#else
inline constexpr native_handle_type invalid_native_socket =
    static_cast<native_handle_type>(-1);
#endif

/** Create a socket the way an adopting caller would: outside the
    library, owned by the caller until assign() succeeds.

    @param family Address family.
    @param type Socket type.
    @return The new descriptor, or @ref invalid_native_socket.
*/
inline native_handle_type
make_native_socket(int family, int type)
{
#if BOOST_COROSIO_HAS_IOCP
    return static_cast<native_handle_type>(::WSASocketW(
        family, type, 0, nullptr, 0, WSA_FLAG_OVERLAPPED));
#else
    return static_cast<native_handle_type>(::socket(family, type, 0));
#endif
}

/** Put a descriptor in the mode the backend needs before adoption.

    Adoption never touches descriptor flags, so the caller must hand
    in a socket that is already configured.

    @param h The descriptor to configure.
*/
inline void
make_native_adoptable([[maybe_unused]] native_handle_type h)
{
#if BOOST_COROSIO_HAS_IOCP
    // WSA_FLAG_OVERLAPPED is set at creation.
#else
    int fd    = static_cast<int>(h);
    int flags = ::fcntl(fd, F_GETFL);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

/// Close a descriptor the library does not own.
inline void
close_native_socket(native_handle_type h)
{
#if BOOST_COROSIO_HAS_IOCP
    ::closesocket(static_cast<SOCKET>(h));
#else
    ::close(static_cast<int>(h));
#endif
}

/** Check whether a descriptor is still open.

    A rejected assign must leave the descriptor with the caller.

    @param h The descriptor to probe.
    @return True while the descriptor is still open.
*/
inline bool
native_socket_valid(native_handle_type h)
{
#if BOOST_COROSIO_HAS_IOCP
    int type = 0;
    int len  = static_cast<int>(sizeof(type));
    return ::getsockopt(
               static_cast<SOCKET>(h), SOL_SOCKET, SO_TYPE,
               reinterpret_cast<char*>(&type), &len) == 0;
#else
    return ::fcntl(static_cast<int>(h), F_GETFD) >= 0;
#endif
}

//
// Embedded Test Certificates
//

// Self-signed server certificate from Boost.Beast
// Subject: C=US, ST=CA, L=Los Angeles, O=Beast, CN=www.example.com
// Valid: 2021-07-06 to 2048-11-21 (self-signed, CA:TRUE)
// Command:
//   openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 10000 -nodes
//       -subj "/C=US/ST=CA/L=Los Angeles/O=Beast/CN=www.example.com"
inline constexpr char const* server_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDlTCCAn2gAwIBAgIUOLxr3q7Wd/pto1+2MsW4fdRheCIwDQYJKoZIhvcNAQEL\n"
    "BQAwWjELMAkGA1UEBhMCVVMxCzAJBgNVBAgMAkNBMRQwEgYDVQQHDAtMb3MgQW5n\n"
    "ZWxlczEOMAwGA1UECgwFQmVhc3QxGDAWBgNVBAMMD3d3dy5leGFtcGxlLmNvbTAe\n"
    "Fw0yMTA3MDYwMTQ5MjVaFw00ODExMjEwMTQ5MjVaMFoxCzAJBgNVBAYTAlVTMQsw\n"
    "CQYDVQQIDAJDQTEUMBIGA1UEBwwLTG9zIEFuZ2VsZXMxDjAMBgNVBAoMBUJlYXN0\n"
    "MRgwFgYDVQQDDA93d3cuZXhhbXBsZS5jb20wggEiMA0GCSqGSIb3DQEBAQUAA4IB\n"
    "DwAwggEKAoIBAQCz0GwgnxSBhygxBdhTHGx5LDLIJSuIDJ6nMwZFvAjdhLnB/vOT\n"
    "Lppr5MKxqQHEpYdyDYGD1noBoz4TiIRj5JapChMgx58NLq5QyXkHV/ONT7yi8x05\n"
    "P41c2F9pBEnUwUxIUG1Cb6AN0cZWF/wSMOZ0w3DoBhnl1sdQfQiS25MTK6x4tATm\n"
    "Wm9SJc2lsjWptbyIN6hFXLYPXTwnYzCLvv1EK6Ft7tMPc/FcJpd/wYHgl8shDmY7\n"
    "rV+AiGTxUU35V0AzpJlmvct5aJV/5vSRRLwT9qLZSddE9zy/0rovC5GML6S7BUC4\n"
    "lIzJ8yxzOzSStBPxvdrOobSSNlRZIlE7gnyNAgMBAAGjUzBRMB0GA1UdDgQWBBR+\n"
    "dYtY9zmFSw9GYpEXC1iJKHC0/jAfBgNVHSMEGDAWgBR+dYtY9zmFSw9GYpEXC1iJ\n"
    "KHC0/jAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQBzKrsiYywl\n"
    "RKeB2LbddgSf7ahiQMXCZpAjZeJikIoEmx+AmjQk1bam+M7WfpRAMnCKooU+Utp5\n"
    "TwtijjnJydkZHFR6UH6oCWm8RsUVxruao/B0UFRlD8q+ZxGd4fGTdLg/ztmA+9oC\n"
    "EmrcQNdz/KIxJj/fRB3j9GM4lkdaIju47V998Z619E/6pt7GWcAySm1faPB0X4fL\n"
    "FJ6iYR2r/kJLoppPqL0EE49uwyYQ1dKhXS2hk+IIfA9mBn8eAFb/0435A2fXutds\n"
    "qhvwIOmAObCzcoKkz3sChbk4ToUTqbC0TmFAXI5Upz1wnADzjpbJrpegCA3pmvhT\n"
    "7356drqnCGY9\n"
    "-----END CERTIFICATE-----\n";

// CA cert is the same as server cert (self-signed)
inline constexpr char const* ca_cert_pem = server_cert_pem;

// Server private key from Boost.Beast (RSA 2048-bit)
// Matches server_cert_pem above
inline constexpr char const* server_key_pem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQCz0GwgnxSBhygx\n"
    "BdhTHGx5LDLIJSuIDJ6nMwZFvAjdhLnB/vOTLppr5MKxqQHEpYdyDYGD1noBoz4T\n"
    "iIRj5JapChMgx58NLq5QyXkHV/ONT7yi8x05P41c2F9pBEnUwUxIUG1Cb6AN0cZW\n"
    "F/wSMOZ0w3DoBhnl1sdQfQiS25MTK6x4tATmWm9SJc2lsjWptbyIN6hFXLYPXTwn\n"
    "YzCLvv1EK6Ft7tMPc/FcJpd/wYHgl8shDmY7rV+AiGTxUU35V0AzpJlmvct5aJV/\n"
    "5vSRRLwT9qLZSddE9zy/0rovC5GML6S7BUC4lIzJ8yxzOzSStBPxvdrOobSSNlRZ\n"
    "IlE7gnyNAgMBAAECggEAY0RorQmldGx9D7M+XYOPjsWLs1px0cXFwGA20kCgVEp1\n"
    "kleBeHt93JqJsTKwOzN2tswl9/ZrnIPWPUpcbBlB40ggjzQk5k4jBY50Nk2jsxuV\n"
    "9A9qzrP7AoqhAYTQjZe42SMtbkPZhEeOyvCqxBAi6csLhcv4eB4+In0kQo7dfvLs\n"
    "Xu/3WhSsuAWqdD9EGnhD3n+hVTtgiasRe9318/3R9DzP+IokoQGOtXm+1dsfP0mV\n"
    "8XGzQHBpUtJNn0yi6SC4kGEQuKkX33zORlSnZgT5VBLofNgra0THd7x3atOx1lbr\n"
    "V0QizvCdBa6j6FwhOQwW8UwgOCnUbWXl/Xn4OaofMQKBgQDdRXSMyys7qUMe4SYM\n"
    "Mdawj+rjv0Hg98/xORuXKEISh2snJGKEwV7L0vCn468n+sM19z62Axz+lvOUH8Qr\n"
    "hLkBNqJvtIP+b0ljRjem78K4a4qIqUlpejpRLw6a/+44L76pMJXrYg3zdBfwzfwu\n"
    "b9NXdwHzWoNuj4v36teGP6xOUwKBgQDQCT52XX96NseNC6HeK5BgWYYjjxmhksHi\n"
    "stjzPJKySWXZqJpHfXI8qpOd0Sd1FHB+q1s3hand9c+Rxs762OXlqA9Q4i+4qEYZ\n"
    "qhyRkTsl+2BhgzxmoqGd5gsVT7KV8XqtuHWLmetNEi+7+mGSFf2iNFnonKlvT1JX\n"
    "4OQZC7ntnwKBgH/ORFmmaFxXkfteFLnqd5UYK5ZMvGKTALrWP4d5q2BEc7HyJC2F\n"
    "+5lDR9nRezRedS7QlppPBgpPanXeO1LfoHSA+CYJYEwwP3Vl83Mq/Y/EHgp9rXeN\n"
    "L+4AfjEtLo2pljjnZVDGHETIg6OFdunjkXDtvmSvnUbZBwG11bMnSAEdAoGBAKFw\n"
    "qwJb6FNFM3JnNoQctnuuvYPWxwM1yjRMqkOIHCczAlD4oFEeLoqZrNhpuP8Ij4wd\n"
    "GjpqBbpzyVLNP043B6FC3C/edz4Lh+resjDczVPaUZ8aosLbLiREoxE0udfWf2dU\n"
    "oBNnrMwwcs6jrRga7Kr1iVgUSwBQRAxiP2CYUv7tAoGBAKdPdekPNP/rCnHkKIkj\n"
    "o13pr+LJ8t+15vVzZNHwPHUWiYXFhG8Ivx7rqLQSPGcuPhNss3bg1RJiZAUvF6fd\n"
    "e6QS4EZM9dhhlO2FmPQCJMrRVDXaV+9TcJZXCbclQnzzBus9pwZZyw4Anxo0vmir\n"
    "nOMOU6XI4lO9Xge/QDEN4Y2R\n"
    "-----END PRIVATE KEY-----\n";

// Self-signed server certificate with an iPAddress SAN only.
// The CN is deliberately not an IP so hostname checks cannot
// succeed via CN fallback; only the IP-SAN path matches.
// Subject: C=US, ST=CA, L=Los Angeles, O=Corosio, CN=corosio-ip-test
// SAN: IP:127.0.0.1
// Command:
//   openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem
//       -days 10000 -nodes
//       -subj "/C=US/ST=CA/L=Los Angeles/O=Corosio/CN=corosio-ip-test"
//       -addext "subjectAltName=IP:127.0.0.1"
inline constexpr char const* server_ip_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDrDCCApSgAwIBAgIUL29ePiWtHFp225p0PpXCfX3Uic4wDQYJKoZIhvcNAQEL\n"
    "BQAwXDELMAkGA1UEBhMCVVMxCzAJBgNVBAgMAkNBMRQwEgYDVQQHDAtMb3MgQW5n\n"
    "ZWxlczEQMA4GA1UECgwHQ29yb3NpbzEYMBYGA1UEAwwPY29yb3Npby1pcC10ZXN0\n"
    "MCAXDTI2MDcyMTE1NTQxNVoYDzIwNTMxMjA2MTU1NDE1WjBcMQswCQYDVQQGEwJV\n"
    "UzELMAkGA1UECAwCQ0ExFDASBgNVBAcMC0xvcyBBbmdlbGVzMRAwDgYDVQQKDAdD\n"
    "b3Jvc2lvMRgwFgYDVQQDDA9jb3Jvc2lvLWlwLXRlc3QwggEiMA0GCSqGSIb3DQEB\n"
    "AQUAA4IBDwAwggEKAoIBAQDTOzWcoMa44QpwSpJCqqZysMe1oUVRpocaRiTd436u\n"
    "E79i9n3zBAJKrR0LL0J2TjcZcn6lcyyiAfdpmEgqht6KSIb7YzTuC3fsKi8b1bfk\n"
    "AGPpw9cQrC4ESK+ppsa5QQbW1gpnUDsJ25o6wHUBJFYO60M54oDfi8+WGUNtzGk3\n"
    "IHmvyFJQqghxhDCLRfDIAc3EdIFG6XcPiY6r/LelDCiVH3Odc2/Cn9iM25MVkG4a\n"
    "SajfhOLejZkBqYKmygPm1RVc6buLEo5hd+Uw2LKc3rytEIeFgAMLhl66FeMt/sGy\n"
    "xjTasrBs3qdd2lH6ddnZPgWKYc1BpNZ0LrYrFfJRqy2rAgMBAAGjZDBiMB0GA1Ud\n"
    "DgQWBBTQwjj9zLFhE81MWLg5zi1QEhRokjAfBgNVHSMEGDAWgBTQwjj9zLFhE81M\n"
    "WLg5zi1QEhRokjAPBgNVHRMBAf8EBTADAQH/MA8GA1UdEQQIMAaHBH8AAAEwDQYJ\n"
    "KoZIhvcNAQELBQADggEBACw47fx00D/oBlGWT6JhWwpfY2kNUiBaZDny1qz8Tf0j\n"
    "5uVnfuuCuZDwsqNwTPMmvG9C34F54GMXlQ/vb9v9YIk3RArTeNyCgJxEC+eY6+g+\n"
    "lO/g0QBRxCSE2JnHTJkNvflgz8NazYrkHs+fu/i3ug6mWkFHx4fh7BaR0Zb602H5\n"
    "NN8/mGbuBJ0TlvmuwKSgsgYDb3InP3g6Od+gOaSmdsrX7RnyAN9gYugbz0tG6DfW\n"
    "oEYzCupdAcUNuYCFmVkiOTjAK8qds6422rwkVwF2gaIJ5dF+fH6U0qu75jI/D5lo\n"
    "faUtSby6prrqx+YjlYaVZzH+OT8iVpnqqfqKjXUQTfU=\n"
    "-----END CERTIFICATE-----\n";

// Matches server_ip_cert_pem above (RSA 2048-bit)
inline constexpr char const* server_ip_key_pem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDTOzWcoMa44Qpw\n"
    "SpJCqqZysMe1oUVRpocaRiTd436uE79i9n3zBAJKrR0LL0J2TjcZcn6lcyyiAfdp\n"
    "mEgqht6KSIb7YzTuC3fsKi8b1bfkAGPpw9cQrC4ESK+ppsa5QQbW1gpnUDsJ25o6\n"
    "wHUBJFYO60M54oDfi8+WGUNtzGk3IHmvyFJQqghxhDCLRfDIAc3EdIFG6XcPiY6r\n"
    "/LelDCiVH3Odc2/Cn9iM25MVkG4aSajfhOLejZkBqYKmygPm1RVc6buLEo5hd+Uw\n"
    "2LKc3rytEIeFgAMLhl66FeMt/sGyxjTasrBs3qdd2lH6ddnZPgWKYc1BpNZ0LrYr\n"
    "FfJRqy2rAgMBAAECggEAYxmlRm2brgNOnW4u/n4Hh0lu+MTHu83wFqCQDVX9CfiT\n"
    "0v8oCgp4dMaRGL08Zjq92P+BcWf+qadYhz79pI4P/DqYsXpSy9evlKoZ3eo/0wVn\n"
    "2rWZweW11Saw21w2YZWjesmCqgPXHwHbcvL2Men1QhyYNqEQq1BxvM7vdqTvO//y\n"
    "4PW17k630gzTaVreM6t/Yf2KJXqeb0+42tRvgN+BhybCKj2uNhC/R08A/Rr+A461\n"
    "p2rXo0YP+6NSJBP/00qz7EI6aAqfu37Q8MRNwqAb+pub1CQpBwGOSqIa3uNWuEuh\n"
    "2/Cct/qMjuH39yt0anMevApvrNYsDZwtZotQsErb2QKBgQD21wVSKQdDXEnKQ7jA\n"
    "M2gWbTqrnJm8awWYjm0sBpzF+yN0enCuOpsBflavJQ2WtV2k7VWKOhBBQxPtwgdI\n"
    "FatDElO5yQz2XgShkYML+IJnufsMh8ZrKZ/gcW3rQyNDmkz36CheeWhdQyTpGAEv\n"
    "dvq8pM0SOpAOOEAXkNl4Z9G7lwKBgQDbEegV6RofSfWwfN52tL3K2DJQqlR+BYEB\n"
    "F8exeYaaY38csbHqOQFfot6kccOqr/EPKP2wf2h1fu3sJ74Hv3U2NDAOvoZQ3d7Q\n"
    "5LR0dBIkWlrbhEYCmHfnmRcofc9QmvS6bq+D6RgNGJTDwOo/vrojGGjuBbEnaz83\n"
    "HMg+PGFxDQKBgQDLMsrAjeHaw9hC12j5X9gpzhVkPHAaOYfLxEN+4JqiKFFRi5HC\n"
    "+5+qpRQ67ie3jund4TpvpcjH0K5RJU7VOnFXr3iZEjbHgTISxzS34AWJ2gIemI7w\n"
    "nL1uCDJSX1xiRF1kHwtMamlNjP6PnCEtr6ZNMOVYQjlgW1H3lFhR1DVFVQKBgDdZ\n"
    "KNgQUudA2nBCvDolpCYRxXSX9Ez6uwM5rNxsJdPv+3eWdasFyBEPp0zI6XTAixkX\n"
    "dDEZn5y/+wDFcb+nYcfWG6Y+ANWBmQASKH2brdG9emMn4kBZoUHEbhNu5egpnldU\n"
    "C8g6Jjd41G042nZMi96+FhS9H2skL46PGRCQVNYpAoGBAMlKXlcaVlb6Jf5tcQOe\n"
    "unY8LjSgAtwZJ9XQ0FAA82j+6oIndSB7AMkPu3mpKGV96cdhdxakGluXpKG2AC8J\n"
    "CDPJTLBEuU/DxEXME1wQ9o3w1RPpi1rE1k8kMVOcS0WZHhGrnyM3vMJuUmo1/Zk/\n"
    "43t2LygWhN+nrUbjLDRyTl7O\n"
    "-----END PRIVATE KEY-----\n";

// Different self-signed CA for "wrong CA" test scenarios
// Subject: CN=localhost
// Valid: 2023-01-01 to 2033-01-01 (self-signed)
// A real, different self-signed CA (CA:TRUE) that does not sign
// server_cert_pem, so both backends load it successfully but reject the
// server at the handshake (no signer to confirm its certificate).
// Command:
//   openssl req -x509 -newkey rsa:2048 -keyout wrong_ca_key.pem
//       -out wrong_ca_cert.pem -days 7300 -nodes -subj "/CN=Wrong Test CA"
//       -addext "basicConstraints=critical,CA:TRUE"
inline constexpr char const* wrong_ca_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDETCCAfmgAwIBAgIUb0TIQloicv2ipq+GAe9z5MnIVHowDQYJKoZIhvcNAQEL\n"
    "BQAwGDEWMBQGA1UEAwwNV3JvbmcgVGVzdCBDQTAeFw0yNjA4MDMxODMwMzJaFw00\n"
    "NjA3MjkxODMwMzJaMBgxFjAUBgNVBAMMDVdyb25nIFRlc3QgQ0EwggEiMA0GCSqG\n"
    "SIb3DQEBAQUAA4IBDwAwggEKAoIBAQC+3DBqcfjePUnZeh5oXFD5pYFMuLfvvyGy\n"
    "svQQUdOz2DEJKhITV460oXB6T0r18x0XVMq1J15d/NrnN2LKOqikHPTMnhBb/j4S\n"
    "wG8HFaajPD2TM2PonrhibzM1lOLGPs8LMs2U8Sh6uCAjTEzlwBKBB1NCZGnF9jnZ\n"
    "vo5XCpKFY8wSsKTXJFMza/3Ct0lm2Znt5wzojdC7fO9gKCIpvU/g/n3eWF8Nz2W2\n"
    "6rWQ0jmpY7nQEf6sRySy5jryR3lNldvOzdgepBv3o4J+H1RtZawUCLmwt4ygz4Oj\n"
    "DYEWoaOxu6bnTwYjNtff1GI0hNawCTbmter5Kk6OABS0rnWOuXCzAgMBAAGjUzBR\n"
    "MB0GA1UdDgQWBBRIgAIlF0OVIHX97izxSUMsxFtG9zAfBgNVHSMEGDAWgBRIgAIl\n"
    "F0OVIHX97izxSUMsxFtG9zAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUA\n"
    "A4IBAQAViMcVGaE3PyrxAB4apBKdeDuAPqezg5DEDd6R5PdEkLNWXkpDg1io3czv\n"
    "qsN5S9Bu6lwENGejHqRwwIejxXKyrgJOGJEm4oMUbuc7KnBgiQGXcxQ0k5L3355g\n"
    "Q/eS8C0rbuxc4JVXh1fdCU23UdvJ2NzgWJ0aqMYcwufnKz6B1C86EHz4ISZ9HtGe\n"
    "8E83WXl67bV8LBpx4S3j/gG7S/hPPDguuFsrxoRs6/4bY7COk3SqJnek+mVQt7zX\n"
    "fzL5MCO+YXQ0i0SN9r+OnBYtnHLUf/JFDLUERY3jp8ilyFdFLn4sVHtQd6yKN10T\n"
    "IXVcdZuIBIxWxaT+Flimpm2eFfAK\n"
    "-----END CERTIFICATE-----\n";

// Expired certificate for testing certificate expiry validation
// Subject: CN=www.example.com
// Valid: 2020-01-01 to 2020-01-02 (expired, self-signed, CA:TRUE)
// Command (Linux with faketime):
//   faketime '2020-01-01 00:00:00' openssl req -x509 -newkey rsa:2048
//       -keyout expired_key.pem -out expired_cert.pem -days 1 -nodes
//       -subj "/CN=www.example.com"
inline constexpr char const* expired_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDFTCCAf2gAwIBAgIUcWCw0O1DjiTT+alvcOHTN56vTh0wDQYJKoZIhvcNAQEL\n"
    "BQAwGjEYMBYGA1UEAwwPd3d3LmV4YW1wbGUuY29tMB4XDTIwMDEwMTAwMDAwMFoX\n"
    "DTIwMDEwMjAwMDAwMFowGjEYMBYGA1UEAwwPd3d3LmV4YW1wbGUuY29tMIIBIjAN\n"
    "BgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAt5XT6f6Z/abfLI+L0MYD5cszhBa+\n"
    "3h5ddlXypIerCwxiKR1gnjWafdWm/ZriML073ozTAhgF0bQg1VPRNDeSvyAUSJQp\n"
    "5dPLjq1K4FwFBKAuo5GYWePE42vysAlOaJ70Rr0F2Lerk8e+FJJKGS9APWsi4FeQ\n"
    "fSJc1zfODCieSuePBtjmbZJPe9gGrcv8d4KjQo3C0hA2qKZIQTkr0bHmqUtup9m7\n"
    "0W5VNJdWgGdNpirDigCD/x4IZmEzP3mMnP0gp4JRsBEuGXi5nzejcpwrUHZL/Vmo\n"
    "MAvYOsIHU8ewOxuKflaCq5rJjF1uk/i2+CoPiMGebSekJ0J8PAIcqCVrowIDAQAB\n"
    "o1MwUTAdBgNVHQ4EFgQU2p57iEUtXAtUQV/iT5JZNSoHvdswHwYDVR0jBBgwFoAU\n"
    "2p57iEUtXAtUQV/iT5JZNSoHvdswDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0B\n"
    "AQsFAAOCAQEAEezlsqs0yc5FuegqLO3Hwko0knt4jpC2jOqYsId+90dpv8u/s/Um\n"
    "znr8i5jiiv9R665DpTEFF9/ur4bJ5a3rmTE2udy9qn4MZZco0pBZ/7+dtOHwEsfY\n"
    "+bS3Z+weVtsy8LpI6lUxREBUsmPrY+ZzEFOPdfWR1sh5NRX28oWW1ZhmaAdWjHNe\n"
    "YQUC+yyblwFCNqSEdVUdtAOlndY5OrYdUSG1AE7T9z7p/simSKLfC/5IbgX+N3PP\n"
    "0ntHB4+omQsBCqcgtrr0HC8he8xQrFBeEJBNwYevjMXvkcQIuwvvWvZtyMMJIw/i\n"
    "/V5+QRAgU4In8r91KfCHHIY2jnjopTDELA==\n"
    "-----END CERTIFICATE-----\n";

// Expired certificate private key (RSA 2048-bit)
// Matches expired_cert_pem above
inline constexpr char const* expired_key_pem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQC3ldPp/pn9pt8s\n"
    "j4vQxgPlyzOEFr7eHl12VfKkh6sLDGIpHWCeNZp91ab9muIwvTvejNMCGAXRtCDV\n"
    "U9E0N5K/IBRIlCnl08uOrUrgXAUEoC6jkZhZ48Tja/KwCU5onvRGvQXYt6uTx74U\n"
    "kkoZL0A9ayLgV5B9IlzXN84MKJ5K548G2OZtkk972Aaty/x3gqNCjcLSEDaopkhB\n"
    "OSvRseapS26n2bvRblU0l1aAZ02mKsOKAIP/HghmYTM/eYyc/SCnglGwES4ZeLmf\n"
    "N6NynCtQdkv9WagwC9g6wgdTx7A7G4p+VoKrmsmMXW6T+Lb4Kg+IwZ5tJ6QnQnw8\n"
    "AhyoJWujAgMBAAECggEAMVH0pQPrzduzUC7eQn+4E1eUZvOPYm/o7v4nGjGCb4zr\n"
    "oB0O1GIVN6Ia4z3lb2+fMmpF0+WtRomsWnNSnEMjzuno2RjI6sAMCzAeEglWpcf8\n"
    "z5+xPND2l5xsDgPqByxQ9uIYPIEXfLOoKrGka4Cosvdh3sBXhm6hX4ZT+is9X2TC\n"
    "kyoW906lMYXPFX5M9zb+GuGl3HuOXeLbZijwJ1tTMUZnk1fZyWEJt9kms4Fh7yS6\n"
    "CNYzjKNK5LSvqjKMlcirj0x+X3GI4oJ+KWCeUxoUMtokSpHVVVFry/noEa7o1yOr\n"
    "zCYWZQWeIJ5I2RrC3AFTMQATSg2s/DvjHPHazJ7UzQKBgQDhaamCOnPjk3vJBRNh\n"
    "lt8/47rBOLD/Ua/Hh4iKgZ8MNJz6lHBTSd+ESZsSg9PNUCk8wmY8+LLV0CpRI+hF\n"
    "0VDckyjmr1TqVBoc2GBpjPE6skUod/xBZOdQ4Upm2rF8E+JDMbuB8brcCJFCQYLM\n"
    "GG6llHDHIczOgvp2yujCMxWJNQKBgQDQfywH3yQVuePiPbyiGK8ARFuMdHwlVwSP\n"
    "FzivNXVVJp1E6zHoLHAOHIwUsVZYunflDKZriZ3AxjeiSSIMTaLAcPgGp8fP4sdX\n"
    "lvENvjM4QggtYEVyuo5XrmovEtV6at8O5p984dwaAQoznZZv2K9Kt6/gx6a3+zQt\n"
    "H8bdKJCUdwKBgD7KgD2WqtGqM8E7eLqmnGnfthY9BJEa4CxkxNRQZ02vGktzLhcF\n"
    "bQ4csuXlcwquWc5jGLfDT43f/um7ZuiL9kp7c9lO3giohN2kKLc+W7ROFJXBVrOg\n"
    "uA7/swoTwX0ezNiK8gCwpazFdjFOrnDMHYZiY0gVUkf0lHCi9VOjh0xBAoGAEW5A\n"
    "WRwfoS1cTuLIbWjQ4J3WZYSriFehCvFvDL7UY10KEuPy1S055QQf9e7pgBt+wIhx\n"
    "NVZY+O/ZYNjqXsryy1Hmem/2dXvJHJqC5po7H/3tPxXoWHIeSlhLiknxzP04Tr+b\n"
    "H86mHwptNul61TjxVrbKnmkyl/kJYKhicMTeaXsCgYB+wNCxuQ4MIzErm7CXnKCp\n"
    "xQoFFzR0Fhay5x86Ry9hxBYCeio2CSByV+pFX0AOvvJ7hhm3iSD5p91ulIgl3YfL\n"
    "23Ot+Yles5ZYawVJ3cqeFGiG7vPi2KU9EztdnRlmJwF7P7m4XzzcNvvbK/FbOQT5\n"
    "E7D5rHt+zVEyi3BDrCSTZw==\n"
    "-----END PRIVATE KEY-----\n";

// Root CA certificate for certificate chain tests
// Subject: CN=Test Root CA
// Valid: 2026-01-22 to 2036-01-20 (self-signed, CA:TRUE)
// Command:
//   openssl req -x509 -newkey rsa:2048 -keyout root_ca_key.pem -out root_ca_cert.pem
//       -days 3650 -nodes -subj "/CN=Test Root CA"
inline constexpr char const* root_ca_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDDzCCAfegAwIBAgIUQFc5HqhX9NsPK7m+gssB9iLY6VwwDQYJKoZIhvcNAQEL\n"
    "BQAwFzEVMBMGA1UEAwwMVGVzdCBSb290IENBMB4XDTI2MDEyMjE3MzAwOFoXDTM2\n"
    "MDEyMDE3MzAwOFowFzEVMBMGA1UEAwwMVGVzdCBSb290IENBMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuZrR4YgRV9BC/9MnG6U0+3m8l+UDhklBeF04\n"
    "nVeRhPQmDMDbZ4TxnH9zBc71EdvgCqVJr2GGa5QXU0a9yjKB7Vb97VFjO+MAZGjq\n"
    "GRzuYDdNUlj0ZOa04ZIWLhRvTr5sA649DonSxw6tEla+PZtsr/numK6OOCkAa24D\n"
    "WDEtWOHIp/xyLwGsJrwkqDniteQHec7RugufC9nvZHpiC/y23oFeRsg9cOda6hzq\n"
    "LMvFV9lZkjp5ChlEoY3bNhDXG53l47k11Z0Qnv4A6SPVmveFS+D74KxbORdWIu6k\n"
    "dd/C2zJ18XiT8N+NXgacEaSj8ygHExQ4BC8MyvJGqm8ZH6nZ4wIDAQABo1MwUTAd\n"
    "BgNVHQ4EFgQUgzNRvlv4m9jsyfNAVU34IbvmiMcwHwYDVR0jBBgwFoAUgzNRvlv4\n"
    "m9jsyfNAVU34IbvmiMcwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOC\n"
    "AQEAQC4S3sa4ZbSH7Df62GSZaQhD19HKMshlXCk+E2QwC7cfnaAAE1CKemd6hPe5\n"
    "4Ofci9YdbRl6g0LF3SQe+DMMiK1sqjCSnEAOuPJ0fRcaVkh87SuUHOhucC9TQoLn\n"
    "/oUPSQHvprghJk1HVOq7qQI6iQZjurODNBtddVAkk5r/1p4vaRPBtr471i3GSBBc\n"
    "Hy51FXBcO+9910w7Pxrs5htSnAh5Eprn0+P0h/1liQhT5Fuz27PFTxCttcNvagfD\n"
    "rdtULUbjRBePcR3ooCj88M2ndF0ifvMvGBYtsBdaY56dc0zkYACyiiFWV5kmSLM8\n"
    "ay5B/d3dN2x7UoJRiZ2X7jD7sA==\n"
    "-----END CERTIFICATE-----\n";

// Root CA private key (RSA 2048-bit)
// Matches root_ca_cert_pem above
inline constexpr char const* root_ca_key_pem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQC5mtHhiBFX0EL/\n"
    "0ycbpTT7ebyX5QOGSUF4XTidV5GE9CYMwNtnhPGcf3MFzvUR2+AKpUmvYYZrlBdT\n"
    "Rr3KMoHtVv3tUWM74wBkaOoZHO5gN01SWPRk5rThkhYuFG9OvmwDrj0OidLHDq0S\n"
    "Vr49m2yv+e6Yro44KQBrbgNYMS1Y4cin/HIvAawmvCSoOeK15Ad5ztG6C58L2e9k\n"
    "emIL/LbegV5GyD1w51rqHOosy8VX2VmSOnkKGUShjds2ENcbneXjuTXVnRCe/gDp\n"
    "I9Wa94VL4PvgrFs5F1Yi7qR138LbMnXxeJPw341eBpwRpKPzKAcTFDgELwzK8kaq\n"
    "bxkfqdnjAgMBAAECggEAWytxbRcpbbkfMArIawv7uotR2ErmMFBLmJQx+xfIo0ZK\n"
    "anlRTMhA5l60YWYHe35FzvTh/QQqwy07R+y3zVqB99ODZ89Sr1gSGUBvvWY4sYp4\n"
    "sLqBUg8BSsw3mOrwwf1HkYdE9p88qgrLePai/CAcg1SBnv4fXfbF/f9MJUYCwGVP\n"
    "bXrWq9JQcL2e867UqVqlJMiFB0uLs4kYGJEz5CZQMwBU9bgpBtnPpBXntgsbyDIu\n"
    "5y3kNiiPHs1VU9F99J9kacVfVAv6vBZH2Y3X9IOG8gQwOoAil4f6zpM9CFUj4LZs\n"
    "tPPS1glYbjmhOdlljC5eJCfLJC+9Xpwyp5ZN5duoAQKBgQD8tBCefPEeY2yajtg9\n"
    "0/L/+ODX+AjfyijdSs5G/0U0ZsHaKSedE0GNbEksLgXAgH1JsZxwBOVoxJizprTn\n"
    "q8hu0umaoJ3Zf50l23uMJqZK4Tnd3R+oTBuHjVY052zSEbpbtbB1Ha3urnHMCcdS\n"
    "5nYg0qLG0bYA+FwR1c8tG3RVsQKBgQC8BquqrCPsZxRge3+bBTEKj9W4/vPWKp+s\n"
    "jWI1mXyQhFceZ9RLYTAOp0Mbi9tAvk9ovcetodtCnJqoD04NaoLRE4hIC+/UUqWI\n"
    "OUEWCDO+02g+mMuTsRkuFj9HIWPUXd9P1j4iTycSSKFp1QM85t/ggYqDbIiOgfhL\n"
    "s/sbYQQJ0wKBgCW80jqI2A08tcxDBsH88+4MAa/e55xb+UxKzpFFr9UKf2qP+M15\n"
    "QbHX+PlzCgLcbVli/8Suxn+l1FQH0j5CphT+xEoGMGx5pUMxCrs8TlsiVVzvl7mv\n"
    "W/EbR0NxSAv6/8SQVoC25PGe9XmOAEk+B2gRbKOaT77HWCCFuIG49t+RAoGBALQ3\n"
    "dIyWh6wLtLUxScJsvG+SI1g4TcAlhHvf25TiM0lU/yduf0VstqIk4SZi61hn0Dbl\n"
    "R6D9tOlorreMS9SCFTaOER51Cn8oY+5oaiDS5b3uZUkyLFW39hl9S1NDBqtC+kpM\n"
    "X6uE0D8vDD8i4wKZi1Vk9D05Zr2ohzMQJAs+9p7vAoGBAPCocp7uY17s1rKvtZvM\n"
    "N+aTXVRpxVya6ICunCpk1VhcAwT6EHxvXKKqa3c6xZLGMkLOpTqByzC2f7+Ur7i4\n"
    "btGnK3i/LAhBPWDvfpafnUeGaODCbxr8+i/e6xwF3a1bwCd5SPxPzS14FRfcScmZ\n"
    "A4Q1Y46cpHN/bzeTQFG6tMOB\n"
    "-----END PRIVATE KEY-----\n";

// Intermediate CA certificate (signed by root CA)
// Subject: CN=Test Intermediate CA
// Issuer: CN=Test Root CA
// Valid: 2026-07-09 to 2036-07-06 (CA:TRUE)
// basicConstraints is marked CRITICAL: WolfSSL enforces RFC 5280's rule that
// a CA certificate must carry a critical basicConstraints extension (OpenSSL
// is lenient). Regenerated with the same key + subject as before, so every
// certificate signed by this intermediate remains valid.
// Commands:
//   openssl req -new -key intermediate_key.pem -out intermediate.csr
//       -subj "/CN=Test Intermediate CA"
//   openssl x509 -req -in intermediate.csr -CA root_ca_cert.pem -CAkey root_ca_key.pem
//       -CAcreateserial -out intermediate_cert.pem -days 3650
//       -extfile <(printf "basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n")
inline constexpr char const* intermediate_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDJzCCAg+gAwIBAgIUcUBzbbnpdTvewEIRh0FLBDvDDBMwDQYJKoZIhvcNAQEL\n"
    "BQAwFzEVMBMGA1UEAwwMVGVzdCBSb290IENBMB4XDTI2MDcwOTE3MzkyN1oXDTM2\n"
    "MDcwNjE3MzkyN1owHzEdMBsGA1UEAwwUVGVzdCBJbnRlcm1lZGlhdGUgQ0EwggEi\n"
    "MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDCqobUGWRLfletWGsTWGdySYCb\n"
    "l2DJ06wVSW/TXvozFmIMKve4T5LKFDTAQtVrp/hK97HqAlTXWjhMTqq1SYHlN4dv\n"
    "utguzY7Vf96nJWVoJzsq7jAVhukK3bpRo6ytMcj6TRK7DIELKsbCOtvsLTxl0iGk\n"
    "26uE1zn2xk78GXJLRL5QHgeMrkgwWEdY8AeHm9VJ+dxBtnhzPR0z/AFaMmPODMSN\n"
    "+HGkDwVyBxOiPrt9GouEci+rx7AUv3Iv8wLZ+AOiCC0Fbfe9zMqVxVppRB8mUt4c\n"
    "+Np45GnIUk6/Fi+pdNJLTEE5WnoiA87GK+CbAezZt36vYIxSUIfoGz0jKrbpAgMB\n"
    "AAGjYzBhMA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgEGMB0GA1UdDgQW\n"
    "BBRsXLruQi4QIh95qM4gWkjL1gc1ZDAfBgNVHSMEGDAWgBSDM1G+W/ib2OzJ80BV\n"
    "Tfghu+aIxzANBgkqhkiG9w0BAQsFAAOCAQEAc1MilNqUSic4RhznHdFj0fXTPQ0K\n"
    "73WRf/6TmFcoQymlJhMqk2e3NJVxsfW7G/DfR/0Lm2mOn14mDczIBAHFGMdEb4+s\n"
    "iUtu992nroGkxf4euwlwD+LJckWVbnJ1kUhx5WBpFICgW5dvF5KFmYNf9fhLXQs4\n"
    "Ltt/hnNtF4b+vzjBH3xq8LGyQZyt3BkyjLQcHFnPsKepPGW1JFn/2POI2as7WMob\n"
    "vy3qJ9hh52y3K0VO/zxtVef9cpOTHLH1Eo42uHLc5xQMhDN9Kvv1oywthZYyZ+Mc\n"
    "r25U90/0AfnuJjXdCUhj1EMW6QiVk6MQrWhiUXUUKjFUmnChEs++TcL5hw==\n"
    "-----END CERTIFICATE-----\n";

// Intermediate CA private key (RSA 2048-bit)
// Matches intermediate_cert_pem above
inline constexpr char const* intermediate_key_pem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDCqobUGWRLflet\n"
    "WGsTWGdySYCbl2DJ06wVSW/TXvozFmIMKve4T5LKFDTAQtVrp/hK97HqAlTXWjhM\n"
    "Tqq1SYHlN4dvutguzY7Vf96nJWVoJzsq7jAVhukK3bpRo6ytMcj6TRK7DIELKsbC\n"
    "OtvsLTxl0iGk26uE1zn2xk78GXJLRL5QHgeMrkgwWEdY8AeHm9VJ+dxBtnhzPR0z\n"
    "/AFaMmPODMSN+HGkDwVyBxOiPrt9GouEci+rx7AUv3Iv8wLZ+AOiCC0Fbfe9zMqV\n"
    "xVppRB8mUt4c+Np45GnIUk6/Fi+pdNJLTEE5WnoiA87GK+CbAezZt36vYIxSUIfo\n"
    "Gz0jKrbpAgMBAAECggEACa+QuLM5lykrAxFMm74XwLjcrHN9ws0NtOTePPcBHa7D\n"
    "tiNHdUCHMGCNAIUb7oaBUHdQ48L/E/kqFIQvzj8YEgx8+qnrTy+2As/FrAiIBbuC\n"
    "jd210aD1G3kEQ2ei3UxhtQuzjFAr0UPawHNLkN/uL2Y1e3tKnS7nKyPkksO979FM\n"
    "CbcZw5fsxrI1zup1sUY+Z6SFoHxZmsXcUze2Nh5kdtJ09DSiMR7FhnnK84Q42UXF\n"
    "IsqzzMH5MzGzloX8TRJvEwQkuLZXmDSx+3rjCh2hGhTgx8XkL25Q6q1PNv0+OYcu\n"
    "ivsARHxtNiZyjXnBn/F5AxEzOAIpuowYHiJmk3J2+QKBgQD5SEe3VhP32Z0zYBsL\n"
    "4OK3jUPYTMa8fA9A1RKnAB/ygI81CnGA/p/Sluo47WmgEcxuaA32cox+i80rt4f9\n"
    "/1agVjRRJclxHn5+KSsnJlznGONsY9+DlvHyaoJoyT1yrWFtReywLKe9vhfjUrjK\n"
    "2xZq3/KClmJMd47Qq/NKec2gfwKBgQDH6XdtlnNYaO8qw0Tomy0m9wwjjLQVk8OW\n"
    "neTG7dePvD9g1CFMYlYSE5+8nSpy+56hOkgdz5ngT9tspue8RoIyqkEdxlMlaPqM\n"
    "67cjxdhdMqB0YtK7M07rkYqp4+k91SNWUPSKyXEVPMMbtITO9cHBuc2kl0Iq2T7N\n"
    "vMEuvhj0lwKBgA9MZkpUGAmf60vZ3A8QkBlfrAg8Pf4XRwBdkzV4hn1lcmR47ZpT\n"
    "Bg/wfxNbTp4qOXeVHzY+tWyWu9KxAsGNyA0y/Sb1wLUWgADSGfnfGth76IkgX/k9\n"
    "bD/KVZKEtyawiUghgHMXanv0jJbA3uJkK64HbGSjQgkbVUJtKxMpAnuVAoGAFMnL\n"
    "WIL/pZ7r1/eMT9/rFxUzlvLHu0KtYRk0NBeBhfneYVRNziKfrquJvdReGKzftwZX\n"
    "f3oaF0BWofrNOD/gxCH+OXlpJge/ni3Y0oh9Ulu0YcXxAfR47XgqAjaoB30FerFa\n"
    "bKA7+ShjZZslAFx/9IQ8xTPRdqE2rbBGKnUsJSsCgYEAwJHMAurHH16QSGPnEFTJ\n"
    "3x63BYzRf+4S+IYtlZVJk/iZvk5Ru/ezW0cOK+Ty3y/w6vANlc2Eaf2nZ4UZH07o\n"
    "MqPoJs1OF0fCZwjWq26fJ3MigLvp1Mo+EwHUExIvkB4QOs9bcDH9FHNBs+qiV6By\n"
    "p91byQ0HYRzDCcHYULcZjkM=\n"
    "-----END PRIVATE KEY-----\n";

// Server certificate signed by intermediate CA
// Subject: CN=www.example.com
// Issuer: CN=Test Intermediate CA
// Valid: 2026-01-22 to 2027-01-22 (end-entity certificate)
// Commands:
//   openssl req -new -newkey rsa:2048 -keyout chain_server_key.pem -out server.csr
//       -nodes -subj "/CN=www.example.com"
//   openssl x509 -req -in server.csr -CA intermediate_cert.pem -CAkey intermediate_key.pem
//       -CAcreateserial -out chain_server_cert.pem -days 365
inline constexpr char const* chain_server_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDCTCCAfGgAwIBAgIUICKZdMPYLi+vx0rER9U9G0/zzecwDQYJKoZIhvcNAQEL\n"
    "BQAwHzEdMBsGA1UEAwwUVGVzdCBJbnRlcm1lZGlhdGUgQ0EwHhcNMjYwMTIyMTcz\n"
    "MzEyWhcNMjcwMTIyMTczMzEyWjAaMRgwFgYDVQQDDA93d3cuZXhhbXBsZS5jb20w\n"
    "ggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQChfRaitIy/YbFh4Wa0KomP\n"
    "EF8tU3QzyOQ8tD0bxQx8hG6POBEjVh7FUf++n6Sm72UbHGH7txQTNpmoihBp0M1N\n"
    "Bkv85MtaevOkTEGtmY552rHPWezIpOMM6A9Vlu5H6tYs+2zorQJ9VfPt7mGbC56L\n"
    "nOCMEujSwn2B8y0/jh1ZXSe8wGHokBrbigvsJIGNJ1T9HmLf+SaXN4hrLPar8u6S\n"
    "bsDe78l9ZYxyUr8HTAzHuJksxkRbi7z1kQUVKXSg6YoKArHbVVYF8COKRApgTmjY\n"
    "FxIkgpRyYPOnwTQWShzx+Frb0jx1wMagapR07B9Q2Ozk+X2UDPsOj//94J7xJq2f\n"
    "AgMBAAGjQjBAMB0GA1UdDgQWBBQ0aZz4UflELiLyRCbpfJJbn/uFqTAfBgNVHSME\n"
    "GDAWgBRsXLruQi4QIh95qM4gWkjL1gc1ZDANBgkqhkiG9w0BAQsFAAOCAQEAiUKb\n"
    "rDKCzkxU+yT6xG+Dplwhw1218C34QSaMQfx/6qyGYTZfhklqUUeA2sjtBFzFeeWy\n"
    "H7f5eM+i9IBPskd5AJMZpWDv2jA2TgJypvJuTdR3JC0M5bbOLeU57JxLxdizGzAd\n"
    "GR56ERvzeOtHJwnEOsaz8AnSGY3gurAgPI6n9FpQtc25/bhLreknhx5Y0JYaBRPw\n"
    "O98I4pZz0QmtWuaro4LN6vlJf58krvKPKhvuCwEWZvGN7PkC2XbKGf/Xko9/a0Bn\n"
    "l2+4NI2lFdUrd3bperQVMXKm+U3cFHLXm6x+mqUcA5Epz5DUsQZhs18GcsdQh7NG\n"
    "7T5qXswPM7MpHozuTg==\n"
    "-----END CERTIFICATE-----\n";

// Server private key for chain_server_cert_pem (RSA 2048-bit)
// Matches chain_server_cert_pem above
inline constexpr char const* chain_server_key_pem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQChfRaitIy/YbFh\n"
    "4Wa0KomPEF8tU3QzyOQ8tD0bxQx8hG6POBEjVh7FUf++n6Sm72UbHGH7txQTNpmo\n"
    "ihBp0M1NBkv85MtaevOkTEGtmY552rHPWezIpOMM6A9Vlu5H6tYs+2zorQJ9VfPt\n"
    "7mGbC56LnOCMEujSwn2B8y0/jh1ZXSe8wGHokBrbigvsJIGNJ1T9HmLf+SaXN4hr\n"
    "LPar8u6SbsDe78l9ZYxyUr8HTAzHuJksxkRbi7z1kQUVKXSg6YoKArHbVVYF8COK\n"
    "RApgTmjYFxIkgpRyYPOnwTQWShzx+Frb0jx1wMagapR07B9Q2Ozk+X2UDPsOj//9\n"
    "4J7xJq2fAgMBAAECggEALb1S5HnUJb7jcZBYuS4VMUbXVmy9TI+ZidIZPtzUmQ4f\n"
    "jIQ6YnJZm9UKZXEtPzUuQ3wKCrRDxN9hrVmRpY8FH0xpyHL7YCDUEpSgw61rK/t0\n"
    "AoF7bic5wiWWdk0eJ5ON30bFha+/NUXbpegvkC091lh0R2hxtoRs7Ro2FjrH+E/V\n"
    "oLT23HGnUYSI2dNjxduFspAqPh3xNv7yjRrCc2KT83ku5GYhsiSg8WTbq7IBtUav\n"
    "1QJ1tyqsLxFnFcDpl9N3Wh5r7Xbf8FL3w12m66efJ7yGMCLOJDxGDkRL4fnQyGQV\n"
    "WPYe5K9vxyw/IZH6f2cq/3FEZmgo5nTz4rxInmQ3xQKBgQDRAV8MpzF6xygmcG+/\n"
    "udIQdS0RDJrH1VE2mwyGvQqJbsNGIBOgDN/UIApRlhhA6gJygBd3Uj1cMAAmRI8d\n"
    "KvJBEB8ivbzwBO3L2eE9918aPQ5p+bNbN5c7uohBpZqmN7eUgWodi/omQR+86Kfb\n"
    "VAILXQhd4cO8dDNrCI3W+ahAQwKBgQDFzJclQZVMDdjuM4MyafF1ro2azhdHUe7n\n"
    "a3JCi1PkqM1BjxuEfhFZKViqcnDrpOLamW0cMICfICOCtTapH1QaxjdaoLDS90DN\n"
    "SEishTMJ2e7nHXr2TNeE/PXWNm9yualu7EUwhTgoEBM5fvFbywCfHiVFS72QBJrD\n"
    "CgWNWgAFdQKBgQC/XCYOi7X92AKmzyNBw3zVnLNafNPqSyFEgcmCQ+s10bfwqMXP\n"
    "MHpu2bcY4/fo11jORQE3OpD7quc4ImV2KzAK6hvXzykCCUE/94kHF0p315cu6HSS\n"
    "+973zN2cXWeu8CyhR6xEyTiLdez9JXcqlUwZ42AZtO9lyG6bfQWA4qxtyQKBgDgL\n"
    "8sABx1YXjmJggkpkrqCT51f4EayJ0NIOJgApDop6Mj7jV/7A4hWLm64gY1LCE+2x\n"
    "D7OvIqL0Llu5EVX2pJQ5mjG52qDMorYIR19rFr0x3XnrZo4n0+HA87/RCN9PMG1X\n"
    "0XsgJHtloqzmBWnnKbPsjM8H2RzX0Sp2yn/1ApCJAoGBAME9q4pqI+5blm/9r77R\n"
    "OtmUGjIFCxQgViscMpAUq4vNJziofgYdXB/GjtYV75coruvP3MqMc4+Zgrp5tyU+\n"
    "slMAs4tq3nqXViDFJBU/IEDk+8Fwn0zDPCWvlHjEDgZ3J7FioxbTjSqMn8ozoReL\n"
    "ivz83oi40E6Mou2cdfF/o5S+\n"
    "-----END PRIVATE KEY-----\n";

// Client certificate for mTLS (signed by intermediate CA)
// Subject: CN=Test Client
// Issuer: CN=Test Intermediate CA
// Valid: 2026-01-22 to 2027-01-22 (end-entity certificate)
// Commands:
//   openssl req -new -newkey rsa:2048 -keyout client_key.pem -out client.csr
//       -nodes -subj "/CN=Test Client"
//   openssl x509 -req -in client.csr -CA intermediate_cert.pem -CAkey intermediate_key.pem
//       -CAcreateserial -out client_cert.pem -days 365
inline constexpr char const* client_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDBTCCAe2gAwIBAgIUICKZdMPYLi+vx0rER9U9G0/zzeYwDQYJKoZIhvcNAQEL\n"
    "BQAwHzEdMBsGA1UEAwwUVGVzdCBJbnRlcm1lZGlhdGUgQ0EwHhcNMjYwMTIyMTcz\n"
    "MzEyWhcNMjcwMTIyMTczMzEyWjAWMRQwEgYDVQQDDAtUZXN0IENsaWVudDCCASIw\n"
    "DQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBANWTZ/JgpF91Xh3RukLVwnuu1Tld\n"
    "fSYVmCgoFh/lYQeBjHmls2JXRaIsCG35Fn8h0kaas7B/Zz8Ym92k0zMvhfE7XzYi\n"
    "EVQO9BMFnpTI5bUgQId4p7tZ4FyCQ58lnlVE6ytFkx9yWBS3YK89qsHqtVLFz4ry\n"
    "ASNZCKVSPBiDi3rmH88BHPQid6agj/1vU3qti4YptNMXclMmUfgIZoGq3sjVvfMl\n"
    "FKW8fDRl2GVlH9NgfnCeDoobOszw7Xckn3bibTh1tmNbQ/DXHXDQqwHqDu/nCCR0\n"
    "BDHNxFeZj1WW0AVgN/qd/MSZetslyjrVnUrhf33FiMf3JUw+iExEIYKE02MCAwEA\n"
    "AaNCMEAwHQYDVR0OBBYEFPv4jcET7PmxUHqXqV8uSmLBdW1yMB8GA1UdIwQYMBaA\n"
    "FGxcuu5CLhAiH3moziBaSMvWBzVkMA0GCSqGSIb3DQEBCwUAA4IBAQA1yECFvGJ0\n"
    "+KBBUzU++8v7xhl/tMKt7gqCd/2dvr4KW9iH6euYW/m3sl3iZ/h2O4kshSWTyVnc\n"
    "aumFusDxsMFW6h0XdQ0MlX1BIQC9aERZhXTG7LeXPKvrUmDTNeNdCI2xokVSVGmh\n"
    "FiQLllUhmjlKpwI5r5AyoUegpdNmXGmDqfpkrQ7aHijwZ7agyceCLlfJAujDVMBe\n"
    "5AKW6CXiAlWbTuzPDzl1SZGTIzBNErHqEGg/MfxNVJfqxvhT5/pVQTaoLICvVgZG\n"
    "Y7aGqGhK6eBv9NjOFHoUJvfBKTXfzklc0S8LgMZCvFTkoLMAvQiS4ebohgW9iQuo\n"
    "8KVXaw2EqiiJ\n"
    "-----END CERTIFICATE-----\n";

// Client private key for mTLS (RSA 2048-bit)
// Matches client_cert_pem above
inline constexpr char const* client_key_pem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDVk2fyYKRfdV4d\n"
    "0bpC1cJ7rtU5XX0mFZgoKBYf5WEHgYx5pbNiV0WiLAht+RZ/IdJGmrOwf2c/GJvd\n"
    "pNMzL4XxO182IhFUDvQTBZ6UyOW1IECHeKe7WeBcgkOfJZ5VROsrRZMfclgUt2Cv\n"
    "ParB6rVSxc+K8gEjWQilUjwYg4t65h/PARz0InemoI/9b1N6rYuGKbTTF3JTJlH4\n"
    "CGaBqt7I1b3zJRSlvHw0ZdhlZR/TYH5wng6KGzrM8O13JJ924m04dbZjW0Pw1x1w\n"
    "0KsB6g7v5wgkdAQxzcRXmY9VltAFYDf6nfzEmXrbJco61Z1K4X99xYjH9yVMPohM\n"
    "RCGChNNjAgMBAAECggEAWl1pILtVMPKG5NUFGxw4kn5Rx1jQB9ohK/RyEALMgBGH\n"
    "Lz013gkQ9GHvGyDGLPpRbwArwSTWuXKfGDOSDNkxsfSt/0iAznEZQichhtBNqMpB\n"
    "o1Agn/uSG3IeTGrtSCTF3+QrMKX/sJw6M0tDQZMeLyx0+NQWOS+FofVeafzWeiO3\n"
    "soY3iQLCsVInQALFMrPUHbNGln/8gH+SuqSYThVx0nF8k464v/3rueiNGX552pMX\n"
    "0hkiLoXq92AlLrrqoSurJxgwQghAMtO+fyupfeE+HcNWBX0nTAl/DuFud5qJJD+O\n"
    "A4p6Oz7lD+wThLxpAItfe+XWsDcYlIee+AcrgpKvcQKBgQD7fg6JvePqm3OtT9oW\n"
    "wk+ozWeGnP3u5AVq2HgHtmmOCWhehqDJPoLkF9bkEymrMAQiuBIga4JdOZeG+tho\n"
    "sobAhBbtBPV6HtE0Xt/i53x7T+v4kF7LNcL+/eZf2FX7ARW9xcNHhayHNTmgLWXC\n"
    "sFizkmAAjrwYhcIZWMJA4xLS2wKBgQDZZ1+WpOh7VeSeopiwJ8fkPXooSUwf130f\n"
    "DM9x+0F2yRcr4UrSOU6XQSlc8LKmRSDJ1Orol7RRTtFGg/pd91hIjkQJodMyed1/\n"
    "gCKADy0p3rDhzCq+rwUHD9G7T5AhQiPr1eyXx3Wo1PyvlGc+IJeDL4cAsbaIWhkZ\n"
    "dHYqgFl0GQKBgE1Yy6fZWwuAm+cls/Fj+ZP0+G4SQpcCUhg2U1Qr6fLhOdQ4m6LJ\n"
    "MwBrxI+IxTv9HIiBDDIkXofFerDs3Tn2DjOPbG2hJM5WRAlTVJA4mbRjNDPSUxU0\n"
    "h7Bc7kl0A52bC9C9zf1lQ1aiLALzc2SZT+6KijQhsf/ow3WAMt45+EQZAoGBAL1S\n"
    "uHuHwK0nb6B2GGHPQtQQdYD/07sm/V882Kp6E9hN5k/gMjhAj6BIrqyxL+J78MHT\n"
    "GX7UHcNwz+6IoE+URt1ohvecZT9fwPR3sZOzo7ECrSb1lYPZBpfPvuVPtERCROXr\n"
    "tc23dU9Bq4t7wSzpVQh5Kyf/muXDEHiKYx1ACKaBAoGANfg390PT1HCo8/i0t1ZA\n"
    "LnXWFQHU9Wg6UxK1KGN+sU6qsyaplE3E9N4M4CsfDUarx0W8KaiaQWp6jdveU7r9\n"
    "n81WOIBRLb4/Ew9ZJXS3V+bf5DS2LIHc0C9NUWSeeI3inB0xgERy6vtbdaSBfnmq\n"
    "J8I8kP82/dhlU/5NJGiPwqg=\n"
    "-----END PRIVATE KEY-----\n";

// Wrong hostname certificate for hostname verification failure tests
// Subject: CN=wrong.example.com (instead of www.example.com)
// Valid: 2026-01-22 to 2027-01-22 (self-signed, CA:TRUE)
// Command:
//   openssl req -x509 -newkey rsa:2048 -keyout wrong_host_key.pem -out wrong_host_cert.pem
//       -days 365 -nodes -subj "/CN=wrong.example.com"
inline constexpr char const* wrong_host_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDGTCCAgGgAwIBAgIUAJXP7QDgWvI47I5I8IQcxzXmtP0wDQYJKoZIhvcNAQEL\n"
    "BQAwHDEaMBgGA1UEAwwRd3JvbmcuZXhhbXBsZS5jb20wHhcNMjYwMTIyMTczMzI2\n"
    "WhcNMjcwMTIyMTczMzI2WjAcMRowGAYDVQQDDBF3cm9uZy5leGFtcGxlLmNvbTCC\n"
    "ASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALjYkpWNUeZgZvdsaawTDd0P\n"
    "W6DReKnBP10u73ZgY/8a6XJxVqo4jUK5mKH5SD/LS1rJB4nsgi2l8P5eNx2UpFED\n"
    "/ybGNxo5nPhIYnwyvpsmNj8lZGdMUke+AwTh3QIM7lRebPxhSlMbnS/F9+1mCFG3\n"
    "ijReW7UcwGewMx2s775dFww6tNmzVcvXeer5vgAlw/LkgI1HPhqwOCvnJQn1Q+Y4\n"
    "VzMzb1FYEM3gPfNP4qPwJe8ut38CYVadEofKnRtTuutgjKAWlGe+EveBTbUuHfe3\n"
    "laA672JDrdwzgeJ+LfrsMerzsyzQnrh8/eMiGjdLAduTw3H3lM6e2SVYfPYjIBUC\n"
    "AwEAAaNTMFEwHQYDVR0OBBYEFLeFbAlvr2VFP3+vubuYVXZwm7YAMB8GA1UdIwQY\n"
    "MBaAFLeFbAlvr2VFP3+vubuYVXZwm7YAMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZI\n"
    "hvcNAQELBQADggEBAJzvr3/8X18AmAM5CRUFwgoLLVxGLpmqeRcNxGcHUC7GrboY\n"
    "/HhuV1kPrn2vrdilCl3Ya+OeF8xh1t5ky8lX+MRkESWxylBh1/1E9hTSz/sIKmD0\n"
    "4dmJE65mc6YEez4CijGIKA4PqO1wHs8jnsxQCFDyRyAbTI2kBZv2i7OHtv8vo3EX\n"
    "6bhW4kV+x8//4RjZ1dAwr7fbDlkOleOdCe48kFX91q0AAhEjjpgUNWXMN2CoICLe\n"
    "QCphWvMv8vkKzRyyyH8FyBAc5ZnNb3gcBEZeuicivi7Jy/DZdA+KJKF607Fb7SPZ\n"
    "bC60J6FqZXhJQSss8hllyLXgzIYX/gTK8+Gadn0=\n"
    "-----END CERTIFICATE-----\n";

// Wrong hostname certificate private key (RSA 2048-bit)
// Matches wrong_host_cert_pem above
inline constexpr char const* wrong_host_key_pem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQC42JKVjVHmYGb3\n"
    "bGmsEw3dD1ug0XipwT9dLu92YGP/GulycVaqOI1CuZih+Ug/y0tayQeJ7IItpfD+\n"
    "XjcdlKRRA/8mxjcaOZz4SGJ8Mr6bJjY/JWRnTFJHvgME4d0CDO5UXmz8YUpTG50v\n"
    "xfftZghRt4o0Xlu1HMBnsDMdrO++XRcMOrTZs1XL13nq+b4AJcPy5ICNRz4asDgr\n"
    "5yUJ9UPmOFczM29RWBDN4D3zT+Kj8CXvLrd/AmFWnRKHyp0bU7rrYIygFpRnvhL3\n"
    "gU21Lh33t5WgOu9iQ63cM4Hifi367DHq87Ms0J64fP3jIho3SwHbk8Nx95TOntkl\n"
    "WHz2IyAVAgMBAAECggEAHaU4Ty1w1Oyhnu1/fiY5KzrHFIH74ufYIHsCUz8u0m9v\n"
    "wNe+EUNMHoczHEklZfvWDEug/qUUlLsgLT+RgdhAyTCFp6OTG0zhqK09RFOEH9Bv\n"
    "U03NLkb+jDyEcfBCeI133MafHpQA7lbHrS2IL4YVb/uqee8nMKMZlZeb/xapBaPV\n"
    "HqZ5+UTV6hT/yFqXD2g0nhHFkZb36JlU+R+WZJjcIzbJxuakteKZlTtjfwLDaida\n"
    "/kHbHlMVtiZkmVm5CZP8ICVykEfUvyzA/9/t9LE63GzUcBiEE23S+ZrBIrzncHe4\n"
    "h7I2dpi3sIZ1STguGjhdFZeTbrLLT0KsQJr1R4eMAQKBgQDeoKkkuLtjPhFHquXT\n"
    "GXwMRMBRVILtBsc4hZ8Qy+D1GiFTWxdxrLe/oU1z1zEWZhJjz20VmmqCmqGMbTHn\n"
    "Q5N+ZO3/k/2Y0KmnJ8iM3bKozROsMTv+xxB19+Xk3aXtFSqDzeu4UgyOYLO9AXRY\n"
    "1jkhv3ehjzfqZPQJ4aV4OQaUAQKBgQDUjgrN4jIQA7CbpHIqT95OwQBuu8I6bM9R\n"
    "zbAodBhFqXOMwcVOFY66CwSAjp+xEYaPZ+7aNKu+YkCdpOaBlGGzHhYUB2GU0E9w\n"
    "Byhf/LAI8WMGUMb6TSEZi8NysDgCzhEB+rIJKxmL5rKTlYx6K7NWErqarDdVKlX+\n"
    "3hrvruj8FQKBgF/mZ1ZJOXdjuj/cD0pjNPt39jxSol+GRvVDIiUzHgGXMvncSHoQ\n"
    "Q8sJqfqXnS6f45YZOU1QCkeeYq7CLvgHNRcCVT9+OYTFhf9adNqxeY+bX7kSMFzs\n"
    "1Vtr4R04mYxKTNkgMEVjGsOORn7JjJvkFBJEjz0KG7Udrb4/9G6YagwBAoGAGxJY\n"
    "R+6mR6ngpYIlVERF4SvtvSzGySAwq4+R/yUCLmUtpWDMm2xdeE6M7T69EhVUWRF4\n"
    "t2v778ydxDZLcXePlfuf/j8Oa6C4bWFMACWz2f+8iAJjxV9rdtB5PTM6fwj125Wt\n"
    "dUN7BnmEhw2GDc1hEvZhs+95QKyatVJeheZ2IB0CgYAfyONzjt3rN+MwzoqbK6zq\n"
    "MJzeQIAZy5qrP4j8WX6kMqc8o74K1XcQ6D2rCXnsl6zI4nZMVC4/OSr7qSi3pqiS\n"
    "KdcRSeK9FiCNJKrHVIFF6ESIZQbu3nRmPbe5ia9UAYPFZjR5cfL86HLURlNDP8Ig\n"
    "pkVV7X2vCKbi7v7voSZAwQ==\n"
    "-----END PRIVATE KEY-----\n";

// Untrusted CA certificate for testing verification failures
// Subject: CN=Untrusted CA
// Valid: 2026-01-22 to 2036-01-20 (self-signed, CA:TRUE)
// Command:
//   openssl req -x509 -newkey rsa:2048 -keyout untrusted_ca_key.pem -out untrusted_ca_cert.pem
//       -days 3650 -nodes -subj "/CN=Untrusted CA"
inline constexpr char const* untrusted_ca_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDDzCCAfegAwIBAgIUc+DM0BNA1pDpUpezmCQiUQoe+3YwDQYJKoZIhvcNAQEL\n"
    "BQAwFzEVMBMGA1UEAwwMVW50cnVzdGVkIENBMB4XDTI2MDEyMjE3MzMzOVoXDTM2\n"
    "MDEyMDE3MzMzOVowFzEVMBMGA1UEAwwMVW50cnVzdGVkIENBMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEA7EyTGB9e6wmVFVwEHJzOni999nFV1sGirC5k\n"
    "cSFUu2Ab853h8wn7tBhzfdiWEIKTpW4evQX0RDEsIUQXLQumjP8G2GOprsi75yVA\n"
    "VHTNZrF6c7zjEahGqW1JX3KlVc88uSZGPOG66JXM3BYlCjY3tBlBHPbySYSzXdNG\n"
    "SpFI5TN/gISgLAnjwMwPG7Jo+DEOGhezHjDmZadL8uUvXOYSbONqyIaMJ67Sh0HM\n"
    "52x/nxkzk6TO/PjfAroXLtki+xD301j5voUTwL3v539hr1dJimqASdUOFmP2NKYB\n"
    "ZICIjoBIx49wSz1ZDtV5FYmZ9O9yOg+98ISK/Tv9PI7oZryEywIDAQABo1MwUTAd\n"
    "BgNVHQ4EFgQUextp3IEu2z5jYyXw3DrYQmThtHowHwYDVR0jBBgwFoAUextp3IEu\n"
    "2z5jYyXw3DrYQmThtHowDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOC\n"
    "AQEARJ1sFG8ceBq2iCCb6ninM+pC/nsxfxktqDxPZgc6Dybb6mTSb3sKwKRH0pTM\n"
    "0z61JbWEVdNpT1tShjnJ5e/YWn90e/8lQBS8LVH/QsfKjGZk5GxUS9186BvAuKQR\n"
    "R668C4CFsxgv0do1Hur8857KvH/z3sruR/ZEgeWTeVqSIxYZaC6HboSoHafq0J/L\n"
    "SCfyoTn+iBxPMdnhwCvpONL8sEkvGW8cYW2URZqFlO/775K+sPbfeYXxuUq/ocEf\n"
    "XmvTRzAeijN/sDeGKVZhi/yGtMv0Q/t0ZwXFU0Mj+fGCmti8QzEFa9RCf+Tx3CiZ\n"
    "zzXbHDJUEOFjKq67XelGy9zeNQ==\n"
    "-----END CERTIFICATE-----\n";

// Full certificate chain: server cert + intermediate cert (for chain tests)
// Contains: chain_server_cert_pem followed by intermediate_cert_pem
// Command:
//   cat chain_server_cert.pem intermediate_cert.pem > server_fullchain.pem
inline constexpr char const* server_fullchain_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDCTCCAfGgAwIBAgIUICKZdMPYLi+vx0rER9U9G0/zzecwDQYJKoZIhvcNAQEL\n"
    "BQAwHzEdMBsGA1UEAwwUVGVzdCBJbnRlcm1lZGlhdGUgQ0EwHhcNMjYwMTIyMTcz\n"
    "MzEyWhcNMjcwMTIyMTczMzEyWjAaMRgwFgYDVQQDDA93d3cuZXhhbXBsZS5jb20w\n"
    "ggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQChfRaitIy/YbFh4Wa0KomP\n"
    "EF8tU3QzyOQ8tD0bxQx8hG6POBEjVh7FUf++n6Sm72UbHGH7txQTNpmoihBp0M1N\n"
    "Bkv85MtaevOkTEGtmY552rHPWezIpOMM6A9Vlu5H6tYs+2zorQJ9VfPt7mGbC56L\n"
    "nOCMEujSwn2B8y0/jh1ZXSe8wGHokBrbigvsJIGNJ1T9HmLf+SaXN4hrLPar8u6S\n"
    "bsDe78l9ZYxyUr8HTAzHuJksxkRbi7z1kQUVKXSg6YoKArHbVVYF8COKRApgTmjY\n"
    "FxIkgpRyYPOnwTQWShzx+Frb0jx1wMagapR07B9Q2Ozk+X2UDPsOj//94J7xJq2f\n"
    "AgMBAAGjQjBAMB0GA1UdDgQWBBQ0aZz4UflELiLyRCbpfJJbn/uFqTAfBgNVHSME\n"
    "GDAWgBRsXLruQi4QIh95qM4gWkjL1gc1ZDANBgkqhkiG9w0BAQsFAAOCAQEAiUKb\n"
    "rDKCzkxU+yT6xG+Dplwhw1218C34QSaMQfx/6qyGYTZfhklqUUeA2sjtBFzFeeWy\n"
    "H7f5eM+i9IBPskd5AJMZpWDv2jA2TgJypvJuTdR3JC0M5bbOLeU57JxLxdizGzAd\n"
    "GR56ERvzeOtHJwnEOsaz8AnSGY3gurAgPI6n9FpQtc25/bhLreknhx5Y0JYaBRPw\n"
    "O98I4pZz0QmtWuaro4LN6vlJf58krvKPKhvuCwEWZvGN7PkC2XbKGf/Xko9/a0Bn\n"
    "l2+4NI2lFdUrd3bperQVMXKm+U3cFHLXm6x+mqUcA5Epz5DUsQZhs18GcsdQh7NG\n"
    "7T5qXswPM7MpHozuTg==\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDJzCCAg+gAwIBAgIUcUBzbbnpdTvewEIRh0FLBDvDDBMwDQYJKoZIhvcNAQEL\n"
    "BQAwFzEVMBMGA1UEAwwMVGVzdCBSb290IENBMB4XDTI2MDcwOTE3MzkyN1oXDTM2\n"
    "MDcwNjE3MzkyN1owHzEdMBsGA1UEAwwUVGVzdCBJbnRlcm1lZGlhdGUgQ0EwggEi\n"
    "MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDCqobUGWRLfletWGsTWGdySYCb\n"
    "l2DJ06wVSW/TXvozFmIMKve4T5LKFDTAQtVrp/hK97HqAlTXWjhMTqq1SYHlN4dv\n"
    "utguzY7Vf96nJWVoJzsq7jAVhukK3bpRo6ytMcj6TRK7DIELKsbCOtvsLTxl0iGk\n"
    "26uE1zn2xk78GXJLRL5QHgeMrkgwWEdY8AeHm9VJ+dxBtnhzPR0z/AFaMmPODMSN\n"
    "+HGkDwVyBxOiPrt9GouEci+rx7AUv3Iv8wLZ+AOiCC0Fbfe9zMqVxVppRB8mUt4c\n"
    "+Np45GnIUk6/Fi+pdNJLTEE5WnoiA87GK+CbAezZt36vYIxSUIfoGz0jKrbpAgMB\n"
    "AAGjYzBhMA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgEGMB0GA1UdDgQW\n"
    "BBRsXLruQi4QIh95qM4gWkjL1gc1ZDAfBgNVHSMEGDAWgBSDM1G+W/ib2OzJ80BV\n"
    "Tfghu+aIxzANBgkqhkiG9w0BAQsFAAOCAQEAc1MilNqUSic4RhznHdFj0fXTPQ0K\n"
    "73WRf/6TmFcoQymlJhMqk2e3NJVxsfW7G/DfR/0Lm2mOn14mDczIBAHFGMdEb4+s\n"
    "iUtu992nroGkxf4euwlwD+LJckWVbnJ1kUhx5WBpFICgW5dvF5KFmYNf9fhLXQs4\n"
    "Ltt/hnNtF4b+vzjBH3xq8LGyQZyt3BkyjLQcHFnPsKepPGW1JFn/2POI2as7WMob\n"
    "vy3qJ9hh52y3K0VO/zxtVef9cpOTHLH1Eo42uHLc5xQMhDN9Kvv1oywthZYyZ+Mc\n"
    "r25U90/0AfnuJjXdCUhj1EMW6QiVk6MQrWhiUXUUKjFUmnChEs++TcL5hw==\n"
    "-----END CERTIFICATE-----\n";

//
// Context Helpers
//

/** Create a context with anonymous ciphers (no certificates needed). */
inline tls_context
make_anon_context()
{
    tls_context ctx;
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::none));
    require_ok(ctx.set_ciphersuites(
        "aNULL:eNULL:@SECLEVEL=0"));
    return ctx;
}

/** Create a server context with test certificate. */
inline tls_context
make_server_context()
{
    tls_context ctx;
    require_ok(ctx.use_certificate(
        server_cert_pem,
        tls_file_format::pem));
    require_ok(ctx.use_private_key(
        server_key_pem,
        tls_file_format::pem));
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::none));
    return ctx;
}

/** Create a client context that trusts the test CA. */
inline tls_context
make_client_context()
{
    tls_context ctx;
    require_ok(ctx.add_certificate_authority(
        ca_cert_pem));
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::peer));
    return ctx;
}

/** Create a client context that trusts the WRONG CA (for failure tests). */
inline tls_context
make_wrong_ca_context()
{
    tls_context ctx;
    require_ok(ctx.add_certificate_authority(
        wrong_ca_cert_pem));
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::peer));
    return ctx;
}

/** server_key_pem encrypted with AES-128-CBC (traditional PEM).
    Passphrase: "test-password"
    Command:
      openssl rsa -traditional -in server_key.pem -aes128 -passout pass:test-password
*/
inline constexpr char const* encrypted_server_key_pem =
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "Proc-Type: 4,ENCRYPTED\n"
    "DEK-Info: AES-128-CBC,8042604E303B1469DCCA597EA6555AA9\n"
    "\n"
    "c6KSpVasopI6/gsa+NOVnOsMGJG8Y1emu3J4aLsCoYkHWQT7axL/ZLTH7AjRn4Sj\n"
    "HD+gP/4QA6UMdVqShesc4zd1bEhq2RV3yir6ast6IfXB3zpq4ggK2cLMYYOo4VwF\n"
    "jKP4XTN8KL3xLQssWdahPsPnkFBR0M/VCo6YCpuZi8zc5iAOHzkGVXf3rCkRORtO\n"
    "5qssbqV33FzwHPeH0PuKWZxcApYfLSRA7vt/bqIxg49WhXGJMhkv4qFpSoaiZ5Tc\n"
    "HlNLfrTbG+A0zUh6f3Oeqm6F95LP7yi2TGxM7oGiJtkwksWoEvS4d9p+NmRNKr0i\n"
    "gZJjOUKq9oxPzFdcRS1H7AkDjraw3q1NTxrcFlXbjtxDm9t6GkLuwXfUz0n31IL9\n"
    "/jQX3/N3eVKp8mHMRWmdtllmK0/XeYBH5A4MKpBz4/smuDoStMsmezP4+0qYyE2H\n"
    "YUHaTlTrJQBG/BpIJmJk7BfzqGoTg3dgSNxMc1gYpCwcK0X/w+6HzEOtMjGWGhv1\n"
    "Cbq03wMH8BLdIFMNRpnN0cLUBJItnn58AxDg0JsIii7jxEpAVVpl3tTC5Lk3q97k\n"
    "15fpGhMKya8iZ0EFs6jdjuJoGNl7+KnMoet0cQ1CyV5uSzus6qBdz6ATHsF8KTRj\n"
    "xNCXc1cbEt8A2soazFBUVeN1MQFt5yTKLCEz1Sb4M9Wn3IT07JUpWjGZLIrz9KWe\n"
    "JU2oKjSxQmKbAPmpRRe84MR5O8qCytCAstR/GG+qU1HZsW/bgB/2RTY7kcTeOpke\n"
    "/jw2iZUygNkXgCiik+LqwAbchkVw6ImRYCTdaXfq+bFwV+2JBUMfotCgxawFVaDc\n"
    "EdEF18g01II/xxr9HGA6hCJioP4curRiJNeqqs0S+4nJAxv8IiesdbmzbxBzAPql\n"
    "bzcfmrMH2qrDEJg1NiPaMODwfcdpgfjE5yMewI+nCRuOWSVPzk092zmPI9qv1xFK\n"
    "B+76CzH88yGESip3x2Zsce1FB/HV2CBcvzjIkkMtHdFzpXidg0bS7vDG4WTyGdti\n"
    "9z3K31rD0qi6IRtQlLiHZIaKhSMNqPnfePRCru0S4rHWGcVtdrsKaHL201meLSPi\n"
    "Iw82DRbNVs/lLPGtMJ0DzKbJvTMWFoC0zZ/nIaRa2hi7nd/Ig0oD19cVIkB4IWPp\n"
    "tuR53NZs9EsEqjNnNmn5ftdTAO8P50EulaaPjJggIF59C9u74hhgI+UvMrnOhq0C\n"
    "nlyfUgl6nS0WgBc+rtCdB59xppPobefyZ06Rud4QFJmz/wdazXzuimPCkpNDv/8t\n"
    "lbS952xleT2dYawtho4K+ZMV6q2SFfjAXZjhy/fbVP1vqqXOZPeEC4n070ITifHO\n"
    "GN/5evJpcYxohXB2paKkwHWeN9oaolBXBpW5zZKNacEcu/uFWjN8UYuxO/9/Wls7\n"
    "yimyYsmoYtbxVTH5H+UE4TlaIhGXy46VzYRkEYx43m7laIPdyyctQV4cb7Xec9ZS\n"
    "o/UWTdrl/e0k0UoftWRH4a5RCubDQ21khoyTabVSfiJY48GEAUExGItiG6C7KZ/S\n"
    "xXW6nBG4TmkXJYEwVNJfVygvVDjFPrHtKhFLHs7nsigDK0jNvTbAXNhhCrSMkLC8\n"
    "-----END RSA PRIVATE KEY-----\n";

/// Passphrase matching `encrypted_server_key_pem`.
inline constexpr char const* encrypted_key_password = "test-password";

/** server_cert_pem + server_key_pem exported as a PKCS#12 bundle.
    Passphrase: "test-password". Generated with:
      openssl pkcs12 -export -inkey server.key -in server.crt \\
        -keypbe AES-256-CBC -certpbe AES-256-CBC -macalg sha256
    (PBES2/AES-256-CBC; 3DES is disabled in the test wolfSSL build)
*/
inline constexpr unsigned char server_p12[] = {
    0x30, 0x82, 0x0a, 0x87, 0x02, 0x01, 0x03, 0x30, 0x82, 0x0a, 0x35, 0x06,
    0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x01, 0xa0, 0x82,
    0x0a, 0x26, 0x04, 0x82, 0x0a, 0x22, 0x30, 0x82, 0x0a, 0x1e, 0x30, 0x82,
    0x04, 0x8a, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07,
    0x06, 0xa0, 0x82, 0x04, 0x7b, 0x30, 0x82, 0x04, 0x77, 0x02, 0x01, 0x00,
    0x30, 0x82, 0x04, 0x70, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
    0x01, 0x07, 0x01, 0x30, 0x5f, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7,
    0x0d, 0x01, 0x05, 0x0d, 0x30, 0x52, 0x30, 0x31, 0x06, 0x09, 0x2a, 0x86,
    0x48, 0x86, 0xf7, 0x0d, 0x01, 0x05, 0x0c, 0x30, 0x24, 0x04, 0x10, 0xd7,
    0x31, 0x9c, 0xcd, 0x95, 0x22, 0xd6, 0x91, 0x14, 0xbd, 0x9e, 0xbe, 0x88,
    0x5f, 0x0d, 0xad, 0x02, 0x02, 0x08, 0x00, 0x30, 0x0c, 0x06, 0x08, 0x2a,
    0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x09, 0x05, 0x00, 0x30, 0x1d, 0x06,
    0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x01, 0x2a, 0x04, 0x10,
    0xe5, 0x84, 0x61, 0xf8, 0x7b, 0xde, 0xa8, 0x89, 0x02, 0xde, 0x22, 0x49,
    0xc8, 0x5b, 0xe4, 0x72, 0x80, 0x82, 0x04, 0x00, 0xaf, 0x0c, 0x6e, 0xfd,
    0x77, 0xb4, 0xbb, 0x12, 0x7f, 0x8b, 0x79, 0xd6, 0x17, 0x2c, 0xd6, 0xd0,
    0x5e, 0x60, 0x29, 0xbc, 0xcf, 0xcf, 0x43, 0x63, 0x4a, 0x3b, 0x6e, 0xa8,
    0x0e, 0x85, 0x66, 0xca, 0xc6, 0x02, 0xe2, 0x20, 0x1a, 0xd7, 0x21, 0xd0,
    0xb1, 0x17, 0x74, 0xec, 0xe1, 0x53, 0xd9, 0x1f, 0xb2, 0xae, 0xeb, 0x2e,
    0x94, 0x01, 0xd6, 0x46, 0x7f, 0x5f, 0x21, 0x8e, 0x1f, 0x9f, 0x21, 0x6d,
    0x6c, 0xca, 0x4f, 0x77, 0x2a, 0xe0, 0x4d, 0x8b, 0xf0, 0x1a, 0x65, 0x00,
    0xe0, 0x23, 0x65, 0x7b, 0x82, 0x53, 0xd8, 0x00, 0x6d, 0x20, 0xe9, 0x78,
    0xe4, 0x54, 0x06, 0xcd, 0xde, 0x2f, 0x39, 0xe8, 0x2d, 0x97, 0x81, 0xee,
    0x8e, 0x5b, 0x81, 0x40, 0x77, 0x4d, 0x8a, 0x88, 0x8a, 0xcc, 0xf0, 0x20,
    0x1c, 0xae, 0x92, 0x9a, 0xac, 0xf7, 0x28, 0x77, 0xcf, 0x89, 0x10, 0x2f,
    0x1b, 0x2c, 0x46, 0x96, 0x2b, 0xf2, 0x13, 0x65, 0xdf, 0x7c, 0x73, 0xbd,
    0x01, 0x87, 0xc3, 0x22, 0xca, 0x41, 0xa4, 0xc0, 0x9a, 0xe5, 0xab, 0x27,
    0x28, 0xc5, 0xcd, 0x21, 0x2e, 0xa6, 0xcd, 0x69, 0x76, 0x5e, 0x35, 0xee,
    0x8b, 0xc4, 0xfb, 0x31, 0xaa, 0x76, 0x9a, 0x1d, 0x5b, 0x30, 0x25, 0xfb,
    0x46, 0x47, 0xd8, 0x6d, 0x99, 0x2c, 0x4e, 0xc3, 0x6c, 0x42, 0x97, 0xcf,
    0x16, 0x06, 0x07, 0xc5, 0x81, 0xc5, 0x17, 0xab, 0x7e, 0xd2, 0x39, 0x9b,
    0xc2, 0xe7, 0x25, 0x30, 0x22, 0x6a, 0xe3, 0xa6, 0x1e, 0x93, 0x62, 0x60,
    0xc2, 0x22, 0x7a, 0xcd, 0x23, 0xe1, 0x05, 0x71, 0xd3, 0x5b, 0xcb, 0xc1,
    0xd5, 0x71, 0xdf, 0x9b, 0x70, 0x62, 0x61, 0x18, 0xef, 0x6b, 0x17, 0x68,
    0x96, 0x9f, 0x29, 0x2a, 0xf1, 0x13, 0xc1, 0xa0, 0x75, 0xb3, 0x7a, 0x39,
    0x04, 0x32, 0xa9, 0xc3, 0xe1, 0xfd, 0x9c, 0x2d, 0x76, 0x95, 0x49, 0x86,
    0x92, 0x6e, 0x1a, 0xf9, 0x26, 0x9d, 0x09, 0xb6, 0x51, 0xd3, 0x96, 0xf7,
    0x70, 0x9e, 0x23, 0x48, 0x10, 0x30, 0x2a, 0xd9, 0xb6, 0x3d, 0xe0, 0x3f,
    0x93, 0x21, 0xfd, 0x20, 0xff, 0xf6, 0xeb, 0x45, 0xb3, 0x9a, 0xe3, 0x2d,
    0x3d, 0xe9, 0x57, 0xb9, 0xab, 0xc5, 0xb7, 0x53, 0x49, 0x48, 0x31, 0x09,
    0x4b, 0x0c, 0x20, 0x0b, 0x20, 0x7f, 0x43, 0x3b, 0xc7, 0x04, 0x35, 0xcf,
    0x08, 0xa4, 0x4d, 0x6b, 0xff, 0x0a, 0xa9, 0xaa, 0xa2, 0x72, 0x1c, 0x5d,
    0x53, 0xde, 0xe6, 0x4b, 0x9b, 0xc9, 0x56, 0x56, 0x8b, 0x3a, 0xec, 0x4d,
    0xae, 0x4f, 0xef, 0x31, 0x3c, 0x2f, 0x42, 0xf2, 0xb7, 0x23, 0xb3, 0x50,
    0x3d, 0xb6, 0xcb, 0xb3, 0xe9, 0x56, 0x7a, 0xca, 0xbb, 0x68, 0xbe, 0xec,
    0xb7, 0xd8, 0x82, 0x62, 0x3a, 0x30, 0xc2, 0xfa, 0xee, 0x1f, 0xbb, 0x86,
    0x91, 0x93, 0x71, 0x05, 0x72, 0x96, 0xf5, 0x11, 0xd7, 0x3f, 0xad, 0x7c,
    0x5f, 0xb7, 0x69, 0xe0, 0x4a, 0xbf, 0x1f, 0x7b, 0x44, 0x2b, 0x51, 0x21,
    0xda, 0x72, 0x23, 0xbc, 0x46, 0x7f, 0x02, 0x40, 0x5b, 0xef, 0xcc, 0x7c,
    0x8a, 0x33, 0xd5, 0xe8, 0x52, 0x1a, 0x9a, 0xe1, 0x13, 0x3c, 0x56, 0xa5,
    0xe7, 0x77, 0x22, 0x72, 0x36, 0x7f, 0x57, 0x57, 0xe5, 0x32, 0xf6, 0x2a,
    0xf7, 0xab, 0xf9, 0xfa, 0x87, 0xe9, 0x20, 0xe6, 0xe3, 0xed, 0xcb, 0x3e,
    0x9b, 0xf0, 0xa7, 0x99, 0x06, 0xd7, 0xaa, 0x3d, 0x3a, 0x80, 0xf2, 0xd5,
    0x7b, 0x37, 0xc4, 0xbc, 0x26, 0x97, 0x72, 0x78, 0x56, 0x37, 0xa2, 0xd6,
    0x90, 0x37, 0xcd, 0xf9, 0x78, 0xd8, 0x8d, 0xf8, 0x10, 0x30, 0xd7, 0x61,
    0xe9, 0x5c, 0x00, 0xc1, 0x8a, 0x11, 0x88, 0xf2, 0x4e, 0xab, 0xce, 0x3f,
    0x55, 0x36, 0xfe, 0xd0, 0xa9, 0xf9, 0x6a, 0xcd, 0xed, 0xf3, 0xdc, 0x94,
    0x33, 0x0d, 0x10, 0x45, 0x3f, 0x54, 0x5c, 0x75, 0xfb, 0x05, 0x48, 0x27,
    0x4e, 0x2e, 0x75, 0xe3, 0x56, 0xd4, 0x92, 0xc9, 0x79, 0xb3, 0xb3, 0x93,
    0x6d, 0x89, 0x68, 0x85, 0xff, 0xee, 0x59, 0x76, 0xa5, 0x4e, 0xb6, 0x0b,
    0xa8, 0xfb, 0x23, 0x84, 0xdb, 0x3b, 0x53, 0x4a, 0xd5, 0xb7, 0x30, 0x4d,
    0x2a, 0x75, 0xb2, 0x71, 0xf4, 0xda, 0x73, 0x27, 0x56, 0x98, 0xcb, 0xe5,
    0x03, 0x6f, 0x43, 0xb4, 0x65, 0xbb, 0xb3, 0x0f, 0xc0, 0xfc, 0xdd, 0x45,
    0x7e, 0x99, 0x75, 0x16, 0xb6, 0x44, 0xfb, 0x0b, 0x1d, 0xe5, 0xb8, 0x44,
    0xe8, 0x87, 0xed, 0xed, 0x6d, 0x21, 0x8c, 0xa5, 0x55, 0xd2, 0xb5, 0xb2,
    0x36, 0xbc, 0xa3, 0x93, 0x5d, 0x6f, 0x73, 0x11, 0xba, 0xda, 0x75, 0xb9,
    0x98, 0x20, 0xef, 0x94, 0x3d, 0xf4, 0x08, 0x01, 0xab, 0xd7, 0xae, 0xe2,
    0x86, 0xd1, 0xe5, 0x64, 0xa8, 0x5b, 0x14, 0x99, 0x01, 0x8b, 0xa3, 0x09,
    0xa9, 0x1e, 0x44, 0x1c, 0xdc, 0x46, 0x33, 0xe7, 0xe5, 0x8a, 0x70, 0xb5,
    0xea, 0x1f, 0x4c, 0x21, 0x18, 0xfe, 0x75, 0xc9, 0x0b, 0x7a, 0xd1, 0x4c,
    0x94, 0x2d, 0x36, 0xf9, 0xe7, 0xe5, 0x67, 0x24, 0xa3, 0x43, 0xcc, 0x51,
    0xa0, 0xcb, 0x33, 0x9b, 0x36, 0x03, 0x1b, 0xdb, 0x91, 0xd7, 0xd8, 0xea,
    0x4d, 0x0a, 0x7b, 0x27, 0x6f, 0xee, 0x11, 0xc3, 0xc3, 0x52, 0x9c, 0x91,
    0x7b, 0x56, 0x10, 0x04, 0x70, 0xd7, 0x1f, 0x68, 0xfb, 0xa0, 0x53, 0x85,
    0x87, 0x21, 0x7a, 0xe9, 0xaf, 0x50, 0xe4, 0x80, 0x37, 0xe9, 0x12, 0x5c,
    0x69, 0x96, 0xbd, 0xe6, 0x69, 0xb8, 0x79, 0x64, 0x6f, 0x97, 0xd4, 0x0c,
    0x54, 0x45, 0xce, 0xee, 0x45, 0x07, 0xdf, 0x76, 0x44, 0xa0, 0xb7, 0x34,
    0x8d, 0xfc, 0x2b, 0x3d, 0xe1, 0x40, 0xa2, 0xf3, 0xeb, 0xb6, 0x22, 0x7f,
    0x76, 0x9f, 0x72, 0x01, 0x59, 0xee, 0x99, 0x17, 0x2b, 0xbf, 0x7f, 0x29,
    0x62, 0x87, 0x25, 0x50, 0x12, 0x7a, 0x86, 0x55, 0x2a, 0x74, 0xf6, 0x1f,
    0x39, 0xc6, 0xb8, 0x4b, 0xac, 0x5a, 0x63, 0x7a, 0xa9, 0xe3, 0x8c, 0xdd,
    0x70, 0xc0, 0xb4, 0x5d, 0x88, 0xa8, 0x8e, 0xc1, 0xc0, 0xa8, 0xf3, 0xf6,
    0x91, 0x9f, 0xe4, 0x64, 0x3f, 0x79, 0x29, 0xc1, 0xfc, 0x4c, 0x31, 0xb1,
    0x5b, 0x30, 0x61, 0x7e, 0xc6, 0x68, 0xc5, 0x27, 0x5e, 0xb5, 0x83, 0xd2,
    0x58, 0x75, 0x43, 0xb3, 0x75, 0x4a, 0x0e, 0xa8, 0xf2, 0xd8, 0x80, 0x6d,
    0x31, 0x9e, 0x48, 0x3f, 0x09, 0x39, 0xf8, 0xc0, 0x97, 0x4b, 0x4e, 0xa6,
    0xad, 0xcc, 0x18, 0xee, 0x44, 0x0d, 0xe8, 0x5e, 0xbd, 0xb7, 0x34, 0x65,
    0x60, 0x64, 0x36, 0xb3, 0xb9, 0xa2, 0x50, 0xd8, 0x34, 0x47, 0x39, 0xbe,
    0x0d, 0x80, 0x24, 0x2d, 0x75, 0x73, 0x32, 0x4f, 0x10, 0x6c, 0x59, 0xe0,
    0xb0, 0x95, 0x3a, 0x96, 0x86, 0x6e, 0xf5, 0x74, 0xb5, 0x78, 0x97, 0x53,
    0x87, 0xf8, 0x9e, 0x3f, 0xa5, 0x1e, 0x22, 0xd5, 0xa5, 0xf7, 0x78, 0xb2,
    0xbb, 0x22, 0x1d, 0xc6, 0x70, 0x79, 0x26, 0x97, 0x18, 0x58, 0x80, 0x1c,
    0x43, 0xc8, 0xae, 0x44, 0xbf, 0x09, 0x83, 0x30, 0xec, 0x0f, 0xb1, 0x86,
    0x3b, 0x3e, 0x1a, 0x4f, 0x4e, 0x8e, 0xa6, 0x23, 0xf7, 0x9a, 0xfa, 0x4e,
    0x42, 0xcb, 0x02, 0x1f, 0xc8, 0x0a, 0xfa, 0xe5, 0x7c, 0x1c, 0xda, 0x68,
    0xb2, 0x9c, 0x99, 0x68, 0x5d, 0x70, 0x71, 0x24, 0xb1, 0x1f, 0x29, 0xba,
    0xc4, 0x81, 0x01, 0x78, 0x77, 0x06, 0x46, 0xdd, 0x54, 0x53, 0xae, 0x13,
    0xd6, 0x50, 0xaf, 0x48, 0xfd, 0x57, 0x98, 0xae, 0x1f, 0xff, 0x34, 0x91,
    0xe8, 0xfd, 0x3f, 0xbf, 0x7c, 0x99, 0x78, 0x48, 0x9a, 0xe4, 0x9a, 0xea,
    0x78, 0xf7, 0x94, 0x42, 0x0b, 0x7a, 0x63, 0x06, 0xa2, 0x25, 0xc4, 0x63,
    0x30, 0x82, 0x05, 0x8c, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
    0x01, 0x07, 0x01, 0xa0, 0x82, 0x05, 0x7d, 0x04, 0x82, 0x05, 0x79, 0x30,
    0x82, 0x05, 0x75, 0x30, 0x82, 0x05, 0x71, 0x06, 0x0b, 0x2a, 0x86, 0x48,
    0x86, 0xf7, 0x0d, 0x01, 0x0c, 0x0a, 0x01, 0x02, 0xa0, 0x82, 0x05, 0x39,
    0x30, 0x82, 0x05, 0x35, 0x30, 0x5f, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
    0xf7, 0x0d, 0x01, 0x05, 0x0d, 0x30, 0x52, 0x30, 0x31, 0x06, 0x09, 0x2a,
    0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x05, 0x0c, 0x30, 0x24, 0x04, 0x10,
    0x5e, 0x1a, 0x19, 0x51, 0xeb, 0x2e, 0x9b, 0xf2, 0xdd, 0xfe, 0x32, 0x0b,
    0x8e, 0x13, 0xa4, 0xac, 0x02, 0x02, 0x08, 0x00, 0x30, 0x0c, 0x06, 0x08,
    0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x09, 0x05, 0x00, 0x30, 0x1d,
    0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x01, 0x2a, 0x04,
    0x10, 0x2e, 0x39, 0xe8, 0x93, 0xe5, 0x92, 0x85, 0x7f, 0x21, 0xa3, 0xf4,
    0x89, 0xda, 0x51, 0xf9, 0xcf, 0x04, 0x82, 0x04, 0xd0, 0x37, 0x51, 0x2f,
    0x8d, 0xd3, 0x48, 0x0f, 0x17, 0xe1, 0x8a, 0x4b, 0xb9, 0xc2, 0x49, 0x0c,
    0x60, 0x81, 0x0d, 0x0a, 0x2e, 0x4a, 0x44, 0x93, 0x62, 0x9b, 0x24, 0xc8,
    0x81, 0x2f, 0x8d, 0x91, 0xce, 0x98, 0x4a, 0xcd, 0x07, 0xd6, 0xd5, 0xb9,
    0x89, 0xad, 0xdf, 0xc2, 0x7d, 0x29, 0x43, 0x5f, 0xb8, 0x65, 0x2f, 0x18,
    0x51, 0x2c, 0x5f, 0xab, 0xe3, 0xa9, 0x0d, 0x02, 0xf5, 0xd5, 0x9a, 0x0e,
    0x4c, 0x3d, 0x84, 0x4a, 0x92, 0x61, 0xb8, 0x34, 0xfd, 0xe9, 0x1a, 0xd6,
    0xeb, 0xf1, 0x15, 0x90, 0x43, 0x71, 0xd0, 0xdf, 0x61, 0x09, 0xa3, 0x24,
    0x1f, 0x0c, 0x04, 0xb1, 0x8e, 0x4a, 0x9b, 0xe9, 0x35, 0xf9, 0x9a, 0x84,
    0xd6, 0xaf, 0xc6, 0xb3, 0x1c, 0x7c, 0xa9, 0xb9, 0x44, 0x09, 0xc9, 0x45,
    0xe0, 0xb7, 0x4a, 0x6d, 0x27, 0x68, 0x0a, 0xd8, 0x95, 0x1f, 0x4b, 0x4d,
    0x7f, 0x4a, 0x18, 0xd0, 0x1d, 0x5a, 0x46, 0xd5, 0xcb, 0x7e, 0xfd, 0x6d,
    0xf2, 0x9f, 0x3b, 0xe0, 0xe0, 0x71, 0xa3, 0xbf, 0xd1, 0x5b, 0xff, 0xd5,
    0x3d, 0xc2, 0xda, 0xcd, 0x27, 0xa2, 0x90, 0x5b, 0x13, 0xa9, 0x99, 0x4e,
    0xd1, 0x60, 0x85, 0x14, 0x09, 0x26, 0x5d, 0x9e, 0x52, 0x4a, 0x7e, 0x2b,
    0x98, 0x2b, 0xf4, 0x8b, 0xd4, 0x35, 0xdb, 0xd8, 0xe4, 0xa8, 0x51, 0xc4,
    0xd7, 0xf7, 0x3a, 0xad, 0xe7, 0x80, 0xa6, 0xe1, 0x44, 0xe9, 0x95, 0x6e,
    0x3e, 0xc5, 0x69, 0x95, 0x79, 0xb7, 0x02, 0x02, 0x59, 0x29, 0xa4, 0x90,
    0x82, 0x7e, 0xb3, 0x14, 0x28, 0x47, 0xab, 0x2e, 0xdd, 0x5e, 0x43, 0xa6,
    0xd8, 0xec, 0x80, 0xe4, 0x0e, 0x0c, 0x0d, 0x2e, 0xc7, 0xc9, 0xdb, 0xbb,
    0x23, 0xe2, 0xa9, 0x3a, 0x0d, 0x7e, 0xb3, 0x77, 0x12, 0xf0, 0x18, 0xbd,
    0x41, 0x1c, 0x96, 0x43, 0xf2, 0xb6, 0x3a, 0xc0, 0xd2, 0x90, 0x5d, 0x7f,
    0x4d, 0xa4, 0xda, 0x50, 0x28, 0x60, 0x62, 0x36, 0x05, 0xc6, 0xa1, 0x2f,
    0xd6, 0x91, 0xcf, 0x4a, 0xc4, 0x0b, 0x33, 0xd4, 0x33, 0xd2, 0x85, 0xae,
    0xbd, 0xb3, 0xc2, 0x21, 0x2e, 0x74, 0x67, 0x62, 0x01, 0x91, 0xbf, 0xaf,
    0x1a, 0xa6, 0x52, 0x8f, 0xe1, 0x5c, 0x2c, 0xf9, 0x34, 0x98, 0x7f, 0xcc,
    0x9f, 0x6c, 0xf1, 0x1b, 0x0f, 0x8f, 0x99, 0xf1, 0x91, 0x34, 0xa4, 0x11,
    0xf3, 0x86, 0x23, 0xe8, 0x95, 0x6a, 0x07, 0xad, 0x52, 0x2e, 0xec, 0xdd,
    0x81, 0xd9, 0xb5, 0x49, 0x9c, 0x1d, 0x3e, 0x89, 0x88, 0x90, 0x56, 0x7f,
    0xc1, 0xff, 0x46, 0xf3, 0x8f, 0xf6, 0xbd, 0x0d, 0x57, 0x9c, 0x2a, 0xad,
    0x86, 0xb3, 0x81, 0x45, 0x13, 0x1f, 0x11, 0x61, 0xe0, 0x45, 0xa4, 0x55,
    0x68, 0xb7, 0x6c, 0x48, 0x94, 0x05, 0xeb, 0x2e, 0x26, 0x4f, 0x41, 0x6a,
    0x9a, 0x28, 0x7d, 0x43, 0xb5, 0xcb, 0x78, 0x32, 0xf2, 0xf8, 0xb8, 0x05,
    0x85, 0x39, 0x24, 0xc7, 0x89, 0x04, 0x77, 0x23, 0xe3, 0x5d, 0x1f, 0x78,
    0x98, 0x09, 0xab, 0x8a, 0xbc, 0x2b, 0x8a, 0xf2, 0xc6, 0xd2, 0x85, 0x6e,
    0xb8, 0xe6, 0xb7, 0xb3, 0x5c, 0x0c, 0x2f, 0xe4, 0xdd, 0x2c, 0xab, 0xf2,
    0xf5, 0x48, 0x8e, 0xd4, 0xc1, 0x5b, 0xe9, 0x2b, 0xdb, 0x71, 0x7f, 0xc2,
    0xc0, 0x10, 0x96, 0x8a, 0x10, 0xd3, 0x5c, 0x1d, 0xf6, 0x26, 0xca, 0xcc,
    0x11, 0xa4, 0x23, 0xdc, 0x30, 0x48, 0x48, 0x73, 0x5d, 0x8a, 0x14, 0x09,
    0x3a, 0x8f, 0x89, 0x1d, 0x4f, 0xd5, 0x96, 0x87, 0x5e, 0xc7, 0x38, 0x63,
    0x40, 0x9b, 0x8d, 0x14, 0xc0, 0x3e, 0xd8, 0xeb, 0xf7, 0xc3, 0x2e, 0x88,
    0x90, 0x62, 0x68, 0x77, 0x72, 0xed, 0x35, 0xda, 0xf4, 0xed, 0xb0, 0x19,
    0xe1, 0xcb, 0x4c, 0x6a, 0x4a, 0x99, 0x88, 0x81, 0xcd, 0xd2, 0x0c, 0x24,
    0x32, 0x02, 0x72, 0x95, 0x86, 0xa8, 0x0d, 0xaa, 0x19, 0x33, 0x91, 0xeb,
    0xd0, 0xf2, 0x0f, 0x33, 0xd7, 0x85, 0xcf, 0x56, 0x58, 0x0e, 0x0b, 0xb4,
    0x6a, 0xb0, 0xdd, 0x0a, 0xed, 0x51, 0xb1, 0xa1, 0xbb, 0x9c, 0x3b, 0xf4,
    0x50, 0xea, 0x77, 0x34, 0x66, 0x28, 0x1f, 0xfc, 0xa7, 0xad, 0xb0, 0x50,
    0x29, 0x8d, 0x81, 0xf6, 0xd7, 0xbb, 0x21, 0xa8, 0x88, 0x98, 0xae, 0x5e,
    0x8b, 0xea, 0xc9, 0xc9, 0x50, 0xfb, 0xce, 0x81, 0x5d, 0xbc, 0x08, 0x87,
    0x8b, 0x04, 0x2a, 0x8a, 0xd1, 0x64, 0x08, 0x47, 0x5b, 0xeb, 0x72, 0xc5,
    0x1f, 0x90, 0x17, 0x3a, 0xb2, 0xb0, 0x01, 0x5a, 0x02, 0x57, 0xf4, 0x4f,
    0xdd, 0x40, 0xd0, 0xae, 0x2f, 0xd0, 0x58, 0x98, 0xa8, 0xa0, 0xb8, 0x37,
    0x0a, 0x74, 0x2f, 0x1a, 0x1a, 0x71, 0x93, 0xaf, 0xb2, 0xfa, 0xe1, 0x03,
    0x61, 0xb4, 0x59, 0x4a, 0xaa, 0x05, 0xf7, 0x30, 0xc9, 0xa0, 0x32, 0xdf,
    0x34, 0xb0, 0x16, 0xa6, 0xfc, 0x6a, 0x2b, 0x73, 0x50, 0x00, 0x1f, 0xf8,
    0x20, 0x09, 0x94, 0x55, 0xd9, 0x01, 0xa7, 0x55, 0x15, 0x32, 0xdb, 0x92,
    0xf4, 0x65, 0x02, 0xb5, 0x1e, 0x87, 0xde, 0x5c, 0xc1, 0x9e, 0x4b, 0xe3,
    0xa4, 0xc6, 0x61, 0xe2, 0x09, 0x6e, 0x9e, 0x60, 0x09, 0xfc, 0x1f, 0x88,
    0xb6, 0x8c, 0xc3, 0x44, 0x3a, 0xd0, 0x57, 0x82, 0xdc, 0x44, 0x62, 0x71,
    0xc2, 0x19, 0xf1, 0xf9, 0x3b, 0x06, 0xfd, 0xcd, 0xde, 0x87, 0xb7, 0xf8,
    0xfe, 0x36, 0xad, 0x56, 0x81, 0x7c, 0xfc, 0x72, 0x95, 0xda, 0xbe, 0xa4,
    0xfe, 0x5c, 0x20, 0xca, 0xb4, 0x11, 0xa4, 0xb1, 0x4a, 0x53, 0x60, 0x6d,
    0xd8, 0xc7, 0x08, 0xf0, 0xf2, 0x3a, 0xc4, 0x1d, 0xfa, 0xed, 0x58, 0x6e,
    0x5e, 0xd7, 0x61, 0xfb, 0xd7, 0xa6, 0x41, 0x51, 0x88, 0x69, 0xb5, 0x33,
    0xcd, 0x6a, 0x7a, 0x09, 0x9e, 0x2d, 0xce, 0x39, 0xe9, 0xd7, 0xf3, 0x40,
    0x6f, 0x18, 0x9c, 0x00, 0xbb, 0xb8, 0xaa, 0x96, 0x80, 0x41, 0x32, 0x86,
    0x85, 0xf0, 0xe2, 0xfc, 0x5d, 0x94, 0xdf, 0xf6, 0x83, 0xcc, 0x4b, 0xf9,
    0x98, 0xb2, 0x41, 0x06, 0xff, 0x2d, 0x40, 0xd8, 0x1a, 0x10, 0x55, 0xe6,
    0x24, 0x77, 0x82, 0x6f, 0xe9, 0x42, 0x5a, 0x5a, 0x94, 0x0b, 0xf1, 0xd9,
    0xf7, 0x47, 0x65, 0xae, 0x08, 0xda, 0x3b, 0x02, 0x8b, 0xdb, 0xbd, 0x03,
    0x21, 0x90, 0x56, 0x74, 0x11, 0xaa, 0x7e, 0x4f, 0x3a, 0xe0, 0x83, 0xbe,
    0x21, 0x40, 0x40, 0x5f, 0xad, 0x39, 0x94, 0xa7, 0x13, 0xfc, 0xbd, 0xc5,
    0x3b, 0x7a, 0x08, 0xbe, 0xa9, 0x68, 0xd9, 0x1d, 0x0e, 0x9c, 0xa7, 0x5b,
    0x9d, 0x3c, 0xb0, 0x41, 0xde, 0x9c, 0xf6, 0x6e, 0x8c, 0x04, 0x34, 0xed,
    0xfc, 0x8e, 0x55, 0xd3, 0xc5, 0x86, 0x78, 0xd2, 0x25, 0x7b, 0xaf, 0x1d,
    0xde, 0x5c, 0x80, 0x27, 0x27, 0x63, 0x16, 0x29, 0xa5, 0xc4, 0x3a, 0xd7,
    0xec, 0x85, 0x39, 0x37, 0xc0, 0x16, 0xae, 0x2b, 0x2d, 0x42, 0x7f, 0x72,
    0x73, 0x54, 0x23, 0xca, 0x1d, 0xa3, 0x92, 0x52, 0xec, 0x62, 0x1c, 0x43,
    0x34, 0x79, 0x19, 0x5a, 0x67, 0x07, 0x9f, 0x9a, 0xac, 0x5a, 0xf2, 0x92,
    0x16, 0x0a, 0x6e, 0xd9, 0x2a, 0x98, 0x66, 0x7c, 0x59, 0x66, 0xac, 0x5f,
    0x57, 0xb9, 0x5d, 0x2e, 0x33, 0xb3, 0x74, 0xcc, 0x74, 0x7e, 0x81, 0xfb,
    0x06, 0x6d, 0x7b, 0x47, 0xb2, 0xe0, 0x6f, 0x7d, 0xd4, 0x8a, 0x92, 0x13,
    0xfe, 0x9d, 0x5d, 0x56, 0x5a, 0x77, 0x29, 0x6e, 0x12, 0x4e, 0xa6, 0x47,
    0x8c, 0x75, 0xbe, 0x56, 0x27, 0x51, 0x41, 0xf2, 0x03, 0x4e, 0xb0, 0xae,
    0x9a, 0x9e, 0x5e, 0x2f, 0x2e, 0x4f, 0x8d, 0xa7, 0x45, 0x4b, 0x8c, 0xc5,
    0xaa, 0x4c, 0x14, 0x3f, 0x57, 0x74, 0x3f, 0xb8, 0xe0, 0xdc, 0x8e, 0xfd,
    0x70, 0x78, 0xf4, 0x50, 0xf3, 0x23, 0x3e, 0x6c, 0xcd, 0x98, 0x2c, 0x24,
    0x39, 0x4a, 0x8b, 0xea, 0x91, 0x67, 0xed, 0x84, 0xbd, 0x0e, 0x54, 0xdb,
    0xbd, 0x67, 0x52, 0x88, 0x63, 0x31, 0xef, 0x7c, 0x3d, 0xab, 0x61, 0xc2,
    0xfd, 0xcd, 0xb3, 0xf5, 0x33, 0xd8, 0xbd, 0xcd, 0x25, 0x1d, 0x93, 0x7f,
    0x4c, 0x41, 0xf2, 0x08, 0xc4, 0x97, 0xb6, 0xd3, 0x02, 0xc2, 0x1d, 0xc9,
    0x18, 0x4e, 0x3f, 0x84, 0xf7, 0x9a, 0xe2, 0x93, 0x08, 0x62, 0x0e, 0x19,
    0xc7, 0x6c, 0xb0, 0x53, 0x07, 0x2a, 0x22, 0xc3, 0x05, 0xcc, 0x11, 0xb0,
    0x97, 0x08, 0x98, 0x69, 0xe4, 0xfb, 0xb1, 0xe5, 0x45, 0xe2, 0xda, 0x14,
    0x6f, 0x60, 0xaa, 0xc1, 0x18, 0xb3, 0x1a, 0x78, 0x85, 0xbd, 0x70, 0x43,
    0x4a, 0xb3, 0x39, 0x0e, 0x38, 0x7b, 0x4c, 0xb9, 0x5b, 0x02, 0xbf, 0xda,
    0x65, 0xb7, 0xfb, 0x3d, 0xab, 0xde, 0x95, 0xb8, 0xea, 0xfe, 0xff, 0x8a,
    0xfa, 0x93, 0x11, 0x00, 0xf1, 0x2e, 0xa7, 0xb2, 0x34, 0x06, 0xdd, 0xdc,
    0xfc, 0x6c, 0xba, 0x38, 0x48, 0xde, 0xc8, 0xd0, 0x65, 0x22, 0x50, 0xf3,
    0x26, 0x87, 0x66, 0x2c, 0x3f, 0x14, 0x29, 0x50, 0x03, 0xb1, 0x05, 0x6a,
    0x3d, 0xa6, 0x9e, 0x9e, 0xbd, 0x5b, 0xf7, 0x20, 0x80, 0x5e, 0x52, 0x32,
    0xc9, 0x41, 0xb5, 0x6c, 0xf4, 0x4e, 0x16, 0xb5, 0x23, 0xd6, 0xa2, 0xbb,
    0xfc, 0x3f, 0xd5, 0x5a, 0x3f, 0x23, 0xa7, 0xce, 0x6a, 0xb9, 0x94, 0x05,
    0x38, 0x76, 0x86, 0x66, 0x97, 0x31, 0x25, 0x30, 0x23, 0x06, 0x09, 0x2a,
    0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x15, 0x31, 0x16, 0x04, 0x14,
    0x3a, 0x9d, 0x84, 0xd4, 0xfd, 0x45, 0xd5, 0xc2, 0x80, 0x6d, 0xaf, 0x27,
    0x85, 0x1f, 0x23, 0xa3, 0xa0, 0xc7, 0xa9, 0x49, 0x30, 0x49, 0x30, 0x31,
    0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02,
    0x01, 0x05, 0x00, 0x04, 0x20, 0x25, 0xaf, 0xce, 0x7f, 0x92, 0xfb, 0x42,
    0x36, 0xe3, 0x97, 0xe4, 0x0c, 0x5e, 0x6f, 0x6e, 0x37, 0xe4, 0x0c, 0x4c,
    0x10, 0x18, 0x96, 0xba, 0xba, 0x9c, 0x0b, 0xa1, 0xb1, 0x04, 0x14, 0x1b,
    0x01, 0x04, 0x10, 0x67, 0xa7, 0x59, 0x31, 0xf2, 0x26, 0x1f, 0x63, 0xfe,
    0x87, 0x20, 0xeb, 0x11, 0xa8, 0x05, 0x00, 0x02, 0x02, 0x08, 0x00,
};

/// Passphrase matching `server_p12`.
inline constexpr char const* p12_password = "test-password";

/** A PKCS#12 bundle carrying leaf + key + one intermediate.

    The leaf is signed by an intermediate (with *critical* basicConstraints,
    which WolfSSL requires of a CA) that is itself signed by
    root_ca_cert_pem. Passphrase: "test-password" (AES-256-CBC PBES2).

    Used to verify the backend loads *and sends* the bundle's chain: a
    client trusting only the root CA can verify only if the server presents
    the intermediate.
*/
inline constexpr unsigned char server_chain_p12[] = {
    0x30, 0x82, 0x0d, 0x77, 0x02, 0x01, 0x03, 0x30, 0x82, 0x0d, 0x25, 0x06,
    0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x01, 0xa0, 0x82,
    0x0d, 0x16, 0x04, 0x82, 0x0d, 0x12, 0x30, 0x82, 0x0d, 0x0e, 0x30, 0x82,
    0x07, 0x7a, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07,
    0x06, 0xa0, 0x82, 0x07, 0x6b, 0x30, 0x82, 0x07, 0x67, 0x02, 0x01, 0x00,
    0x30, 0x82, 0x07, 0x60, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
    0x01, 0x07, 0x01, 0x30, 0x5f, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7,
    0x0d, 0x01, 0x05, 0x0d, 0x30, 0x52, 0x30, 0x31, 0x06, 0x09, 0x2a, 0x86,
    0x48, 0x86, 0xf7, 0x0d, 0x01, 0x05, 0x0c, 0x30, 0x24, 0x04, 0x10, 0xee,
    0x4c, 0xf0, 0xfe, 0xd0, 0x8f, 0xd3, 0xc4, 0xa0, 0x63, 0x6d, 0x1c, 0xee,
    0x99, 0xf0, 0x0d, 0x02, 0x02, 0x08, 0x00, 0x30, 0x0c, 0x06, 0x08, 0x2a,
    0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x09, 0x05, 0x00, 0x30, 0x1d, 0x06,
    0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x01, 0x2a, 0x04, 0x10,
    0x85, 0xef, 0x8a, 0xc1, 0x5d, 0x12, 0xd0, 0x75, 0xaa, 0x83, 0x08, 0xf6,
    0xd9, 0x59, 0x67, 0x61, 0x80, 0x82, 0x06, 0xf0, 0x25, 0xde, 0x75, 0xcb,
    0xbd, 0x86, 0x76, 0x5d, 0x55, 0x86, 0xb2, 0xd8, 0x25, 0x13, 0x4e, 0x63,
    0xb4, 0x7e, 0x94, 0xbf, 0xf9, 0xf8, 0xc0, 0x2a, 0x89, 0x9a, 0xd4, 0x79,
    0x6c, 0xf0, 0x47, 0xc1, 0x51, 0x1c, 0x36, 0x6d, 0xd7, 0xee, 0x14, 0x40,
    0xc9, 0xb2, 0x7c, 0x08, 0x92, 0xd8, 0xf2, 0xd9, 0x71, 0x7f, 0xd9, 0x05,
    0x4d, 0x86, 0xa1, 0x23, 0x11, 0x25, 0xc6, 0x79, 0x35, 0xb0, 0x45, 0xcc,
    0x44, 0xbc, 0x34, 0xe6, 0x05, 0x91, 0xf1, 0xa3, 0xe1, 0x1d, 0x6f, 0x4a,
    0xd0, 0xfa, 0x51, 0x90, 0x74, 0x0d, 0x15, 0x28, 0xd6, 0xda, 0xad, 0xf0,
    0x17, 0x94, 0x8f, 0x81, 0x37, 0x4d, 0x91, 0x75, 0x93, 0xeb, 0xbe, 0xc5,
    0x59, 0xbc, 0x96, 0x0e, 0xce, 0xae, 0x1b, 0x7d, 0xfb, 0x49, 0x81, 0x2b,
    0xf6, 0x33, 0x90, 0xa6, 0xa7, 0xfe, 0xd0, 0x40, 0x2e, 0x60, 0x02, 0xc4,
    0x54, 0x34, 0x63, 0x21, 0xf5, 0x9c, 0x97, 0x26, 0x42, 0xcd, 0xf3, 0x97,
    0x07, 0x5a, 0x6e, 0x41, 0x59, 0x6d, 0xae, 0xa9, 0xc6, 0xa3, 0x1f, 0xbc,
    0x18, 0xfe, 0x8b, 0xf4, 0x14, 0xcd, 0x9c, 0x8a, 0x90, 0x83, 0xd0, 0x1b,
    0x1b, 0x84, 0x7f, 0x5c, 0x38, 0xfa, 0x2a, 0x2b, 0x1e, 0x88, 0xa8, 0xe7,
    0x0e, 0x17, 0xa2, 0xec, 0x63, 0x17, 0x8b, 0xe0, 0xdf, 0xb2, 0xdf, 0x9b,
    0x41, 0x84, 0xf5, 0x43, 0x97, 0x7d, 0x19, 0xf8, 0xe7, 0x41, 0xdb, 0x1f,
    0xfd, 0x9a, 0xe2, 0x5c, 0x53, 0x35, 0x71, 0x23, 0x36, 0x3a, 0x0c, 0x2f,
    0x1e, 0x6a, 0x43, 0x24, 0x16, 0xea, 0x43, 0x6a, 0xe2, 0xf1, 0x1b, 0xc5,
    0x0d, 0x04, 0xa9, 0xe4, 0xc5, 0xe3, 0x9b, 0x8d, 0xb0, 0x05, 0xf6, 0x13,
    0x5f, 0xfa, 0x87, 0x4c, 0x5d, 0xa1, 0xcb, 0x41, 0x0f, 0xd5, 0x2b, 0x4c,
    0xcb, 0xcf, 0x76, 0xaf, 0x94, 0x0d, 0x31, 0x45, 0xe0, 0x98, 0x42, 0xdb,
    0x59, 0xa0, 0x57, 0x77, 0x29, 0x2e, 0x92, 0x2f, 0x99, 0x6f, 0x1f, 0xc6,
    0x09, 0x3c, 0xb3, 0xfd, 0x03, 0x7f, 0xfe, 0x82, 0x44, 0x71, 0xe8, 0x9d,
    0x2e, 0x45, 0x73, 0xfd, 0x51, 0xa1, 0x09, 0x36, 0xb6, 0x08, 0xf4, 0xb4,
    0x2d, 0x95, 0x7d, 0x28, 0x99, 0x18, 0xd6, 0xe9, 0xbe, 0xa0, 0x8f, 0x89,
    0x92, 0xa2, 0x18, 0x55, 0x3d, 0x64, 0x78, 0xb1, 0xa6, 0x4d, 0xa7, 0x9a,
    0x99, 0x68, 0x62, 0x64, 0x01, 0x45, 0x21, 0x71, 0x6f, 0xa1, 0x19, 0x43,
    0xdc, 0xb7, 0x87, 0xf1, 0x49, 0x2d, 0xc7, 0xc4, 0x8f, 0x18, 0xb1, 0xfa,
    0x26, 0x2b, 0xe8, 0x7a, 0xc0, 0x9a, 0x2e, 0xb5, 0xf0, 0xb4, 0x6f, 0x3f,
    0xff, 0x9f, 0x72, 0x28, 0x2e, 0x45, 0x52, 0x61, 0x47, 0xa7, 0x19, 0xb3,
    0x46, 0x7a, 0x24, 0x2f, 0xc9, 0x9f, 0xb4, 0xe9, 0xb4, 0x93, 0x49, 0x40,
    0x30, 0x00, 0x88, 0x65, 0xb3, 0x8d, 0xec, 0xd2, 0x80, 0x19, 0xed, 0xcb,
    0xfe, 0x63, 0x8e, 0xed, 0x82, 0x1a, 0x0f, 0x53, 0xa2, 0x1f, 0xbe, 0x6f,
    0xf3, 0xed, 0x9d, 0x12, 0x37, 0x3b, 0xba, 0x6f, 0xdf, 0x40, 0x7f, 0x15,
    0x58, 0x06, 0xa5, 0xa5, 0x27, 0xc7, 0x7d, 0xcc, 0x80, 0xe5, 0x6c, 0x6d,
    0x0c, 0x5a, 0xdb, 0xc3, 0x49, 0xb9, 0x94, 0x2d, 0xa8, 0x12, 0x5e, 0xa4,
    0x93, 0x0c, 0xf4, 0x8f, 0x36, 0xd4, 0xe8, 0x1d, 0x7d, 0xbc, 0x35, 0x57,
    0x61, 0x29, 0xca, 0xb7, 0xe4, 0xb3, 0x9c, 0xbc, 0x11, 0x57, 0x15, 0x89,
    0xef, 0xda, 0x43, 0xa6, 0xe5, 0x9c, 0xb9, 0x1c, 0xa1, 0x61, 0x8b, 0xe4,
    0x35, 0x13, 0x11, 0xd4, 0x56, 0xba, 0xd2, 0x3c, 0xfd, 0xfc, 0x5e, 0x12,
    0xf9, 0x55, 0x2d, 0x29, 0x66, 0x6d, 0x58, 0xaa, 0x71, 0x2b, 0xae, 0xba,
    0x4b, 0xe0, 0x87, 0x90, 0x0b, 0x1d, 0x73, 0xe2, 0xe7, 0xc2, 0x0d, 0x98,
    0x3a, 0x39, 0x23, 0xdb, 0x1e, 0xb9, 0x14, 0x68, 0xb1, 0xbc, 0x27, 0xe7,
    0x7b, 0xc2, 0x3a, 0x4c, 0x43, 0x03, 0xf7, 0x48, 0x84, 0x68, 0x0a, 0xca,
    0xd6, 0xbd, 0xa6, 0xb3, 0xf6, 0x1c, 0x5e, 0x18, 0xf9, 0x6b, 0x97, 0x98,
    0x36, 0x94, 0xe7, 0x69, 0xab, 0x46, 0x71, 0xac, 0xb1, 0x77, 0x16, 0xe6,
    0x79, 0x9a, 0xab, 0x7c, 0x8b, 0x06, 0x98, 0x43, 0x87, 0xa0, 0xd7, 0x57,
    0x7c, 0x79, 0x46, 0x7a, 0x59, 0x08, 0xde, 0x92, 0x90, 0x7b, 0xe7, 0x9b,
    0xe9, 0xb6, 0xc5, 0xf6, 0x9e, 0x62, 0x61, 0x75, 0x02, 0x99, 0x13, 0x66,
    0x9d, 0x8f, 0x3a, 0xc3, 0x38, 0xd0, 0x66, 0xdc, 0x48, 0xbe, 0xb5, 0xe7,
    0x89, 0xa8, 0x97, 0x54, 0xf8, 0x70, 0x7d, 0xab, 0x51, 0xb3, 0xe7, 0x70,
    0x91, 0x22, 0xe5, 0xa5, 0x8b, 0x44, 0x94, 0x0e, 0x26, 0x1d, 0xe6, 0x77,
    0x8a, 0x19, 0x00, 0x8b, 0xb6, 0x1b, 0x5d, 0x28, 0xca, 0xaa, 0x94, 0xe7,
    0x73, 0x5f, 0x7c, 0xaf, 0xf7, 0x72, 0x96, 0xda, 0x8d, 0x70, 0x74, 0x57,
    0x81, 0x82, 0x3c, 0xb6, 0xe9, 0x9d, 0xad, 0x76, 0x13, 0x7f, 0xb1, 0x49,
    0x76, 0x9e, 0x7c, 0x50, 0x33, 0x46, 0x77, 0x07, 0x3c, 0x30, 0xa6, 0x78,
    0xec, 0xba, 0x4a, 0xe0, 0x90, 0xf3, 0x28, 0x14, 0xd8, 0x42, 0xac, 0x63,
    0xcb, 0xf8, 0x64, 0xe3, 0x0a, 0x95, 0x8c, 0xfb, 0x85, 0x19, 0xd1, 0xe1,
    0xc4, 0xfa, 0x59, 0xdb, 0xbc, 0x95, 0xc1, 0x8a, 0x3e, 0x76, 0xce, 0x34,
    0x95, 0x70, 0x0c, 0xb4, 0x67, 0x23, 0x86, 0x4a, 0xeb, 0x35, 0xcf, 0x61,
    0xd0, 0xe9, 0x47, 0x04, 0x8b, 0xc3, 0x34, 0xc4, 0xf1, 0x28, 0x7a, 0x14,
    0x77, 0x18, 0x2c, 0xe8, 0xac, 0xac, 0xfc, 0x7e, 0x6f, 0x94, 0x2c, 0xb6,
    0xaa, 0xe5, 0xbf, 0x56, 0x78, 0x3f, 0x9a, 0x9a, 0x5b, 0x94, 0x39, 0xce,
    0x8b, 0x8b, 0xde, 0xb9, 0xf3, 0xb1, 0x15, 0x0f, 0x60, 0x79, 0x50, 0xa8,
    0x4d, 0xf6, 0x9b, 0x89, 0xf6, 0xfc, 0x8c, 0x00, 0xfd, 0x29, 0xa1, 0x1c,
    0x7a, 0x79, 0x57, 0xb6, 0xcb, 0x28, 0x0e, 0x71, 0x26, 0x59, 0x48, 0x63,
    0x83, 0xfa, 0xb8, 0xc6, 0x52, 0x99, 0x52, 0xac, 0xba, 0x26, 0x98, 0xec,
    0xbb, 0x0e, 0x2d, 0x23, 0xc0, 0x79, 0xcc, 0x93, 0x10, 0x38, 0xff, 0x67,
    0xe7, 0x4a, 0x3b, 0x3e, 0xdc, 0x01, 0x18, 0xa2, 0x43, 0x82, 0x45, 0x3d,
    0x18, 0x6e, 0x35, 0xfe, 0x2f, 0x28, 0xe0, 0x65, 0x69, 0x59, 0x84, 0x0e,
    0xe5, 0x1f, 0x5e, 0x00, 0x74, 0x1c, 0xf3, 0xa0, 0x51, 0x27, 0xae, 0x2a,
    0xc4, 0x2f, 0x41, 0x67, 0x38, 0xef, 0x32, 0x4f, 0xa5, 0x2a, 0x4b, 0x36,
    0xc3, 0x79, 0xfc, 0x21, 0x4e, 0x4f, 0x43, 0x3b, 0x26, 0xbc, 0xdd, 0xd1,
    0xa0, 0xa8, 0xee, 0x68, 0x7f, 0xd3, 0xbf, 0x8c, 0xde, 0x49, 0x46, 0xc1,
    0xec, 0x18, 0x7c, 0x9f, 0x39, 0x81, 0xde, 0x6f, 0x53, 0x9c, 0xe0, 0x47,
    0x9b, 0xf6, 0xa1, 0x11, 0x59, 0x60, 0xe5, 0xfb, 0x96, 0xb0, 0xe1, 0xcf,
    0x19, 0xdc, 0x68, 0x64, 0xe1, 0x26, 0x92, 0x96, 0x54, 0xc3, 0x7f, 0xac,
    0x5b, 0xa6, 0x34, 0xc3, 0x6e, 0x47, 0x64, 0xa4, 0x31, 0xa4, 0x97, 0x66,
    0x92, 0x7d, 0x78, 0x24, 0xe8, 0x86, 0xea, 0x29, 0x9b, 0x76, 0x8e, 0xd0,
    0x5c, 0x4c, 0x74, 0x8a, 0xf9, 0x13, 0x4f, 0xb7, 0x77, 0x26, 0x04, 0xcd,
    0xfd, 0x9d, 0x65, 0x79, 0x5b, 0xd8, 0xd5, 0xe7, 0x24, 0x76, 0x29, 0x6e,
    0xf8, 0x17, 0x07, 0x97, 0x4c, 0x38, 0x62, 0x31, 0x0e, 0x98, 0xb1, 0x5a,
    0x42, 0xd4, 0xa5, 0xe2, 0xcd, 0xef, 0xe0, 0x78, 0xbd, 0x0d, 0x9f, 0x35,
    0x1b, 0x1c, 0x96, 0x2b, 0x5a, 0xb0, 0xc9, 0x5f, 0xfe, 0xac, 0xfe, 0x3a,
    0x8f, 0x51, 0x11, 0x22, 0x15, 0xdb, 0x28, 0x10, 0xc5, 0x44, 0xad, 0x98,
    0x13, 0x3b, 0x0d, 0x77, 0xe4, 0x3f, 0xc2, 0x39, 0xf6, 0x9f, 0x51, 0x25,
    0xd0, 0x19, 0x1e, 0x60, 0x4e, 0x00, 0xb1, 0x93, 0x4a, 0x8b, 0x24, 0xce,
    0x7c, 0x0d, 0xbe, 0xb7, 0x85, 0x33, 0x8c, 0x01, 0x66, 0x5d, 0xf8, 0x32,
    0x00, 0xb8, 0xb5, 0xf5, 0x7e, 0xd8, 0xa7, 0xc2, 0x80, 0xdb, 0x56, 0xf3,
    0x63, 0x99, 0xcd, 0x66, 0xef, 0xd3, 0xc5, 0x5c, 0x00, 0xb4, 0x73, 0x27,
    0x8e, 0x43, 0xa2, 0x24, 0x6b, 0x6f, 0x2a, 0xc1, 0xd1, 0x29, 0x23, 0x5b,
    0x82, 0xf5, 0x26, 0x53, 0x8f, 0x70, 0x03, 0x7b, 0x2d, 0x67, 0xcd, 0x27,
    0x09, 0x50, 0x3e, 0xfc, 0xe2, 0xb5, 0x7e, 0x5f, 0x16, 0x58, 0xb2, 0x9d,
    0xd1, 0xb5, 0xd8, 0x96, 0xa3, 0x3b, 0x70, 0xe9, 0x8c, 0x33, 0xcc, 0x29,
    0xb2, 0xc3, 0xc2, 0x21, 0x48, 0xe8, 0x2d, 0x53, 0xeb, 0xa5, 0x24, 0xbb,
    0x24, 0x99, 0x8d, 0xb6, 0x4c, 0x4e, 0x31, 0xad, 0x55, 0xff, 0xb1, 0x3e,
    0x95, 0x33, 0xa8, 0xcc, 0xef, 0xd8, 0x3a, 0xf7, 0x3d, 0x7d, 0x55, 0xfd,
    0x3f, 0xa1, 0x29, 0xba, 0xe6, 0x7d, 0x1d, 0xf2, 0x27, 0xb8, 0x7b, 0xcf,
    0xd0, 0x42, 0x79, 0x36, 0x72, 0xb0, 0x47, 0xd5, 0xff, 0x8c, 0x02, 0x64,
    0x65, 0x1e, 0x6c, 0x94, 0x45, 0x25, 0x97, 0xc0, 0xa5, 0x84, 0x6b, 0xd6,
    0x11, 0x95, 0x03, 0x6e, 0x3f, 0xbc, 0x32, 0x97, 0xa5, 0xf2, 0x36, 0xb5,
    0xb8, 0x41, 0x39, 0xcc, 0x8d, 0xe5, 0xac, 0x4f, 0xa0, 0xdc, 0xa2, 0x8e,
    0xd2, 0xe5, 0xfa, 0x73, 0x30, 0xb5, 0x30, 0x7e, 0x79, 0x43, 0x74, 0x45,
    0x5d, 0xb6, 0xa1, 0x3d, 0x31, 0xf6, 0x0e, 0x6a, 0x18, 0x9c, 0x72, 0x49,
    0x44, 0xb1, 0xf6, 0x0d, 0x27, 0xd8, 0x63, 0x1b, 0xbc, 0xbe, 0x0f, 0x04,
    0x10, 0x43, 0xd2, 0x50, 0xd5, 0x36, 0xa2, 0x82, 0x78, 0x74, 0x51, 0xe2,
    0x68, 0xd0, 0x84, 0x13, 0x5d, 0xf8, 0xa5, 0x7a, 0x73, 0x69, 0x64, 0xc1,
    0x29, 0xd0, 0x6e, 0xd1, 0x03, 0xaa, 0xfc, 0x03, 0xf2, 0xbf, 0xb1, 0xe3,
    0x33, 0xba, 0x3e, 0x5f, 0x27, 0xcc, 0x78, 0x33, 0xca, 0x73, 0xe1, 0x1a,
    0x05, 0xb0, 0x66, 0xac, 0xc3, 0x5b, 0xe3, 0x34, 0x06, 0x36, 0x44, 0x92,
    0x8d, 0x5d, 0x25, 0x6f, 0x19, 0xa7, 0x6f, 0xf1, 0xa8, 0xb3, 0x9f, 0x37,
    0xf6, 0x22, 0x70, 0xf8, 0x61, 0x85, 0x58, 0xcb, 0x83, 0x5d, 0x86, 0x0a,
    0xbe, 0x7c, 0x85, 0x09, 0x3f, 0xa5, 0x4f, 0xb0, 0x89, 0xf6, 0x09, 0xaa,
    0x1e, 0x90, 0x1e, 0x55, 0xe6, 0x23, 0xb2, 0x4b, 0xed, 0x09, 0xa8, 0x6a,
    0x0e, 0x21, 0xa6, 0x92, 0xac, 0x17, 0x92, 0xe1, 0xa7, 0x36, 0x83, 0xbd,
    0xe5, 0xae, 0xf4, 0x00, 0x4e, 0x48, 0x7f, 0xda, 0x2f, 0x6f, 0xe3, 0x83,
    0xf5, 0x04, 0x28, 0xbe, 0xb6, 0xbb, 0x68, 0x85, 0x9a, 0x04, 0x1b, 0x44,
    0xfb, 0x6f, 0x62, 0xcd, 0x71, 0x88, 0x3d, 0xf8, 0x75, 0x2a, 0x41, 0x0a,
    0xeb, 0xfa, 0x4a, 0xe6, 0x52, 0x0f, 0xaf, 0xe3, 0xfe, 0x50, 0xcc, 0xf3,
    0x50, 0x57, 0x57, 0xff, 0x42, 0x62, 0x57, 0x78, 0x44, 0xa5, 0x50, 0xc6,
    0x66, 0xd1, 0x0d, 0x28, 0xf1, 0xa2, 0xdc, 0x07, 0xc8, 0x35, 0x85, 0xd4,
    0xea, 0x7a, 0xec, 0x41, 0x48, 0x2f, 0x90, 0x2d, 0x97, 0x77, 0x97, 0x3e,
    0xa9, 0x1a, 0x48, 0x7e, 0x6e, 0x83, 0x91, 0xcc, 0xba, 0x2a, 0xcd, 0x87,
    0x00, 0x85, 0xe1, 0x15, 0x8f, 0xbc, 0xf5, 0xf7, 0x28, 0x5f, 0x55, 0x94,
    0x52, 0x06, 0xe8, 0x40, 0x0d, 0x75, 0x1a, 0xe3, 0xe6, 0xcf, 0xc5, 0xc2,
    0x36, 0x29, 0x46, 0x8e, 0xa8, 0x4e, 0x64, 0x5e, 0xf0, 0x58, 0xe7, 0x34,
    0x5d, 0xd7, 0x71, 0x11, 0xcd, 0x78, 0x9f, 0xcf, 0x61, 0x7d, 0x71, 0x31,
    0x99, 0x53, 0xd2, 0xe6, 0x8c, 0xf7, 0xb5, 0x66, 0xcb, 0xcd, 0xb8, 0x90,
    0x04, 0x77, 0x8a, 0xdf, 0xb8, 0xf7, 0x59, 0xc9, 0x5b, 0xee, 0xcb, 0x9e,
    0xe8, 0xca, 0x6a, 0x10, 0xc5, 0xc2, 0xa6, 0x7e, 0xe1, 0x3e, 0xc4, 0x37,
    0x74, 0x98, 0xa2, 0x58, 0x3e, 0xfb, 0x34, 0x75, 0xbc, 0xb6, 0xd1, 0x30,
    0x8f, 0xc8, 0x3f, 0xeb, 0x1e, 0x6c, 0xdf, 0x01, 0x9d, 0xdf, 0xf0, 0x15,
    0x99, 0xfd, 0x9b, 0xf8, 0xda, 0xe2, 0xb2, 0xb9, 0x39, 0xa9, 0x81, 0xaa,
    0x86, 0x00, 0x45, 0xd0, 0x09, 0x5e, 0x65, 0xc9, 0x11, 0x83, 0x48, 0x65,
    0x92, 0x0e, 0x2a, 0x77, 0x15, 0x16, 0xaa, 0x50, 0x07, 0x31, 0xe5, 0xc7,
    0xd4, 0x92, 0x60, 0x39, 0xc9, 0xa6, 0x92, 0x4b, 0x2e, 0x86, 0x34, 0x73,
    0x2f, 0x6e, 0xfc, 0x8a, 0xa1, 0xc4, 0x06, 0xab, 0xf5, 0xb2, 0x01, 0x41,
    0x0d, 0xa3, 0x0e, 0x8b, 0x9d, 0x36, 0x07, 0xbd, 0xbc, 0x60, 0xf1, 0x69,
    0xcf, 0x7c, 0xff, 0x3d, 0xf2, 0x8e, 0x6c, 0xf0, 0xa9, 0xba, 0x1e, 0xed,
    0x08, 0x0a, 0x58, 0x1a, 0x34, 0xbf, 0x6f, 0x33, 0x5f, 0xe7, 0x76, 0xf4,
    0x35, 0x92, 0xa0, 0xa8, 0x09, 0xca, 0xaa, 0xee, 0xe4, 0xc4, 0x4c, 0xa7,
    0x40, 0x20, 0x48, 0x31, 0x0f, 0xa2, 0xbb, 0x42, 0x17, 0x47, 0xb9, 0xf9,
    0x76, 0x9c, 0xf9, 0x6b, 0x79, 0x34, 0xa1, 0xeb, 0x94, 0x64, 0x91, 0x38,
    0x3d, 0xc0, 0x3f, 0x08, 0xe0, 0x56, 0xa9, 0x0b, 0x0d, 0x4b, 0xcc, 0x9d,
    0xc7, 0xdc, 0x5c, 0x4d, 0x18, 0x17, 0xb9, 0xee, 0xbf, 0x85, 0x21, 0xc2,
    0xeb, 0x99, 0x2b, 0x86, 0x21, 0x20, 0xdf, 0xf4, 0xc1, 0xa5, 0x59, 0x89,
    0x4f, 0xc0, 0xa2, 0x9f, 0x20, 0xdc, 0xc6, 0xf9, 0x9f, 0xfa, 0xfd, 0x53,
    0x33, 0x4f, 0xa6, 0xb8, 0xe7, 0xb2, 0xf0, 0xc9, 0x30, 0x82, 0x05, 0x8c,
    0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x01, 0xa0,
    0x82, 0x05, 0x7d, 0x04, 0x82, 0x05, 0x79, 0x30, 0x82, 0x05, 0x75, 0x30,
    0x82, 0x05, 0x71, 0x06, 0x0b, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01,
    0x0c, 0x0a, 0x01, 0x02, 0xa0, 0x82, 0x05, 0x39, 0x30, 0x82, 0x05, 0x35,
    0x30, 0x5f, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x05,
    0x0d, 0x30, 0x52, 0x30, 0x31, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7,
    0x0d, 0x01, 0x05, 0x0c, 0x30, 0x24, 0x04, 0x10, 0x0f, 0x2a, 0x09, 0xd1,
    0x70, 0xd5, 0x1c, 0xe4, 0x2e, 0xc2, 0xac, 0x82, 0x0c, 0xdd, 0x11, 0x32,
    0x02, 0x02, 0x08, 0x00, 0x30, 0x0c, 0x06, 0x08, 0x2a, 0x86, 0x48, 0x86,
    0xf7, 0x0d, 0x02, 0x09, 0x05, 0x00, 0x30, 0x1d, 0x06, 0x09, 0x60, 0x86,
    0x48, 0x01, 0x65, 0x03, 0x04, 0x01, 0x2a, 0x04, 0x10, 0x81, 0x10, 0xe4,
    0xff, 0xdd, 0x0c, 0x0e, 0xed, 0x45, 0x46, 0x0a, 0x1b, 0xcf, 0x78, 0x3c,
    0xd3, 0x04, 0x82, 0x04, 0xd0, 0xee, 0x4b, 0x1d, 0x9d, 0xa9, 0xa2, 0x3b,
    0xf8, 0x48, 0x8d, 0xe5, 0x28, 0x18, 0x41, 0xe6, 0x40, 0x4b, 0xa7, 0x61,
    0xef, 0x77, 0x7a, 0xfe, 0x28, 0x8d, 0x39, 0x51, 0x0e, 0xdc, 0x24, 0x4f,
    0xe0, 0x1e, 0x09, 0x28, 0x01, 0x88, 0x19, 0x4c, 0x37, 0xc2, 0x99, 0x2c,
    0x6c, 0x2f, 0x3b, 0xbd, 0x24, 0x80, 0x98, 0x18, 0x33, 0x5e, 0x2b, 0x0b,
    0xe4, 0xcd, 0x00, 0xec, 0x9b, 0x0c, 0xdd, 0xf5, 0x68, 0x3b, 0x57, 0xeb,
    0x72, 0x9a, 0xab, 0x73, 0xa4, 0x9f, 0x9a, 0x40, 0xec, 0xe8, 0x74, 0x95,
    0xe4, 0x55, 0xb0, 0x0c, 0x6d, 0xcb, 0xdf, 0x7f, 0x7c, 0x58, 0xf6, 0xc5,
    0x99, 0xd7, 0x57, 0x56, 0x90, 0xf9, 0x78, 0x0f, 0x15, 0xd3, 0x7e, 0xc9,
    0xff, 0x6a, 0xcc, 0xfa, 0x98, 0xdd, 0x0d, 0x12, 0xfb, 0x7a, 0xb8, 0x2e,
    0x71, 0xf6, 0x4e, 0xa1, 0x1d, 0x84, 0x3a, 0x58, 0x02, 0x34, 0x2b, 0x03,
    0xab, 0x4b, 0x14, 0xd9, 0xa1, 0xa1, 0xa6, 0x15, 0x64, 0x06, 0x37, 0xf6,
    0xa6, 0x88, 0x2c, 0x22, 0xab, 0x5d, 0x3e, 0x46, 0x3b, 0xe8, 0x3c, 0x4a,
    0x96, 0x33, 0x69, 0xa2, 0x7c, 0x0f, 0xcf, 0xc1, 0x3b, 0x89, 0x27, 0x55,
    0x0d, 0xac, 0x60, 0xf1, 0x81, 0x22, 0x40, 0x24, 0x46, 0xd9, 0x47, 0x22,
    0xd1, 0xc9, 0xb4, 0xf7, 0x17, 0x86, 0x46, 0x3f, 0x73, 0x6c, 0xb6, 0x7d,
    0x20, 0x29, 0xda, 0x9c, 0x44, 0x08, 0x5d, 0xc1, 0x6f, 0x23, 0x90, 0xaf,
    0x57, 0xc0, 0x47, 0xfa, 0x42, 0x02, 0x1e, 0x22, 0x88, 0x1e, 0x5e, 0x38,
    0x2a, 0x20, 0x97, 0xd9, 0x0f, 0xb7, 0x62, 0xc4, 0xa7, 0x85, 0x3b, 0xd4,
    0x97, 0xe0, 0x33, 0x83, 0x91, 0x86, 0xc1, 0x34, 0xfe, 0x2c, 0xf4, 0x49,
    0x98, 0x1c, 0x52, 0x23, 0x77, 0x39, 0x33, 0x4a, 0x2b, 0x8d, 0x10, 0x67,
    0x57, 0x06, 0x01, 0x68, 0x60, 0x6c, 0x97, 0x71, 0x18, 0xd2, 0x30, 0x91,
    0xba, 0x43, 0xf7, 0x31, 0x2b, 0xe1, 0x64, 0xc9, 0xc7, 0xf4, 0x63, 0x2d,
    0x0a, 0xf4, 0xe1, 0x57, 0xab, 0x52, 0x37, 0x56, 0x66, 0x53, 0xbd, 0xdd,
    0xfa, 0xbf, 0x25, 0x65, 0xf7, 0x6e, 0xdf, 0x9d, 0x79, 0x26, 0x93, 0x41,
    0xf2, 0x82, 0x0c, 0x57, 0xaa, 0x40, 0x49, 0x1e, 0x21, 0x47, 0xe1, 0x41,
    0x7f, 0xc5, 0x47, 0x08, 0xa9, 0x3f, 0xb4, 0xa2, 0xb5, 0xb4, 0xf6, 0x52,
    0x2e, 0x5a, 0x1e, 0xa7, 0x54, 0x23, 0xee, 0x32, 0x11, 0x18, 0x1f, 0x1a,
    0x8a, 0x18, 0x48, 0x9e, 0x06, 0x2a, 0x28, 0x53, 0x9c, 0x9d, 0x0d, 0xce,
    0xef, 0x2e, 0xba, 0xce, 0xff, 0x2a, 0x8b, 0x55, 0x5d, 0x71, 0x36, 0x65,
    0x2c, 0x6d, 0xd2, 0xbb, 0x51, 0x5a, 0x87, 0xa3, 0x99, 0x01, 0x4f, 0x45,
    0x5b, 0xce, 0xff, 0xcc, 0xf6, 0x16, 0x34, 0xd9, 0xc4, 0xec, 0xa1, 0xe6,
    0x25, 0x8b, 0x0e, 0x64, 0x00, 0xc4, 0x9e, 0x8c, 0xb0, 0xa6, 0xa9, 0x4a,
    0x52, 0x81, 0x0a, 0xab, 0x6f, 0x98, 0x87, 0xf2, 0xe1, 0xad, 0x60, 0x98,
    0xf6, 0xfa, 0xe6, 0xc6, 0xcb, 0xdd, 0xd8, 0x59, 0x88, 0xe7, 0xd1, 0x67,
    0xb2, 0x16, 0x16, 0x97, 0x19, 0x18, 0x4b, 0xec, 0x6d, 0x96, 0xc0, 0xc5,
    0xaa, 0x2f, 0xc9, 0xa8, 0xa7, 0x3e, 0x29, 0x23, 0x85, 0xfd, 0x64, 0xf9,
    0xd2, 0x27, 0x1c, 0x1c, 0x11, 0x46, 0xf0, 0x9e, 0xfe, 0x87, 0xde, 0x14,
    0xa9, 0xc5, 0x6a, 0x2a, 0xe7, 0x1b, 0xd1, 0x9e, 0xd3, 0x2c, 0xbd, 0x75,
    0x07, 0x8a, 0xb9, 0x01, 0xfe, 0xa5, 0xfb, 0x4e, 0x6d, 0x69, 0x01, 0x97,
    0x68, 0x02, 0xc3, 0xec, 0xd5, 0xbb, 0x20, 0xcf, 0x44, 0x33, 0xfe, 0x46,
    0x3d, 0xd1, 0xbb, 0x5c, 0x33, 0x76, 0x49, 0x58, 0x71, 0x0f, 0xa3, 0x63,
    0x54, 0x98, 0xf3, 0xee, 0xc9, 0x7a, 0x7c, 0xc0, 0xe4, 0x35, 0x4f, 0xe4,
    0x26, 0x42, 0x59, 0xe0, 0x57, 0xfa, 0xa4, 0xa0, 0xb8, 0x37, 0x6a, 0xe3,
    0x97, 0xbe, 0xcc, 0x00, 0x73, 0x10, 0xb2, 0xf7, 0x4a, 0x9d, 0x3a, 0xdc,
    0x8e, 0x7c, 0x8c, 0x2d, 0x6d, 0x1f, 0x9a, 0x52, 0x9a, 0x96, 0x0f, 0xb4,
    0x76, 0x57, 0x6c, 0x1c, 0x17, 0xe8, 0xef, 0xcb, 0x5a, 0x07, 0x74, 0x17,
    0x08, 0xf9, 0x1a, 0x7d, 0xc6, 0x3d, 0x87, 0x40, 0xef, 0xd3, 0xff, 0x3b,
    0xb5, 0x4f, 0x45, 0xec, 0x73, 0xd1, 0x2f, 0x46, 0x3d, 0x6d, 0x48, 0x97,
    0x4c, 0xb2, 0x2c, 0x7b, 0xe0, 0xee, 0x6b, 0xbd, 0xd5, 0x5b, 0x51, 0xe4,
    0xd2, 0xa0, 0x96, 0xe4, 0x6f, 0xd0, 0xeb, 0x39, 0xbc, 0xac, 0x30, 0x2f,
    0x89, 0xbf, 0xf6, 0xab, 0xc5, 0x50, 0x9d, 0xe1, 0x8b, 0x06, 0xcb, 0xa0,
    0xc7, 0x9c, 0x6e, 0x5f, 0x08, 0x62, 0xea, 0x9b, 0x4d, 0x93, 0xc1, 0xee,
    0x90, 0x1b, 0x8d, 0xd4, 0xb6, 0x29, 0xf4, 0x7d, 0xde, 0x49, 0xb6, 0x30,
    0xc2, 0x64, 0x0b, 0xde, 0x07, 0x6f, 0x2c, 0x6a, 0x32, 0x81, 0xc6, 0x0e,
    0x9e, 0xae, 0x3f, 0x93, 0x58, 0xef, 0x2c, 0xd6, 0xb5, 0xc2, 0x4a, 0x13,
    0xe4, 0x41, 0x6d, 0xf3, 0xf0, 0xc5, 0x96, 0x06, 0xf3, 0x91, 0xe7, 0xe9,
    0x94, 0x34, 0x8d, 0x76, 0xfa, 0xe7, 0xa1, 0x10, 0xc3, 0xc7, 0x73, 0x42,
    0x63, 0x21, 0xdf, 0x33, 0x53, 0xdb, 0x81, 0x0f, 0xea, 0xee, 0xba, 0x69,
    0x54, 0xfd, 0x95, 0xe1, 0xf0, 0xdd, 0x25, 0x55, 0x27, 0xf4, 0xa1, 0x51,
    0x6d, 0x19, 0x01, 0xab, 0x73, 0xdc, 0x16, 0xa5, 0xde, 0x45, 0x1f, 0x13,
    0x4e, 0x8f, 0x2a, 0x3f, 0x25, 0x4f, 0xb0, 0x70, 0xca, 0x00, 0x4c, 0x5f,
    0xf8, 0x31, 0xde, 0x53, 0xd9, 0x84, 0x70, 0xfe, 0xfe, 0xe6, 0x75, 0x8b,
    0xac, 0xb0, 0x2c, 0x1c, 0x7e, 0xb2, 0x90, 0x17, 0x37, 0xa6, 0x9c, 0x22,
    0xaa, 0xbe, 0x40, 0x6c, 0xdc, 0xa7, 0xcc, 0x6f, 0xa8, 0xb7, 0xa5, 0x57,
    0xd5, 0x16, 0xd9, 0xac, 0x9b, 0xd5, 0x1e, 0xc8, 0x5d, 0xc8, 0x01, 0xaa,
    0x3b, 0x70, 0xee, 0x80, 0xc3, 0xec, 0x89, 0xb1, 0xeb, 0x27, 0xb0, 0xca,
    0x4b, 0x73, 0x2c, 0x7c, 0x13, 0xda, 0x47, 0x51, 0x01, 0x67, 0xd8, 0xda,
    0x47, 0xb1, 0x45, 0xd5, 0x69, 0xb0, 0x5f, 0x42, 0x54, 0x51, 0x55, 0x35,
    0xe1, 0x04, 0x7b, 0x4b, 0x61, 0x3d, 0xb7, 0xe8, 0x72, 0x8b, 0x30, 0x3a,
    0x53, 0xfb, 0x7b, 0x8a, 0x77, 0x17, 0x9c, 0x18, 0x1c, 0x57, 0x71, 0x60,
    0x45, 0xad, 0xc4, 0xd7, 0x93, 0x46, 0x04, 0x08, 0x45, 0x22, 0x58, 0x21,
    0x3a, 0xbf, 0x7b, 0x45, 0x2c, 0x3b, 0x2d, 0x6a, 0xa2, 0x8e, 0xcc, 0xbb,
    0x1f, 0xf2, 0xf3, 0xaf, 0x4a, 0x78, 0xb3, 0x5c, 0x8e, 0x74, 0xcb, 0x06,
    0x52, 0xc8, 0xe9, 0x35, 0xc5, 0x35, 0xdb, 0x57, 0x34, 0xe5, 0x96, 0x41,
    0xd3, 0xdf, 0xfc, 0xe7, 0xef, 0xbd, 0x58, 0xf1, 0x43, 0x04, 0xe5, 0x0b,
    0x1e, 0x38, 0xd9, 0x2d, 0xaa, 0xb8, 0xce, 0xf5, 0x4e, 0x36, 0xb5, 0xc8,
    0x3d, 0xce, 0x9d, 0xc4, 0x1a, 0xcf, 0xac, 0x68, 0x18, 0x6f, 0xce, 0x9a,
    0xa8, 0x55, 0x65, 0x94, 0xc8, 0x70, 0x32, 0xa2, 0x74, 0x00, 0x40, 0xe5,
    0x53, 0xc1, 0xd5, 0xe6, 0x10, 0xfd, 0x18, 0xe4, 0x0c, 0xca, 0xb0, 0x47,
    0xd2, 0xae, 0x9f, 0x73, 0xf6, 0x00, 0xe5, 0xb8, 0x8b, 0xd0, 0xe9, 0xbc,
    0xfb, 0x54, 0xc9, 0x1f, 0x1c, 0xa1, 0x7e, 0x43, 0x49, 0x05, 0x9e, 0x6d,
    0xcf, 0x48, 0x73, 0x25, 0xf7, 0xfc, 0xfc, 0xae, 0xa1, 0x89, 0xa0, 0x19,
    0x19, 0xf6, 0x3e, 0x53, 0x07, 0x64, 0xaf, 0x92, 0xe1, 0x30, 0x5f, 0x58,
    0x6e, 0x3c, 0x94, 0x25, 0x1e, 0xe1, 0xdc, 0x0b, 0xf3, 0xb6, 0x3f, 0xbb,
    0xf6, 0x25, 0x9c, 0xd1, 0xe9, 0xe1, 0x87, 0x93, 0x4b, 0x88, 0x72, 0xd1,
    0xdf, 0xb1, 0x3d, 0xc4, 0x96, 0xff, 0x54, 0x17, 0x76, 0xce, 0xd1, 0xce,
    0x9e, 0x6e, 0x0a, 0x38, 0xc1, 0x35, 0xcc, 0xee, 0xdb, 0x29, 0x91, 0x15,
    0xd9, 0xb7, 0x3f, 0x82, 0x1d, 0x69, 0x89, 0x53, 0x32, 0x95, 0x7b, 0x43,
    0xa6, 0xde, 0x5a, 0x23, 0xb0, 0xa0, 0x7c, 0x35, 0xea, 0x79, 0x43, 0x27,
    0xf5, 0xf1, 0xa7, 0xeb, 0x3f, 0x8d, 0x96, 0xc1, 0x05, 0x40, 0xde, 0xfa,
    0xd1, 0x30, 0x9a, 0x26, 0xfb, 0xa0, 0x10, 0x60, 0x5b, 0x85, 0xee, 0x9a,
    0xf9, 0x14, 0x15, 0x46, 0xf3, 0x1c, 0x61, 0xe7, 0xa2, 0x9b, 0x6f, 0x8a,
    0x31, 0x1d, 0x1d, 0x71, 0xea, 0x21, 0x2c, 0x36, 0xd4, 0x52, 0x25, 0x6f,
    0xb3, 0x89, 0x6f, 0x34, 0x44, 0x6f, 0x4c, 0xd9, 0xb1, 0xe7, 0xc8, 0x21,
    0x1d, 0x7b, 0xe5, 0x18, 0x6d, 0x67, 0xf4, 0xfa, 0xfb, 0x55, 0x96, 0x6b,
    0x9b, 0xeb, 0xc6, 0x80, 0x57, 0x53, 0x6a, 0x3b, 0xda, 0xb5, 0x55, 0x93,
    0x82, 0xa0, 0xc1, 0x09, 0xfe, 0x52, 0xfe, 0xb4, 0xea, 0x6e, 0xd5, 0x37,
    0xd2, 0x55, 0x63, 0xff, 0x63, 0x65, 0x94, 0x37, 0x2c, 0x83, 0x99, 0x1f,
    0x09, 0x8f, 0xee, 0x5c, 0x3a, 0x09, 0x85, 0xfb, 0xbb, 0x8b, 0xeb, 0x19,
    0xe6, 0xdf, 0x11, 0x48, 0x5d, 0xb7, 0x7d, 0x71, 0xee, 0x09, 0xe0, 0x2d,
    0x5d, 0x5b, 0x9b, 0xf1, 0xa4, 0x83, 0xd2, 0x1a, 0x5d, 0xac, 0xef, 0xe7,
    0xa3, 0x6b, 0x52, 0x9f, 0x5d, 0x4a, 0xb6, 0x09, 0x46, 0x24, 0x94, 0x39,
    0x31, 0x31, 0x25, 0x30, 0x23, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7,
    0x0d, 0x01, 0x09, 0x15, 0x31, 0x16, 0x04, 0x14, 0xe0, 0x54, 0x19, 0x5a,
    0x15, 0x9f, 0x5f, 0x13, 0xbf, 0xc1, 0x88, 0x01, 0x7e, 0x95, 0x88, 0x64,
    0xf8, 0x9f, 0x50, 0xe2, 0x30, 0x49, 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09,
    0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04,
    0x20, 0x2b, 0xfc, 0x32, 0x0d, 0xfd, 0xa2, 0x97, 0x36, 0x58, 0x3b, 0x03,
    0x13, 0x4d, 0x66, 0x3c, 0x9d, 0x1c, 0xa7, 0x83, 0x15, 0xbb, 0xe0, 0xd3,
    0x4f, 0x9a, 0x72, 0x35, 0x81, 0x4d, 0x67, 0xb9, 0xdd, 0x04, 0x10, 0xb6,
    0xf2, 0x05, 0x54, 0xd5, 0x3c, 0x0f, 0x1b, 0x5d, 0x2d, 0x07, 0x58, 0x63,
    0x52, 0x43, 0x78, 0x02, 0x02, 0x08, 0x00,
};

/** Server leaf signed by root_ca_cert_pem; revoked in revoked_crl_pem.
    SAN: www.example.com. Generated via the openssl ca workflow. */
inline constexpr char const* revoked_leaf_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDGjCCAgKgAwIBAgICEAAwDQYJKoZIhvcNAQELBQAwFzEVMBMGA1UEAwwMVGVz\n"
    "dCBSb290IENBMB4XDTI2MDcwODIwMzQ1NVoXDTM2MDcwNTIwMzQ1NVowHjEcMBoG\n"
    "A1UEAwwTcmV2b2tlZC5leGFtcGxlLmNvbTCCASIwDQYJKoZIhvcNAQEBBQADggEP\n"
    "ADCCAQoCggEBALOWQpheNKte2RFaJyQiKbJ/tCUYSGAQPwbPwbBpi1rgCz6FcPBK\n"
    "XY3TmhtgU4ACIgt/lZ0W+vGvyoP2VH68+4Tic6/D043DEYwj6VfnCoukJszVUqwB\n"
    "/bWfHg5eSRqL390LPdJCMhdSDBjlIw2Kocpt7XwL7o9azzSOlcswgP7OJhLGg3Zj\n"
    "FtLuJzLCFZnZwn4Eyxv1HxhOq7v3+GJwYUo9YwHiK++6trzLOLaV4TCosWUVEueL\n"
    "qMYTcB3A6kXAHlA/PfQyD55kR4Kde/kRSVvrYrODZokgmdcKjAeV4Y3Kg2Fo8qAH\n"
    "RnzBoRW4+Sh1LJxvSoWJ6gG5FX8vqDAxIvkCAwEAAaNpMGcwCQYDVR0TBAIwADAa\n"
    "BgNVHREEEzARgg93d3cuZXhhbXBsZS5jb20wHQYDVR0OBBYEFB3qd1NF/SNrZrr8\n"
    "JoRvIEoBaYX8MB8GA1UdIwQYMBaAFIMzUb5b+JvY7MnzQFVN+CG75ojHMA0GCSqG\n"
    "SIb3DQEBCwUAA4IBAQCtBcup+tPyJzAYYc8k9w6mr5itVLtJ/pY/KgAGsXdVjdPu\n"
    "jEVt5/O3sxkxguSlC34MlTvs9bznt+j9pHkg7iZ1iE4nLWumBkEUdADs+HKq3h6s\n"
    "l4fdDfkbI7ANPd7zf7gidwB1cfcrswBJuNUkRPg9O+59qdo690bw5ucTz2meDagt\n"
    "+RZ3qoZQzGlFT+cbXt1u3pUmVnt7wMfdN9Xig5PNSx5pEEz9SDDnD5UXKqP/h7Qm\n"
    "mtbntoIyAdn8mNn+2HEWaVz8zmF1ONP/dchhu54ntd731/xi0ElbFZUUWIsI/l9N\n"
    "OG746U3+6RD0HP2ktKanQ3tfJOyrsmdzAFFCWTWJ\n"
    "-----END CERTIFICATE-----\n";

/** Private key for revoked_leaf_cert_pem. */
inline constexpr char const* revoked_leaf_key_pem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCzlkKYXjSrXtkR\n"
    "WickIimyf7QlGEhgED8Gz8GwaYta4As+hXDwSl2N05obYFOAAiILf5WdFvrxr8qD\n"
    "9lR+vPuE4nOvw9ONwxGMI+lX5wqLpCbM1VKsAf21nx4OXkkai9/dCz3SQjIXUgwY\n"
    "5SMNiqHKbe18C+6PWs80jpXLMID+ziYSxoN2YxbS7icywhWZ2cJ+BMsb9R8YTqu7\n"
    "9/hicGFKPWMB4ivvura8yzi2leEwqLFlFRLni6jGE3AdwOpFwB5QPz30Mg+eZEeC\n"
    "nXv5EUlb62Kzg2aJIJnXCowHleGNyoNhaPKgB0Z8waEVuPkodSycb0qFieoBuRV/\n"
    "L6gwMSL5AgMBAAECggEAFvzGCSSPNQZpiP0+DbPVHDTuKrdj1ZlkGpZFB99NXV8D\n"
    "TB1JmmsdHuE16d4tIga5M+PraJW/SROEO9sFRXR29N4FtTsUR8zvNeiHibRtwaJ2\n"
    "mE6QDADJ6Hx1x5y4WnkKuTZ4qILHpaY0x9+rWnmN1yW2Dzmkb4jOBWoLm4bZuYO+\n"
    "cKZXsPAhFk897kRtK4v0zZZ3kraVjSuL4wuBi0xb5nZ59DH/mZWZH82SQuiu1r5K\n"
    "adfPXZenAd9iDp+NlwuEMJ+cnFmwjrrLKYnn/H3zdwX/6ZdDk7FxGbbYP/iHC21J\n"
    "rPf7mYyUJv8foJ3F8dbInBgPjqXzPmM2Gck5Z199hwKBgQDihlixSH2Kn5TweCp6\n"
    "qhn5jsw8RZteSM0WSm/VcVftHtdzMS/tlrRTLDI0ZIhu1f+p4NrwStW2+0MLkanG\n"
    "/RZOtErp23pmP2qnMVR1WQu3MeqbAkwM9Mm7w02nVOwBeOaWtlCP3Na4bUKbpMiG\n"
    "jcyfRZORZ1eigeEopKK+1OHG9wKBgQDK9GQV/9JkfW7iLpIHFBxxhd+5TlkNqhML\n"
    "lMwTAgE4KewBKKRViwVHZAWjqhRuvC1+JCiaH7QPQWKZx0eEAPNmaeXQ0yUzAQin\n"
    "aaaa0UxOFkkD4kBSYkaownKuymReKwZyK9xoeSPwtXAZkJKpqHSHFkt9wFtt6i4z\n"
    "l3WoNeo5jwKBgDKNzQlzpiDj0HeCOei6QaXCSq5A0pXOJYcOAbte2kKfGXIpzgp2\n"
    "EbRmLqYmsZQayj39Yp8x9FQr6yCP15YDMZFLB1T9mGltSb4ackDmKIkv6K3Da3mQ\n"
    "v9zZj2ECwNDrTHriIUSaAomSSMU3l1EAIGSDQJW4vIQV/Ev3wiJYnDKtAoGAD1kD\n"
    "6JX79xV1OS2EZXyj2gHhtUWzflEKN6n89MMGDJU+/6dvJfjpYUizFHlcKjOYzR02\n"
    "5NDY8P5k0nQ7eEQKJAiGFJCjE4RUfzSCINsLBiyxQNXvP0unREPQIF+1z1k5l5Cx\n"
    "jkT67s0JuSUxshrHFSAefVf6kglPjR87ColpOQkCgYEAzvtmjisxacqneV8nn8mm\n"
    "hutYvKk2a90zBD79iwMTT0+aFGT/5e4A2wzrPTCtsV0z2o1aKHIVc6y01q2iev7t\n"
    "iVHRH2aBXfjfQOjNaM1wx/wtvYX1RLOXQ29l/wJ60AGhCs4qXvwxpnXlk8zMpWPS\n"
    "SYpfgX5cEhMVn4H0TjRHP0E=\n"
    "-----END PRIVATE KEY-----\n";

/** CRL signed by root_ca_cert_pem revoking revoked_leaf_cert_pem. */
inline constexpr char const* revoked_crl_pem =
    "-----BEGIN X509 CRL-----\n"
    "MIIBhzBxAgEBMA0GCSqGSIb3DQEBCwUAMBcxFTATBgNVBAMMDFRlc3QgUm9vdCBD\n"
    "QRcNMjYwNzA4MjAzNDU1WhcNMzYwNzA1MjAzNDU1WjAVMBMCAhAAFw0yNjA3MDgy\n"
    "MDM0NTVaoA8wDTALBgNVHRQEBAICEAAwDQYJKoZIhvcNAQELBQADggEBAGv/v6zT\n"
    "PHvnPndh9RffnHBKcOAGglPSP23SaMSvz4MfHYEliKNq13fuOfoMubs6nd0XLTBI\n"
    "1+cKgeVGFhwbfeFjprY/5F3VtYrt/aq0VMA396A+FfRgFsutS4FLRDFApseUm/+S\n"
    "AAuLKW+0hanr9rvDstDLudm+Hw3x8fVRRUuOIpVwZjsDJWP9Jz+aoLS2qDP74LaT\n"
    "SVuLkjawrA56SN5DdWsSVwsvS1re8JTMm5cYEypS8UWc+LMD+8hkAkWNGS65GoFf\n"
    "iAFKZ+1B7ulCCkvaiiH8pqkHfq7oZkMV2JFMrrEONkd79Opi1+N10TucEg5wC/KJ\n"
    "8122Qwh+h0NOgHk=\n"
    "-----END X509 CRL-----\n";

/** Create a server context whose key needs the password callback. */
inline tls_context
make_encrypted_key_server_context(bool& callback_invoked)
{
    tls_context ctx;
    require_ok(ctx.use_certificate(
        server_cert_pem,
        tls_file_format::pem));
    ctx.set_password_callback(
        [&callback_invoked](std::size_t, tls_password_purpose) {
            callback_invoked = true;
            return std::string(encrypted_key_password);
        });
    require_ok(ctx.use_private_key(
        encrypted_server_key_pem,
        tls_file_format::pem));
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::none));
    return ctx;
}

/** Create a context that requires peer verification but has no cert. */
inline tls_context
make_verify_no_cert_context()
{
    tls_context ctx;
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::require_peer));
    return ctx;
}

//
// Context Configuration Modes
//

enum class context_mode
{
    anon,         // Anonymous ciphers, no certificates
    shared_cert,  // Both use same context with server cert
    separate_cert // Server has cert, client trusts CA
};

/** Create client and server contexts for the given mode. */
inline std::pair<tls_context, tls_context>
make_contexts(context_mode mode)
{
    switch (mode)
    {
    case context_mode::anon:
        return {make_anon_context(), make_anon_context()};
    case context_mode::shared_cert:
    {
        auto ctx = make_server_context();
        require_ok(ctx.add_certificate_authority(
            ca_cert_pem));
        return {ctx, ctx};
    }
    case context_mode::separate_cert:
        return {make_client_context(), make_server_context()};
    }
    return {make_anon_context(), make_anon_context()};
}

//
// Test Coroutines
//

/** Check an error code, logging its identity on failure.

    `BOOST_TEST(!ec)` records only that the check failed; when triaging
    a one-off CI failure the category and value are what distinguish a
    transport error from an engine error.
*/
inline void
expect_ok(std::error_code const& ec, char const* what)
{
    if (ec)
        test_suite::log << what << " failed: [" << ec.category().name()
            << ":" << ec.value() << "] " << ec.message() << "\n";
    BOOST_TEST(!ec);
}

/** Test bidirectional data transfer on connected streams. */
template<typename StreamA, typename StreamB>
capy::task<>
test_stream(StreamA& a, StreamB& b)
{
    char buf[32] = {};

    // Write from a, read from b
    auto [ec1, n1] = co_await a.write_some(capy::const_buffer("hello", 5));
    expect_ok(ec1, "test_stream write a");
    BOOST_TEST_EQ(n1, 5u);

    auto [ec2, n2] =
        co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
    expect_ok(ec2, "test_stream read b");
    BOOST_TEST_EQ(n2, 5u);
    BOOST_TEST_EQ(std::string_view(buf, n2), "hello");

    // Write from b, read from a
    auto [ec3, n3] = co_await b.write_some(capy::const_buffer("world", 5));
    expect_ok(ec3, "test_stream write b");
    BOOST_TEST_EQ(n3, 5u);

    auto [ec4, n4] =
        co_await a.read_some(capy::mutable_buffer(buf, sizeof(buf)));
    expect_ok(ec4, "test_stream read a");
    BOOST_TEST_EQ(n4, 5u);
    BOOST_TEST_EQ(std::string_view(buf, n4), "world");
}

//
// Parameterized Test Runner
//

/** Run a complete TLS test: handshake, data transfer, shutdown.
    
    @param ioc          The io_context to use
    @param client_ctx   TLS context for the client
    @param server_ctx   TLS context for the server
    @param make_client  Factory: (io_stream&, context) -> TLS stream
    @param make_server  Factory: (io_stream&, context) -> TLS stream
*/
template<typename ClientStreamFactory, typename ServerStreamFactory>
void
run_tls_test(
    io_context& ioc,
    tls_context client_ctx,
    tls_context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server)
{
    auto [s1, s2] = corosio::test::make_socket_pair(ioc);

    auto client = make_client(s1, client_ctx);
    auto server = make_server(s2, server_ctx);

    // Store lambdas in named variables before invoking - anonymous lambda + immediate
    // invocation pattern [...](){}() can cause capture corruption with run_async
    auto client_task = [&client]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        expect_ok(ec, "run_tls_test client handshake");
    };

    auto server_task = [&server]() -> capy::task<> {
        auto [ec] = co_await server.handshake(tls_role::server);
        expect_ok(ec, "run_tls_test server handshake");
    };

    capy::run_async(ioc.get_executor())(client_task());
    capy::run_async(ioc.get_executor())(server_task());

    ioc.run();
    ioc.restart();

    // Bidirectional data transfer
    auto transfer_task = [&client, &server]() -> capy::task<> {
        co_await test_stream(client, server);
    };
    capy::run_async(ioc.get_executor())(transfer_task());

    ioc.run();

    // Skip TLS shutdown - bidirectional close_notify exchange deadlocks
    // in single-threaded io_context. This is a test environment limitation.
    s1.close();
    s2.close();
}

/** Run a TLS test without shutdown phase (for cross-implementation tests).

    TLS shutdown has known interoperability issues between implementations
    due to differing close_notify handling (bidirectional vs unidirectional,
    blocking vs non-blocking). Cross-impl tests verify handshake and data
    transfer; shutdown is skipped to avoid these documented friction points.
    
    @param ioc          The io_context to use
    @param client_ctx   TLS context for the client
    @param server_ctx   TLS context for the server
    @param make_client  Factory: (io_stream&, context) -> TLS stream
    @param make_server  Factory: (io_stream&, context) -> TLS stream
*/
template<typename ClientStreamFactory, typename ServerStreamFactory>
void
run_tls_test_no_shutdown(
    io_context& ioc,
    tls_context client_ctx,
    tls_context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server)
{
    auto [s1, s2] = corosio::test::make_socket_pair(ioc);

    auto client = make_client(s1, client_ctx);
    auto server = make_server(s2, server_ctx);

    // Store lambdas in named variables before invoking - anonymous lambda + immediate
    // invocation pattern [...](){}() can cause capture corruption with run_async
    auto client_task = [&client]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        BOOST_TEST(!ec);
    };

    auto server_task = [&server]() -> capy::task<> {
        auto [ec] = co_await server.handshake(tls_role::server);
        BOOST_TEST(!ec);
    };

    capy::run_async(ioc.get_executor())(client_task());
    capy::run_async(ioc.get_executor())(server_task());

    ioc.run();
    ioc.restart();

    // Bidirectional data transfer
    auto transfer_task = [&client, &server]() -> capy::task<> {
        co_await test_stream(client, server);
    };
    capy::run_async(ioc.get_executor())(transfer_task());

    ioc.run();

    // Skip TLS shutdown - just close sockets (like HTTP "connection: close")
    s1.close();
    s2.close();
}

/** Run a TLS test expecting handshake failure.

    Uses a failsafe deadline to handle the case where one side fails and
    the other blocks waiting for data. When the deadline elapses, sockets
    are closed to unblock any pending operations.
    
    @param ioc          The io_context to use
    @param client_ctx   TLS context for the client
    @param server_ctx   TLS context for the server
    @param make_client  Factory: (io_stream&, context) -> TLS stream
    @param make_server  Factory: (io_stream&, context) -> TLS stream
*/
template<typename ClientStreamFactory, typename ServerStreamFactory>
void
run_tls_test_fail(
    io_context& ioc,
    tls_context client_ctx,
    tls_context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server,
    std::error_code* client_ec_out = nullptr)
{
    auto [s1, s2] = corosio::test::make_socket_pair(ioc);

    auto client = make_client(s1, client_ctx);
    auto server = make_server(s2, server_ctx);

    bool client_failed = false;
    bool server_failed = false;
    bool client_done   = false;
    bool server_done   = false;

    // Failsafe deadline to unblock stuck handshakes; the still-running
    // side requests stop on it once both sides finish.
    std::stop_source failsafe_stop;

    // Store lambdas in named variables before invoking - anonymous lambda + immediate
    // invocation pattern [...](){}() can cause capture corruption with run_async
    auto client_task = [&client, &client_failed, &client_done, &server_done,
                        &failsafe_stop, &s1, &s2,
                        client_ec_out]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        if (client_ec_out)
            *client_ec_out = ec;
        if (ec)
        {
            client_failed = true;
            // Cancel then close sockets to unblock server immediately (IOCP needs cancel)
            if (s1.is_open())
            {
                s1.cancel();
                s1.close();
            }
            if (s2.is_open())
            {
                s2.cancel();
                s2.close();
            }
        }
        client_done = true;
        if (server_done)
            failsafe_stop.request_stop();
    };

    auto server_task = [&server, &server_failed, &server_done, &client_done,
                        &failsafe_stop, &s1, &s2]() -> capy::task<> {
        auto [ec] = co_await server.handshake(tls_role::server);
        if (ec)
        {
            server_failed = true;
            // Cancel then close sockets to unblock client immediately (IOCP needs cancel)
            if (s1.is_open())
            {
                s1.cancel();
                s1.close();
            }
            if (s2.is_open())
            {
                s2.cancel();
                s2.close();
            }
        }
        server_done = true;
        if (client_done)
            failsafe_stop.request_stop();
    };

    bool failsafe_hit = false;
    auto timeout_task = [&failsafe_hit, &s1, &s2]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(200 * failsafe_scale));
        if (!ec)
        {
            failsafe_hit = true;
            // Deadline elapsed - cancel pending operations then close sockets
            if (s1.is_open())
            {
                s1.cancel();
                s1.close();
            }
            if (s2.is_open())
            {
                s2.cancel();
                s2.close();
            }
        }
    };

    capy::run_async(ioc.get_executor())(client_task());
    capy::run_async(ioc.get_executor())(server_task());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        timeout_task());

    ioc.run();
    BOOST_TEST(!failsafe_hit); // failsafe timeout should not be hit

    // At least one side should have failed
    BOOST_TEST(client_failed || server_failed);

    s1.close();
    s2.close();
}

/** Run a TLS shutdown test with graceful close_notify.

    Tests that one side can initiate TLS shutdown (sends close_notify)
    and the other side receives EOF. Uses unidirectional shutdown to
    avoid deadlock in single-threaded io_context.

    Note: TLS shutdown in a single-threaded context can deadlock when both
    sides wait for each other. We use a timeout to detect and recover from
    potential deadlocks.
    
    @param ioc          The io_context to use
    @param client_ctx   TLS context for the client
    @param server_ctx   TLS context for the server
    @param make_client  Factory: (io_stream&, context) -> TLS stream
    @param make_server  Factory: (io_stream&, context) -> TLS stream
*/
template<typename ClientStreamFactory, typename ServerStreamFactory>
void
run_tls_shutdown_test(
    io_context& ioc,
    tls_context client_ctx,
    tls_context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server)
{
    auto [s1, s2] = corosio::test::make_socket_pair(ioc);

    auto client = make_client(s1, client_ctx);
    auto server = make_server(s2, server_ctx);

    // Handshake phase
    auto client_hs = [&client]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        BOOST_TEST(!ec);
    };

    auto server_hs = [&server]() -> capy::task<> {
        auto [ec] = co_await server.handshake(tls_role::server);
        BOOST_TEST(!ec);
    };

    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());

    ioc.run();
    ioc.restart();

    // Data transfer phase
    auto transfer_task = [&client, &server]() -> capy::task<> {
        co_await test_stream(client, server);
    };
    capy::run_async(ioc.get_executor())(transfer_task());

    ioc.run();
    ioc.restart();

    // Shutdown phase: client sends close_notify, server reads EOF then closes socket.
    // Server closing the socket causes client's shutdown to complete.
    bool done = false;

    // Failsafe deadline in case of bugs
    std::stop_source failsafe_stop;

    auto client_shutdown = [&client, &done, &failsafe_stop]() -> capy::task<> {
        auto [ec] = co_await client.shutdown();
        done      = true;
        failsafe_stop.request_stop();
        BOOST_TEST(
            !ec || ec == capy::cond::stream_truncated ||
            ec == capy::cond::eof || ec == capy::cond::canceled);
    };

    auto server_read_then_close = [&server, &s2]() -> capy::task<> {
        char buf[32];
        auto [ec, n] =
            co_await server.read_some(capy::mutable_buffer(buf, sizeof(buf)));
        BOOST_TEST(
            ec == capy::cond::eof || ec == capy::cond::stream_truncated ||
            ec == capy::cond::canceled);
        // Close socket to unblock client's shutdown
        s2.cancel();
        s2.close();
    };

    bool failsafe_hit  = false;
    auto failsafe_task = [&failsafe_hit, &done, &s1, &s2]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(200 * failsafe_scale));
        if (!ec && !done)
        {
            failsafe_hit = true;
            if (s1.is_open())
            {
                s1.cancel();
                s1.close();
            }
            if (s2.is_open())
            {
                s2.cancel();
                s2.close();
            }
        }
    };

    capy::run_async(ioc.get_executor())(client_shutdown());
    capy::run_async(ioc.get_executor())(server_read_then_close());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());

    ioc.run();
    BOOST_TEST(!failsafe_hit); // failsafe timeout should not be hit
    if (s1.is_open())
        s1.close();
    if (s2.is_open())
        s2.close();
}

/** Run a test for stream truncation (socket close without TLS shutdown).

    Tests that when one side closes the underlying socket without
    performing TLS shutdown, the other side receives stream_truncated.
    
    @param ioc          The io_context to use
    @param client_ctx   TLS context for the client
    @param server_ctx   TLS context for the server
    @param make_client  Factory: (io_stream&, context) -> TLS stream
    @param make_server  Factory: (io_stream&, context) -> TLS stream
*/
template<typename ClientStreamFactory, typename ServerStreamFactory>
void
run_tls_truncation_test(
    io_context& ioc,
    tls_context client_ctx,
    tls_context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server)
{
    auto [s1, s2] = corosio::test::make_socket_pair(ioc);

    auto client = make_client(s1, client_ctx);
    auto server = make_server(s2, server_ctx);

    // Handshake phase
    auto client_hs = [&client]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        BOOST_TEST(!ec);
    };

    auto server_hs = [&server]() -> capy::task<> {
        auto [ec] = co_await server.handshake(tls_role::server);
        BOOST_TEST(!ec);
    };

    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());

    ioc.run();
    ioc.restart();

    // Data transfer phase
    auto transfer_task = [&client, &server]() -> capy::task<> {
        co_await test_stream(client, server);
    };
    capy::run_async(ioc.get_executor())(transfer_task());

    ioc.run();
    ioc.restart();

    // Truncation test with timeout protection
    bool read_done    = false;
    bool failsafe_hit = false;

    // Deadline to prevent deadlock
    std::stop_source failsafe_stop;

    auto client_close = [&s1, &s2]() -> capy::task<> {
        // Cancel and close underlying socket without TLS shutdown (IOCP needs cancel)
        s1.cancel();
        s1.close();
        // Wake the peer read path immediately after abrupt close.
        if (s2.is_open())
            s2.cancel();
        co_return;
    };

    auto server_read_truncated = [&server, &read_done,
                                  &failsafe_stop]() -> capy::task<> {
        char buf[32];
        auto [ec, n] =
            co_await server.read_some(capy::mutable_buffer(buf, sizeof(buf)));
        read_done = true;
        failsafe_stop.request_stop();
        // Under IOCP + TLS backends, abrupt peer close may surface as an error
        // or as a zero-byte completion after cancellation/close unblocks the read.
        BOOST_TEST(!!ec || n == 0);
    };

    auto timeout_task = [&failsafe_hit, &s1, &s2]() -> capy::task<> {
        // IOCP peer-close propagation can be bursty under TLS backends.
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(750 * failsafe_scale));
        if (!ec)
        {
            failsafe_hit = true;
            // Deadline elapsed - cancel pending operations (check if still open)
            if (s1.is_open())
            {
                s1.cancel();
                s1.close();
            }
            if (s2.is_open())
            {
                s2.cancel();
                s2.close();
            }
        }
    };

    capy::run_async(ioc.get_executor())(client_close());
    capy::run_async(ioc.get_executor())(server_read_truncated());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        timeout_task());

    ioc.run();
    BOOST_TEST(read_done);
    if (s1.is_open())
        s1.close();
    if (s2.is_open())
        s2.close();
}

//
// Additional Context Helpers for Extended Tests
//

/** Create a server context using chain certificates (signed by intermediate CA). */
inline tls_context
make_chain_server_context()
{
    tls_context ctx;
    require_ok(ctx.use_certificate(
        chain_server_cert_pem,
        tls_file_format::pem));
    require_ok(ctx.use_private_key(
        chain_server_key_pem,
        tls_file_format::pem));
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::none));
    return ctx;
}

/** Create a server context with full certificate chain.
    Server sends entity cert + intermediate cert, allowing client to verify
    chain up to root CA. Uses use_certificate_chain() which expects the full
    chain (entity + intermediates) in a single PEM blob. */
inline tls_context
make_fullchain_server_context()
{
    tls_context ctx;
    // use_certificate_chain expects entity cert followed by intermediate(s)
    require_ok(ctx.use_certificate_chain(
        server_fullchain_pem));
    require_ok(ctx.use_private_key(
        chain_server_key_pem,
        tls_file_format::pem));
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::none));
    return ctx;
}

/** Create a client context that trusts ONLY the root CA (for chain tests).
    Server must send intermediate cert in chain for verification to succeed. */
inline tls_context
make_rootonly_client_context()
{
    tls_context ctx;
    require_ok(ctx.add_certificate_authority(
        root_ca_cert_pem));
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::peer));
    return ctx;
}

/** Create a client context that trusts the root CA (for chain tests). */
inline tls_context
make_chain_client_context()
{
    tls_context ctx;
    // Trust both root and intermediate CA for chain verification
    require_ok(ctx.add_certificate_authority(
        root_ca_cert_pem));
    require_ok(ctx.add_certificate_authority(
        intermediate_cert_pem));
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::peer));
    return ctx;
}

/** Create a server context with an EXPIRED certificate.
    The certificate expired on Jan 2, 2020. */
inline tls_context
make_expired_server_context()
{
    tls_context ctx;
    require_ok(ctx.use_certificate(
        expired_cert_pem,
        tls_file_format::pem));
    require_ok(ctx.use_private_key(
        expired_key_pem,
        tls_file_format::pem));
    return ctx;
}

/** Create a client context that trusts the expired cert's self-signed CA.
    Used with make_expired_server_context() to test expiry validation. */
inline tls_context
make_expired_client_context()
{
    tls_context ctx;
    // Trust the expired cert as its own CA (self-signed)
    require_ok(ctx.add_certificate_authority(
        expired_cert_pem));
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::peer));
    return ctx;
}

/** Create a server context with wrong hostname (CN=wrong.example.com). */
inline tls_context
make_wrong_host_server_context()
{
    tls_context ctx;
    require_ok(ctx.use_certificate(
        wrong_host_cert_pem,
        tls_file_format::pem));
    require_ok(ctx.use_private_key(
        wrong_host_key_pem,
        tls_file_format::pem));
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::none));
    return ctx;
}

/** Create a client context for mTLS (with client certificate). */
inline tls_context
make_mtls_client_context()
{
    tls_context ctx;
    require_ok(ctx.use_certificate(
        client_cert_pem,
        tls_file_format::pem));
    require_ok(ctx.use_private_key(
        client_key_pem,
        tls_file_format::pem));
    // Trust both root and intermediate CA for chain verification
    require_ok(ctx.add_certificate_authority(
        root_ca_cert_pem));
    require_ok(ctx.add_certificate_authority(
        intermediate_cert_pem));
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::peer));
    return ctx;
}

/** Create a server context that requires client certificates (mTLS). */
inline tls_context
make_mtls_server_context()
{
    tls_context ctx;
    require_ok(ctx.use_certificate(
        chain_server_cert_pem,
        tls_file_format::pem));
    require_ok(ctx.use_private_key(
        chain_server_key_pem,
        tls_file_format::pem));
    // Trust both root and intermediate CA for chain verification
    require_ok(ctx.add_certificate_authority(
        root_ca_cert_pem));
    require_ok(ctx.add_certificate_authority(
        intermediate_cert_pem));
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::require_peer));
    return ctx;
}

/** Create a client context that trusts the untrusted CA (for verification failures). */
inline tls_context
make_untrusted_ca_client_context()
{
    tls_context ctx;
    require_ok(ctx.add_certificate_authority(
        untrusted_ca_cert_pem));
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::peer));
    return ctx;
}

/** Create an mTLS client context with INVALID client certificate.
    Uses server_cert_pem (self-signed) which is NOT signed by the
    intermediate/root CA that make_mtls_server_context() trusts. */
inline tls_context
make_invalid_mtls_client_context()
{
    tls_context ctx;
    // Use the self-signed server cert as client cert - server won't trust it
    require_ok(ctx.use_certificate(
        server_cert_pem,
        tls_file_format::pem));
    require_ok(ctx.use_private_key(
        server_key_pem,
        tls_file_format::pem));
    // Trust the chain CAs so we can verify server
    require_ok(ctx.add_certificate_authority(
        root_ca_cert_pem));
    require_ok(ctx.add_certificate_authority(
        intermediate_cert_pem));
    require_ok(ctx.set_verify_mode(
        tls_verify_mode::peer));
    return ctx;
}

//
// Connection Reset Test
//

/** Run a test for connection reset during handshake.

    Tests that when the underlying socket is closed abruptly during
    the TLS handshake, the operation fails with an appropriate error.
    
    @param ioc          The io_context to use
    @param client_ctx   TLS context for the client
    @param server_ctx   TLS context for the server
    @param make_client  Factory: (io_stream&, context) -> TLS stream
    @param make_server  Factory: (io_stream&, context) -> TLS stream
*/
template<typename ClientStreamFactory, typename ServerStreamFactory>
void
run_connection_reset_test(
    io_context& ioc,
    tls_context client_ctx,
    tls_context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server)
{
    auto [s1, s2] = corosio::test::make_socket_pair(ioc);

    auto client = make_client(s1, client_ctx);
    auto server = make_server(s2, server_ctx);

    bool client_failed = false;

    // Timeout protection
    std::stop_source failsafe_stop;

    auto client_task = [&client, &client_failed,
                        &failsafe_stop]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        // Should fail because server closed socket
        if (ec)
            client_failed = true;
        failsafe_stop.request_stop();
    };

    // Server closes socket immediately (simulates connection reset)
    auto server_task = [&s2]() -> capy::task<> {
        // Cancel and close socket to simulate connection reset (IOCP needs cancel)
        s2.cancel();
        s2.close();
        co_return;
    };

    bool failsafe_hit = false;
    auto timeout_task = [&failsafe_hit, &s1]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(200 * failsafe_scale));
        if (!ec && s1.is_open())
        {
            failsafe_hit = true;
            s1.cancel();
            s1.close();
        }
    };

    capy::run_async(ioc.get_executor())(client_task());
    capy::run_async(ioc.get_executor())(server_task());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        timeout_task());

    ioc.run();

    BOOST_TEST(!failsafe_hit); // failsafe timeout should not be hit
    BOOST_TEST(client_failed);

    if (s1.is_open())
        s1.close();
    if (s2.is_open())
        s2.close();
}

//
// Stop Token Cancellation Test
//

/** Run a test for stop token cancellation during handshake.

    Tests that cooperative cancellation via std::stop_token correctly
    interrupts a TLS handshake when stop is requested.
    
    The test is deterministic: the server waits for client to send data
    (ClientHello), proving the client has started, then triggers cancellation.
    
    @param ioc          The io_context to use
    @param client_ctx   TLS context for the client
    @param server_ctx   TLS context for the server
    @param make_client  Factory: (io_stream&, context) -> TLS stream
    @param make_server  Factory: (io_stream&, context) -> TLS stream
*/
template<typename ClientStreamFactory, typename ServerStreamFactory>
void
run_stop_token_handshake_test(
    io_context& ioc,
    tls_context client_ctx,
    tls_context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server)
{
    auto [s1, s2] = corosio::test::make_socket_pair(ioc);

    auto client = make_client(s1, client_ctx);
    auto server = make_server(s2, server_ctx);

    std::stop_source stop_src;
    bool client_got_error = false;

    // Failsafe deadline to prevent infinite hang if cancellation doesn't work
    // 2000ms allows headroom for CI with coverage instrumentation
    std::stop_source failsafe_stop;

    // Client handshake - will be cancelled while waiting for ServerHello
    auto client_task = [&client, &client_got_error,
                        &failsafe_stop]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        if (ec)
            client_got_error = true;
        failsafe_stop.request_stop();
    };

    // Server waits for ClientHello then cancels - deterministic synchronization
    auto server_task = [&s2, &stop_src]() -> capy::task<> {
        // Wait for client to send ClientHello (proves client started handshake)
        char buf[1];
        std::ignore = co_await s2.read_some(capy::mutable_buffer(buf, 1));
        // Client is now blocked waiting for ServerHello - cancel it
        stop_src.request_stop();
    };

    bool failsafe_hit  = false;
    auto failsafe_task = [&failsafe_hit, &s1, &s2]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(2000 * failsafe_scale));
        if (!ec)
        {
            failsafe_hit = true;
            if (s1.is_open())
            {
                s1.cancel();
                s1.close();
            }
            if (s2.is_open())
            {
                s2.cancel();
                s2.close();
            }
        }
    };
    capy::run_async(ioc.get_executor(), stop_src.get_token())(client_task());
    capy::run_async(ioc.get_executor())(server_task());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
    ioc.run();

    BOOST_TEST(!failsafe_hit); // failsafe timeout should not be hit
    BOOST_TEST(client_got_error);

    if (s1.is_open())
        s1.close();
    if (s2.is_open())
        s2.close();
}

/** Run a test for stop token cancellation during read.

    Tests that cooperative cancellation via std::stop_token correctly
    interrupts a TLS read operation when stop is requested.
    
    The test is deterministic: after handshake, the server triggers
    cancellation immediately since the client will be blocked waiting
    for data the server never sends.
*/
template<typename ClientStreamFactory, typename ServerStreamFactory>
void
run_stop_token_read_test(
    io_context& ioc,
    tls_context client_ctx,
    tls_context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server)
{
    auto [s1, s2] = corosio::test::make_socket_pair(ioc);

    auto client = make_client(s1, client_ctx);
    auto server = make_server(s2, server_ctx);

    // Handshake phase
    auto client_hs = [&client]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        BOOST_TEST(!ec);
    };

    auto server_hs = [&server]() -> capy::task<> {
        auto [ec] = co_await server.handshake(tls_role::server);
        BOOST_TEST(!ec);
    };

    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());

    ioc.run();
    ioc.restart();

    // Read cancellation phase
    std::stop_source stop_src;
    bool read_got_error = false;

    // Failsafe deadline - 2000ms allows headroom for CI with coverage instrumentation
    std::stop_source failsafe_stop;

    auto client_read = [&client, &read_got_error,
                        &failsafe_stop]() -> capy::task<> {
        char buf[32];
        auto [ec, n] =
            co_await client.read_some(capy::mutable_buffer(buf, sizeof(buf)));
        if (ec)
            read_got_error = true;
        failsafe_stop.request_stop();
    };

    // Server triggers cancellation immediately - client will block on read
    // since server never sends data. This is deterministic because the
    // client read is queued first and will suspend waiting for socket data.
    auto server_cancel = [&stop_src]() -> capy::task<> {
        stop_src.request_stop();
        co_return;
    };

    bool failsafe_hit  = false;
    auto failsafe_task = [&failsafe_hit, &s1, &s2]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(2000 * failsafe_scale));
        if (!ec)
        {
            failsafe_hit = true;
            if (s1.is_open())
            {
                s1.cancel();
                s1.close();
            }
            if (s2.is_open())
            {
                s2.cancel();
                s2.close();
            }
        }
    };
    capy::run_async(ioc.get_executor(), stop_src.get_token())(client_read());
    capy::run_async(ioc.get_executor())(server_cancel());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
    ioc.run();

    BOOST_TEST(!failsafe_hit); // failsafe timeout should not be hit
    BOOST_TEST(read_got_error);

    if (s1.is_open())
        s1.close();
    if (s2.is_open())
        s2.close();
}

/** How the shutdown read is cancelled in run_shutdown_cancel_test. */
enum class shutdown_cancel_mode
{
    // Cancel the whole task via std::stop_token. The pending socket read then
    // completes with the generic std::errc::operation_canceled.
    stop_token,
    // Cancel just the socket via socket.cancel(). The pending read completes
    // with the capy-category capy::error::canceled.
    socket_cancel
};

/** Run a test for cancellation during TLS shutdown.

    Regression test for cppalliance/corosio#301: cancelling a TLS task while it
    is blocked reading the peer's close_notify during shutdown must surface
    cond::canceled, not cond::stream_truncated (OpenSSL) and not a silent
    success (WolfSSL).

    Both cancel representations are exercised via @p mode (see
    shutdown_cancel_mode): OpenSSL's normalize historically folded the
    capy-category form into stream_truncated, so only socket_cancel reproduced
    that bug, while WolfSSL swallowed either form. cond::canceled matches both.

    The test is deterministic: the client initiates shutdown (sending its own
    close_notify); the server reads that close_notify (proving the client's
    write has landed) and then yields via a short delay so the io_context
    resumes the client past its flush and parks it blocking on the peer read.
    Only then does the server trigger cancellation, guaranteeing it hits the
    shutdown *read*. The server never sends its own close_notify and never
    closes the socket, so the client stays blocked until cancelled.
*/
template<typename ClientStreamFactory, typename ServerStreamFactory>
void
run_shutdown_cancel_test(
    io_context& ioc,
    tls_context client_ctx,
    tls_context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server,
    shutdown_cancel_mode mode)
{
    auto [s1, s2] = corosio::test::make_socket_pair(ioc);

    auto client = make_client(s1, client_ctx);
    auto server = make_server(s2, server_ctx);

    // Handshake phase
    auto client_hs = [&client]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        BOOST_TEST(!ec);
    };

    auto server_hs = [&server]() -> capy::task<> {
        auto [ec] = co_await server.handshake(tls_role::server);
        BOOST_TEST(!ec);
    };

    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());

    ioc.run();
    ioc.restart();

    // Shutdown cancellation phase
    std::stop_source stop_src;
    std::error_code shutdown_ec;
    bool shutdown_done = false;

    // Failsafe deadline - 2000ms allows headroom for CI with coverage instrumentation
    std::stop_source failsafe_stop;

    auto client_shutdown = [&client, &shutdown_ec, &shutdown_done,
                            &failsafe_stop]() -> capy::task<> {
        auto [ec]     = co_await client.shutdown();
        shutdown_ec   = ec;
        shutdown_done = true;
        failsafe_stop.request_stop();
    };

    // See the function docstring for why the drain + short delay is needed to
    // land the cancellation on the shutdown read rather than the close_notify
    // write.
    auto server_drain_then_cancel = [&server, &stop_src, &s1,
                                     mode]() -> capy::task<> {
        char buf[64];
        [[maybe_unused]] auto [ec, n] =
            co_await server.read_some(capy::mutable_buffer(buf, sizeof(buf)));
        [[maybe_unused]] auto [dec] = co_await corosio::delay(
            std::chrono::milliseconds(20 * failsafe_scale));
        if (mode == shutdown_cancel_mode::socket_cancel)
            s1.cancel();
        else
            stop_src.request_stop();
    };

    bool failsafe_hit  = false;
    auto failsafe_task = [&failsafe_hit, &shutdown_done, &s1,
                          &s2]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(2000 * failsafe_scale));
        if (!ec && !shutdown_done)
        {
            failsafe_hit = true;
            if (s1.is_open())
            {
                s1.cancel();
                s1.close();
            }
            if (s2.is_open())
            {
                s2.cancel();
                s2.close();
            }
        }
    };

    // The client task carries the stop token in both modes; only stop_token
    // mode requests a stop, so socket_cancel mode is unaffected by it.
    capy::run_async(ioc.get_executor(), stop_src.get_token())(
        client_shutdown());
    capy::run_async(ioc.get_executor())(server_drain_then_cancel());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
    ioc.run();

    BOOST_TEST(!failsafe_hit); // failsafe timeout should not be hit
    BOOST_TEST(shutdown_ec == capy::cond::canceled);

    if (s1.is_open())
        s1.close();
    if (s2.is_open())
        s2.close();
}

/** Run a test for stop token cancellation during write.

    Tests that cooperative cancellation via std::stop_token correctly
    interrupts a TLS write operation when stop is requested.
    
    The test is deterministic: after handshake, the server waits for
    some data to arrive (proving the client started writing), then
    triggers cancellation.
*/
template<typename ClientStreamFactory, typename ServerStreamFactory>
void
run_stop_token_write_test(
    io_context& ioc,
    tls_context client_ctx,
    tls_context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server)
{
    auto [s1, s2] = corosio::test::make_socket_pair(ioc);

    auto client = make_client(s1, client_ctx);
    auto server = make_server(s2, server_ctx);

    // Handshake phase
    auto client_hs = [&client]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        BOOST_TEST(!ec);
    };

    auto server_hs = [&server]() -> capy::task<> {
        auto [ec] = co_await server.handshake(tls_role::server);
        BOOST_TEST(!ec);
    };

    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());

    ioc.run();
    ioc.restart();

    // Write cancellation phase - fill socket buffer to cause blocking
    std::stop_source stop_src;
    bool write_got_error = false;

    // Large buffer to fill socket buffer and cause blocking
    std::vector<char> large_buf(std::size_t{1024} * 1024, 'X');

    // Failsafe deadline - 2000ms allows headroom for CI with coverage instrumentation
    std::stop_source failsafe_stop;

    auto client_write = [&client, &large_buf, &write_got_error,
                         &failsafe_stop]() -> capy::task<> {
        // Write in loop until cancelled or error
        for (int i = 0; i < 100; ++i)
        {
            auto [ec, n] = co_await client.write_some(
                capy::const_buffer(large_buf.data(), large_buf.size()));
            if (ec)
            {
                write_got_error = true;
                failsafe_stop.request_stop();
                co_return;
            }
        }
        failsafe_stop.request_stop();
    };

    // Server waits for data then cancels - deterministic synchronization
    auto server_cancel = [&s2, &stop_src]() -> capy::task<> {
        // Wait for client to send some data (proves client started writing)
        char buf[1];
        std::ignore = co_await s2.read_some(capy::mutable_buffer(buf, 1));
        // Client is now writing - cancel it
        stop_src.request_stop();
    };

    bool failsafe_hit  = false;
    auto failsafe_task = [&failsafe_hit, &s1, &s2]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(2000 * failsafe_scale));
        if (!ec)
        {
            failsafe_hit = true;
            if (s1.is_open())
            {
                s1.cancel();
                s1.close();
            }
            if (s2.is_open())
            {
                s2.cancel();
                s2.close();
            }
        }
    };
    capy::run_async(ioc.get_executor(), stop_src.get_token())(client_write());
    capy::run_async(ioc.get_executor())(server_cancel());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
    ioc.run();

    BOOST_TEST(!failsafe_hit); // failsafe timeout should not be hit
    BOOST_TEST(write_got_error);

    if (s1.is_open())
        s1.close();
    if (s2.is_open())
        s2.close();
}

//
// Socket Error Propagation Test
//

/** Run a test for socket.cancel() error propagation.

    Tests that calling socket.cancel() while TLS is blocked on socket I/O
    correctly propagates the error through the TLS layer.
    
    The test is deterministic: the server waits for client to send data
    (ClientHello), proving the client has started, then cancels the socket.
*/
template<typename ClientStreamFactory, typename ServerStreamFactory>
void
run_socket_cancel_test(
    io_context& ioc,
    tls_context client_ctx,
    tls_context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server)
{
    auto [s1, s2] = corosio::test::make_socket_pair(ioc);

    auto client = make_client(s1, client_ctx);
    auto server = make_server(s2, server_ctx);

    bool client_got_error = false;

    // Failsafe deadline - 2000ms allows headroom for CI with coverage instrumentation
    std::stop_source failsafe_stop;

    // Client starts handshake - will be cancelled
    auto client_task = [&client, &client_got_error,
                        &failsafe_stop]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        if (ec)
            client_got_error = true;
        failsafe_stop.request_stop();
    };

    // Server waits for ClientHello then cancels - deterministic synchronization
    auto server_task = [&s1, &s2]() -> capy::task<> {
        // Wait for client to send ClientHello (proves client started handshake)
        char buf[1];
        std::ignore = co_await s2.read_some(capy::mutable_buffer(buf, 1));
        // Client is now blocked waiting for ServerHello - cancel its socket
        s1.cancel();
    };

    bool failsafe_hit  = false;
    auto failsafe_task = [&failsafe_hit, &s1, &s2]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(2000 * failsafe_scale));
        if (!ec)
        {
            failsafe_hit = true;
            if (s1.is_open())
            {
                s1.cancel();
                s1.close();
            }
            if (s2.is_open())
            {
                s2.cancel();
                s2.close();
            }
        }
    };
    capy::run_async(ioc.get_executor())(client_task());
    capy::run_async(ioc.get_executor())(server_task());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
    ioc.run();

    BOOST_TEST(!failsafe_hit); // failsafe timeout should not be hit
    BOOST_TEST(client_got_error);

    if (s1.is_open())
        s1.close();
    if (s2.is_open())
        s2.close();
}

} // namespace boost::corosio::test

#endif
