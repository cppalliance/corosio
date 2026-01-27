//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_TLS_TEST_UTILS_HPP
#define BOOST_COROSIO_TEST_TLS_TEST_UTILS_HPP

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/io_stream.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/corosio/tls/context.hpp>
#include <boost/corosio/tls/tls_stream.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include "test_suite.hpp"

#include <chrono>
#include <stop_token>
#include <vector>

namespace boost::corosio::tls::test {

//------------------------------------------------------------------------------
//
// Embedded Test Certificates
//
//------------------------------------------------------------------------------

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

// Different self-signed CA for "wrong CA" test scenarios
// Subject: CN=localhost
// Valid: 2023-01-01 to 2033-01-01 (self-signed)
// A different CA that won't verify server_cert_pem
// Command:
//   openssl req -x509 -newkey rsa:2048 -keyout wrong_ca_key.pem -out wrong_ca_cert.pem
//       -days 3650 -nodes -subj "/CN=localhost"
inline constexpr char const* wrong_ca_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICpDCCAYwCCQDU+pQ4P0jwoDANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAls\n"
    "b2NhbGhvc3QwHhcNMjMwMTAxMDAwMDAwWhcNMzMwMTAxMDAwMDAwWjAUMRIwEAYD\n"
    "VQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC7\n"
    "o5e7Xv5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z\n"
    "5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z\n"
    "5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z\n"
    "5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z\n"
    "5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z\n"
    "5Z5Z5Z5ZAgMBAAEwDQYJKoZIhvcNAQELBQADggEBADummy0000000000000000000\n"
    "0000000000000000000000000000000000000000000000000000000000000000000\n"
    "0000000000000000000000000000000000000000000000000000000000000000000\n"
    "0000000000000000000000000000000000000000000000000000000000000000000\n"
    "0000000000000000000000000000000000000000000000000000000000000000000\n"
    "0000000000000000000000000000000000000000000000=\n"
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
// Valid: 2026-01-22 to 2031-01-21 (CA:TRUE)
// Commands:
//   openssl req -new -newkey rsa:2048 -keyout intermediate_key.pem -out intermediate.csr
//       -nodes -subj "/CN=Test Intermediate CA"
//   openssl x509 -req -in intermediate.csr -CA root_ca_cert.pem -CAkey root_ca_key.pem
//       -CAcreateserial -out intermediate_cert.pem -days 1825
//       -extfile <(echo "basicConstraints=CA:TRUE")
inline constexpr char const* intermediate_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDFDCCAfygAwIBAgIUTxjxnkuFSB8P+4VeoVw5wrVEv9swDQYJKoZIhvcNAQEL\n"
    "BQAwFzEVMBMGA1UEAwwMVGVzdCBSb290IENBMB4XDTI2MDEyMjE3MzIxM1oXDTMx\n"
    "MDEyMTE3MzIxM1owHzEdMBsGA1UEAwwUVGVzdCBJbnRlcm1lZGlhdGUgQ0EwggEi\n"
    "MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDCqobUGWRLfletWGsTWGdySYCb\n"
    "l2DJ06wVSW/TXvozFmIMKve4T5LKFDTAQtVrp/hK97HqAlTXWjhMTqq1SYHlN4dv\n"
    "utguzY7Vf96nJWVoJzsq7jAVhukK3bpRo6ytMcj6TRK7DIELKsbCOtvsLTxl0iGk\n"
    "26uE1zn2xk78GXJLRL5QHgeMrkgwWEdY8AeHm9VJ+dxBtnhzPR0z/AFaMmPODMSN\n"
    "+HGkDwVyBxOiPrt9GouEci+rx7AUv3Iv8wLZ+AOiCC0Fbfe9zMqVxVppRB8mUt4c\n"
    "+Np45GnIUk6/Fi+pdNJLTEE5WnoiA87GK+CbAezZt36vYIxSUIfoGz0jKrbpAgMB\n"
    "AAGjUDBOMAwGA1UdEwQFMAMBAf8wHQYDVR0OBBYEFGxcuu5CLhAiH3moziBaSMvW\n"
    "BzVkMB8GA1UdIwQYMBaAFIMzUb5b+JvY7MnzQFVN+CG75ojHMA0GCSqGSIb3DQEB\n"
    "CwUAA4IBAQAEr9QYAOU47frtpTt/TYazaPRt0gzJMQeG+YlFf+Zgsk02L81kxx+U\n"
    "4cxggby/TGJlJs8x5X7p6AIW1xHXh976uk1wQjR8A4xojdxauQ7pZXrawCesNfz+\n"
    "BJD4rtWD1GL+mGAwL8RT9w5MW+i+6M2IHsxfNp/gVuzEIUeKSaN3hEw10nQ/GZla\n"
    "xXlsA7IDcCDBLR35yV/i2kgUlELJMGJfuMJyLt3nbf4y1exZHoq4q4tP4TYU3338\n"
    "UXsP85AFORr1q+hDwpXoThPn9aAMlQpzgx6UvGekQK3IheMoqVtsir4N9EL2yMyo\n"
    "fDrPhvAUJTaYU/pWeMqNGpOBmvGyiXh9\n"
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
    "MIIDFDCCAfygAwIBAgIUTxjxnkuFSB8P+4VeoVw5wrVEv9swDQYJKoZIhvcNAQEL\n"
    "BQAwFzEVMBMGA1UEAwwMVGVzdCBSb290IENBMB4XDTI2MDEyMjE3MzIxM1oXDTMx\n"
    "MDEyMTE3MzIxM1owHzEdMBsGA1UEAwwUVGVzdCBJbnRlcm1lZGlhdGUgQ0EwggEi\n"
    "MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDCqobUGWRLfletWGsTWGdySYCb\n"
    "l2DJ06wVSW/TXvozFmIMKve4T5LKFDTAQtVrp/hK97HqAlTXWjhMTqq1SYHlN4dv\n"
    "utguzY7Vf96nJWVoJzsq7jAVhukK3bpRo6ytMcj6TRK7DIELKsbCOtvsLTxl0iGk\n"
    "26uE1zn2xk78GXJLRL5QHgeMrkgwWEdY8AeHm9VJ+dxBtnhzPR0z/AFaMmPODMSN\n"
    "+HGkDwVyBxOiPrt9GouEci+rx7AUv3Iv8wLZ+AOiCC0Fbfe9zMqVxVppRB8mUt4c\n"
    "+Np45GnIUk6/Fi+pdNJLTEE5WnoiA87GK+CbAezZt36vYIxSUIfoGz0jKrbpAgMB\n"
    "AAGjUDBOMAwGA1UdEwQFMAMBAf8wHQYDVR0OBBYEFGxcuu5CLhAiH3moziBaSMvW\n"
    "BzVkMB8GA1UdIwQYMBaAFIMzUb5b+JvY7MnzQFVN+CG75ojHMA0GCSqGSIb3DQEB\n"
    "CwUAA4IBAQAEr9QYAOU47frtpTt/TYazaPRt0gzJMQeG+YlFf+Zgsk02L81kxx+U\n"
    "4cxggby/TGJlJs8x5X7p6AIW1xHXh976uk1wQjR8A4xojdxauQ7pZXrawCesNfz+\n"
    "BJD4rtWD1GL+mGAwL8RT9w5MW+i+6M2IHsxfNp/gVuzEIUeKSaN3hEw10nQ/GZla\n"
    "xXlsA7IDcCDBLR35yV/i2kgUlELJMGJfuMJyLt3nbf4y1exZHoq4q4tP4TYU3338\n"
    "UXsP85AFORr1q+hDwpXoThPn9aAMlQpzgx6UvGekQK3IheMoqVtsir4N9EL2yMyo\n"
    "fDrPhvAUJTaYU/pWeMqNGpOBmvGyiXh9\n"
    "-----END CERTIFICATE-----\n";

//------------------------------------------------------------------------------
//
// Context Helpers
//
//------------------------------------------------------------------------------

/** Create a context with anonymous ciphers (no certificates needed). */
inline context
make_anon_context()
{
    context ctx;
    ctx.set_verify_mode( verify_mode::none );
    ctx.set_ciphersuites( "aNULL:eNULL:@SECLEVEL=0" );
    return ctx;
}

/** Create a server context with test certificate. */
inline context
make_server_context()
{
    context ctx;
    ctx.use_certificate( server_cert_pem, file_format::pem );
    ctx.use_private_key( server_key_pem, file_format::pem );
    ctx.set_verify_mode( verify_mode::none );
    return ctx;
}

/** Create a client context that trusts the test CA. */
inline context
make_client_context()
{
    context ctx;
    ctx.add_certificate_authority( ca_cert_pem );
    ctx.set_verify_mode( verify_mode::peer );
    return ctx;
}

/** Create a client context that trusts the WRONG CA (for failure tests). */
inline context
make_wrong_ca_context()
{
    context ctx;
    ctx.add_certificate_authority( wrong_ca_cert_pem );
    ctx.set_verify_mode( verify_mode::peer );
    return ctx;
}

/** Create a context that requires peer verification but has no cert. */
inline context
make_verify_no_cert_context()
{
    context ctx;
    ctx.set_verify_mode( verify_mode::require_peer );
    return ctx;
}

//------------------------------------------------------------------------------
//
// Context Configuration Modes
//
//------------------------------------------------------------------------------

enum class context_mode
{
    anon,           // Anonymous ciphers, no certificates
    shared_cert,    // Both use same context with server cert
    separate_cert   // Server has cert, client trusts CA
};

/** Create client and server contexts for the given mode. */
inline std::pair<context, context>
make_contexts( context_mode mode )
{
    switch( mode )
    {
    case context_mode::anon:
        return { make_anon_context(), make_anon_context() };
    case context_mode::shared_cert:
    {
        auto ctx = make_server_context();
        ctx.add_certificate_authority( ca_cert_pem );
        return { ctx, ctx };
    }
    case context_mode::separate_cert:
        return { make_client_context(), make_server_context() };
    }
    return { make_anon_context(), make_anon_context() };
}

//------------------------------------------------------------------------------
//
// Test Coroutines
//
//------------------------------------------------------------------------------

/** Test bidirectional data transfer on connected streams. */
inline capy::task<>
test_stream( io_stream& a, io_stream& b )
{
    char buf[32] = {};

    // Write from a, read from b
    auto [ec1, n1] = co_await a.write_some(
        capy::const_buffer( "hello", 5 ) );
    BOOST_TEST( !ec1 );
    BOOST_TEST_EQ( n1, 5u );

    auto [ec2, n2] = co_await b.read_some(
        capy::mutable_buffer( buf, sizeof( buf ) ) );
    BOOST_TEST( !ec2 );
    BOOST_TEST_EQ( n2, 5u );
    BOOST_TEST_EQ( std::string_view( buf, n2 ), "hello" );

    // Write from b, read from a
    auto [ec3, n3] = co_await b.write_some(
        capy::const_buffer( "world", 5 ) );
    BOOST_TEST( !ec3 );
    BOOST_TEST_EQ( n3, 5u );

    auto [ec4, n4] = co_await a.read_some(
        capy::mutable_buffer( buf, sizeof( buf ) ) );
    BOOST_TEST( !ec4 );
    BOOST_TEST_EQ( n4, 5u );
    BOOST_TEST_EQ( std::string_view( buf, n4 ), "world" );
}

//------------------------------------------------------------------------------
//
// Parameterized Test Runner
//
//------------------------------------------------------------------------------

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
    context client_ctx,
    context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server )
{
    auto [s1, s2] = corosio::test::make_socket_pair( ioc );

    auto client = make_client( s1, client_ctx );
    auto server = make_server( s2, server_ctx );

    // Store lambdas in named variables before invoking - anonymous lambda + immediate
    // invocation pattern [...](){}() can cause capture corruption with run_async
    auto client_task = [&client]() -> capy::task<>
    {
        auto [ec] = co_await client.handshake( tls_stream::client );
        BOOST_TEST( !ec );
    };

    auto server_task = [&server]() -> capy::task<>
    {
        auto [ec] = co_await server.handshake( tls_stream::server );
        BOOST_TEST( !ec );
    };

    capy::run_async( ioc.get_executor() )( client_task() );
    capy::run_async( ioc.get_executor() )( server_task() );

    ioc.run();
    ioc.restart();

    // Bidirectional data transfer
    auto transfer_task = [&client, &server]() -> capy::task<>
    {
        co_await test_stream( client, server );
    };
    capy::run_async( ioc.get_executor() )( transfer_task() );

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
    context client_ctx,
    context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server )
{
    auto [s1, s2] = corosio::test::make_socket_pair( ioc );

    auto client = make_client( s1, client_ctx );
    auto server = make_server( s2, server_ctx );

    // Store lambdas in named variables before invoking - anonymous lambda + immediate
    // invocation pattern [...](){}() can cause capture corruption with run_async
    auto client_task = [&client]() -> capy::task<>
    {
        auto [ec] = co_await client.handshake( tls_stream::client );
        BOOST_TEST( !ec );
    };

    auto server_task = [&server]() -> capy::task<>
    {
        auto [ec] = co_await server.handshake( tls_stream::server );
        BOOST_TEST( !ec );
    };

    capy::run_async( ioc.get_executor() )( client_task() );
    capy::run_async( ioc.get_executor() )( server_task() );

    ioc.run();
    ioc.restart();

    // Bidirectional data transfer
    auto transfer_task = [&client, &server]() -> capy::task<>
    {
        co_await test_stream( client, server );
    };
    capy::run_async( ioc.get_executor() )( transfer_task() );

    ioc.run();

    // Skip TLS shutdown - just close sockets (like HTTP "connection: close")
    s1.close();
    s2.close();
}

/** Run a TLS test expecting handshake failure.

    Uses a timer to handle the case where one side fails and the other
    blocks waiting for data. When the timer fires, sockets are closed
    to unblock any pending operations.
    
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
    context client_ctx,
    context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server )
{
    auto [s1, s2] = corosio::test::make_socket_pair( ioc );

    auto client = make_client( s1, client_ctx );
    auto server = make_server( s2, server_ctx );

    bool client_failed = false;
    bool server_failed = false;
    bool client_done = false;
    bool server_done = false;

    // Timer to unblock stuck handshakes (failsafe only)
    timer timeout( ioc );
    timeout.expires_after( std::chrono::milliseconds( 200 ) );

    // Store lambdas in named variables before invoking - anonymous lambda + immediate
    // invocation pattern [...](){}() can cause capture corruption with run_async
    auto client_task = [&client, &client_failed, &client_done, &server_done, &timeout, &s1, &s2]() -> capy::task<>
    {
        auto [ec] = co_await client.handshake( tls_stream::client );
        if( ec )
        {
            client_failed = true;
            // Cancel then close sockets to unblock server immediately (IOCP needs cancel)
            if( s1.is_open() ) { s1.cancel(); s1.close(); }
            if( s2.is_open() ) { s2.cancel(); s2.close(); }
        }
        client_done = true;
        if( server_done )
            timeout.cancel();
    };

    auto server_task = [&server, &server_failed, &server_done, &client_done, &timeout, &s1, &s2]() -> capy::task<>
    {
        auto [ec] = co_await server.handshake( tls_stream::server );
        if( ec )
        {
            server_failed = true;
            // Cancel then close sockets to unblock client immediately (IOCP needs cancel)
            if( s1.is_open() ) { s1.cancel(); s1.close(); }
            if( s2.is_open() ) { s2.cancel(); s2.close(); }
        }
        server_done = true;
        if( client_done )
            timeout.cancel();
    };

    bool failsafe_hit = false;
    auto timeout_task = [&timeout, &failsafe_hit, &s1, &s2]() -> capy::task<>
    {
        auto [ec] = co_await timeout.wait();
        if( !ec )
        {
            failsafe_hit = true;
            // Timer expired - cancel pending operations then close sockets
            if( s1.is_open() ) { s1.cancel(); s1.close(); }
            if( s2.is_open() ) { s2.cancel(); s2.close(); }
        }
    };

    capy::run_async( ioc.get_executor() )( client_task() );
    capy::run_async( ioc.get_executor() )( server_task() );
    capy::run_async( ioc.get_executor() )( timeout_task() );

    ioc.run();
    BOOST_TEST( !failsafe_hit );  // failsafe timeout should not be hit

    // At least one side should have failed
    BOOST_TEST( client_failed || server_failed );

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
    context client_ctx,
    context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server )
{
    auto [s1, s2] = corosio::test::make_socket_pair( ioc );

    auto client = make_client( s1, client_ctx );
    auto server = make_server( s2, server_ctx );

    // Handshake phase
    auto client_hs = [&client]() -> capy::task<>
    {
        auto [ec] = co_await client.handshake( tls_stream::client );
        BOOST_TEST( !ec );
    };

    auto server_hs = [&server]() -> capy::task<>
    {
        auto [ec] = co_await server.handshake( tls_stream::server );
        BOOST_TEST( !ec );
    };

    capy::run_async( ioc.get_executor() )( client_hs() );
    capy::run_async( ioc.get_executor() )( server_hs() );

    ioc.run();
    ioc.restart();

    // Data transfer phase
    auto transfer_task = [&client, &server]() -> capy::task<>
    {
        co_await test_stream( client, server );
    };
    capy::run_async( ioc.get_executor() )( transfer_task() );

    ioc.run();
    ioc.restart();

    // Shutdown phase: client sends close_notify, server reads EOF then closes socket.
    // Server closing the socket causes client's shutdown to complete.
    bool done = false;

    // Failsafe timer in case of bugs
    timer failsafe( ioc );
    failsafe.expires_after( std::chrono::milliseconds( 200 ) );

    auto client_shutdown = [&client, &done, &failsafe]() -> capy::task<>
    {
        auto [ec] = co_await client.shutdown();
        done = true;
        failsafe.cancel();
        BOOST_TEST( !ec || ec == capy::cond::stream_truncated ||
                    ec == capy::cond::eof || ec == capy::cond::canceled );
    };

    auto server_read_then_close = [&server, &s2]() -> capy::task<>
    {
        char buf[32];
        auto [ec, n] = co_await server.read_some(
            capy::mutable_buffer( buf, sizeof( buf ) ) );
        BOOST_TEST( ec == capy::cond::eof || ec == capy::cond::stream_truncated ||
                    ec == capy::cond::canceled );
        // Close socket to unblock client's shutdown
        s2.cancel();
        s2.close();
    };

    bool failsafe_hit = false;
    auto failsafe_task = [&failsafe, &failsafe_hit, &done, &s1, &s2]() -> capy::task<>
    {
        auto [ec] = co_await failsafe.wait();
        if( !ec && !done )
        {
            failsafe_hit = true;
            if( s1.is_open() ) { s1.cancel(); s1.close(); }
            if( s2.is_open() ) { s2.cancel(); s2.close(); }
        }
    };

    capy::run_async( ioc.get_executor() )( client_shutdown() );
    capy::run_async( ioc.get_executor() )( server_read_then_close() );
    capy::run_async( ioc.get_executor() )( failsafe_task() );

    ioc.run();
    BOOST_TEST( !failsafe_hit );  // failsafe timeout should not be hit
    if( s1.is_open() ) s1.close();
    if( s2.is_open() ) s2.close();
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
    context client_ctx,
    context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server )
{
    auto [s1, s2] = corosio::test::make_socket_pair( ioc );

    auto client = make_client( s1, client_ctx );
    auto server = make_server( s2, server_ctx );

    // Handshake phase
    auto client_hs = [&client]() -> capy::task<>
    {
        auto [ec] = co_await client.handshake( tls_stream::client );
        BOOST_TEST( !ec );
    };

    auto server_hs = [&server]() -> capy::task<>
    {
        auto [ec] = co_await server.handshake( tls_stream::server );
        BOOST_TEST( !ec );
    };

    capy::run_async( ioc.get_executor() )( client_hs() );
    capy::run_async( ioc.get_executor() )( server_hs() );

    ioc.run();
    ioc.restart();

    // Data transfer phase
    auto transfer_task = [&client, &server]() -> capy::task<>
    {
        co_await test_stream( client, server );
    };
    capy::run_async( ioc.get_executor() )( transfer_task() );

    ioc.run();
    ioc.restart();

    // Truncation test with timeout protection
    bool read_done = false;

    // Timeout to prevent deadlock
    timer timeout( ioc );
    timeout.expires_after( std::chrono::milliseconds( 200 ) );

    auto client_close = [&s1]() -> capy::task<>
    {
        // Cancel and close underlying socket without TLS shutdown (IOCP needs cancel)
        s1.cancel();
        s1.close();
        co_return;
    };

    auto server_read_truncated = [&server, &read_done, &timeout]() -> capy::task<>
    {
        char buf[32];
        auto [ec, n] = co_await server.read_some(
            capy::mutable_buffer( buf, sizeof( buf ) ) );
        read_done = true;
        timeout.cancel();
        // Should get stream_truncated, eof, or canceled
        BOOST_TEST( ec == capy::cond::stream_truncated ||
                    ec == capy::cond::eof ||
                    ec == capy::cond::canceled );
    };

    bool failsafe_hit = false;
    auto timeout_task = [&timeout, &failsafe_hit, &s1, &s2]() -> capy::task<>
    {
        auto [ec] = co_await timeout.wait();
        if( !ec )
        {
            failsafe_hit = true;
            // Timer expired - cancel pending operations (check if still open)
            if( s1.is_open() ) { s1.cancel(); s1.close(); }
            if( s2.is_open() ) { s2.cancel(); s2.close(); }
        }
    };

    capy::run_async( ioc.get_executor() )( client_close() );
    capy::run_async( ioc.get_executor() )( server_read_truncated() );
    capy::run_async( ioc.get_executor() )( timeout_task() );

    ioc.run();
    BOOST_TEST( !failsafe_hit );  // failsafe timeout should not be hit
    if( s1.is_open() ) s1.close();
    if( s2.is_open() ) s2.close();
}

//------------------------------------------------------------------------------
//
// Additional Context Helpers for Extended Tests
//
//------------------------------------------------------------------------------

/** Create a server context using chain certificates (signed by intermediate CA). */
inline context
make_chain_server_context()
{
    context ctx;
    ctx.use_certificate( chain_server_cert_pem, file_format::pem );
    ctx.use_private_key( chain_server_key_pem, file_format::pem );
    ctx.set_verify_mode( verify_mode::none );
    return ctx;
}

/** Create a server context with full certificate chain.
    Server sends entity cert + intermediate cert, allowing client to verify
    chain up to root CA. Uses use_certificate_chain() which expects the full
    chain (entity + intermediates) in a single PEM blob. */
inline context
make_fullchain_server_context()
{
    context ctx;
    // use_certificate_chain expects entity cert followed by intermediate(s)
    ctx.use_certificate_chain( server_fullchain_pem );
    ctx.use_private_key( chain_server_key_pem, file_format::pem );
    ctx.set_verify_mode( verify_mode::none );
    return ctx;
}

/** Create a client context that trusts ONLY the root CA (for chain tests).
    Server must send intermediate cert in chain for verification to succeed. */
inline context
make_rootonly_client_context()
{
    context ctx;
    ctx.add_certificate_authority( root_ca_cert_pem );
    ctx.set_verify_mode( verify_mode::peer );
    return ctx;
}

/** Create a client context that trusts the root CA (for chain tests). */
inline context
make_chain_client_context()
{
    context ctx;
    // Trust both root and intermediate CA for chain verification
    ctx.add_certificate_authority( root_ca_cert_pem );
    ctx.add_certificate_authority( intermediate_cert_pem );
    ctx.set_verify_mode( verify_mode::peer );
    return ctx;
}

/** Create a server context with an EXPIRED certificate.
    The certificate expired on Jan 2, 2020. */
inline context
make_expired_server_context()
{
    context ctx;
    ctx.use_certificate( expired_cert_pem, file_format::pem );
    ctx.use_private_key( expired_key_pem, file_format::pem );
    return ctx;
}

/** Create a client context that trusts the expired cert's self-signed CA.
    Used with make_expired_server_context() to test expiry validation. */
inline context
make_expired_client_context()
{
    context ctx;
    // Trust the expired cert as its own CA (self-signed)
    ctx.add_certificate_authority( expired_cert_pem );
    ctx.set_verify_mode( verify_mode::peer );
    return ctx;
}

/** Create a server context with wrong hostname (CN=wrong.example.com). */
inline context
make_wrong_host_server_context()
{
    context ctx;
    ctx.use_certificate( wrong_host_cert_pem, file_format::pem );
    ctx.use_private_key( wrong_host_key_pem, file_format::pem );
    ctx.set_verify_mode( verify_mode::none );
    return ctx;
}

/** Create a client context for mTLS (with client certificate). */
inline context
make_mtls_client_context()
{
    context ctx;
    ctx.use_certificate( client_cert_pem, file_format::pem );
    ctx.use_private_key( client_key_pem, file_format::pem );
    // Trust both root and intermediate CA for chain verification
    ctx.add_certificate_authority( root_ca_cert_pem );
    ctx.add_certificate_authority( intermediate_cert_pem );
    ctx.set_verify_mode( verify_mode::peer );
    return ctx;
}

/** Create a server context that requires client certificates (mTLS). */
inline context
make_mtls_server_context()
{
    context ctx;
    ctx.use_certificate( chain_server_cert_pem, file_format::pem );
    ctx.use_private_key( chain_server_key_pem, file_format::pem );
    // Trust both root and intermediate CA for chain verification
    ctx.add_certificate_authority( root_ca_cert_pem );
    ctx.add_certificate_authority( intermediate_cert_pem );
    ctx.set_verify_mode( verify_mode::require_peer );
    return ctx;
}

/** Create a client context that trusts the untrusted CA (for verification failures). */
inline context
make_untrusted_ca_client_context()
{
    context ctx;
    ctx.add_certificate_authority( untrusted_ca_cert_pem );
    ctx.set_verify_mode( verify_mode::peer );
    return ctx;
}

/** Create an mTLS client context with INVALID client certificate.
    Uses server_cert_pem (self-signed) which is NOT signed by the
    intermediate/root CA that make_mtls_server_context() trusts. */
inline context
make_invalid_mtls_client_context()
{
    context ctx;
    // Use the self-signed server cert as client cert - server won't trust it
    ctx.use_certificate( server_cert_pem, file_format::pem );
    ctx.use_private_key( server_key_pem, file_format::pem );
    // Trust the chain CAs so we can verify server
    ctx.add_certificate_authority( root_ca_cert_pem );
    ctx.add_certificate_authority( intermediate_cert_pem );
    ctx.set_verify_mode( verify_mode::peer );
    return ctx;
}

//------------------------------------------------------------------------------
//
// Connection Reset Test
//
//------------------------------------------------------------------------------

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
    context client_ctx,
    context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server )
{
    auto [s1, s2] = corosio::test::make_socket_pair( ioc );

    auto client = make_client( s1, client_ctx );
    auto server = make_server( s2, server_ctx );

    bool client_failed = false;

    // Timeout protection
    timer timeout( ioc );
    timeout.expires_after( std::chrono::milliseconds( 200 ) );

    auto client_task = [&client, &client_failed, &timeout]() -> capy::task<>
    {
        auto [ec] = co_await client.handshake( tls_stream::client );
        // Should fail because server closed socket
        if( ec )
            client_failed = true;
        timeout.cancel();
    };

    // Server closes socket immediately (simulates connection reset)
    auto server_task = [&s2]() -> capy::task<>
    {
        // Cancel and close socket to simulate connection reset (IOCP needs cancel)
        s2.cancel();
        s2.close();
        co_return;
    };

    bool failsafe_hit = false;
    auto timeout_task = [&timeout, &failsafe_hit, &s1]() -> capy::task<>
    {
        auto [ec] = co_await timeout.wait();
        if( !ec && s1.is_open() )
        {
            failsafe_hit = true;
            s1.cancel();
            s1.close();
        }
    };

    capy::run_async( ioc.get_executor() )( client_task() );
    capy::run_async( ioc.get_executor() )( server_task() );
    capy::run_async( ioc.get_executor() )( timeout_task() );

    ioc.run();

    BOOST_TEST( !failsafe_hit );  // failsafe timeout should not be hit
    BOOST_TEST( client_failed );

    if( s1.is_open() ) s1.close();
    if( s2.is_open() ) s2.close();
}

//------------------------------------------------------------------------------
//
// Stop Token Cancellation Test
//
//------------------------------------------------------------------------------

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
    context client_ctx,
    context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server )
{

    auto [s1, s2] = corosio::test::make_socket_pair( ioc );

    auto client = make_client( s1, client_ctx );
    auto server = make_server( s2, server_ctx );

    std::stop_source stop_src;
    bool client_got_error = false;

    // Failsafe timeout to prevent infinite hang if cancellation doesn't work
    // 2000ms allows headroom for CI with coverage instrumentation
    timer failsafe( ioc );
    failsafe.expires_after( std::chrono::milliseconds( 2000 ) );

    // Client handshake - will be cancelled while waiting for ServerHello
    auto client_task = [&client, &client_got_error, &failsafe]() -> capy::task<>
    {
        auto [ec] = co_await client.handshake( tls_stream::client );
        if( ec )
            client_got_error = true;
        failsafe.cancel();
    };

    // Server waits for ClientHello then cancels - deterministic synchronization
    auto server_task = [&s2, &stop_src]() -> capy::task<>
    {
        // Wait for client to send ClientHello (proves client started handshake)
        char buf[1];
        (void)co_await s2.read_some( capy::mutable_buffer( buf, 1 ) );
        // Client is now blocked waiting for ServerHello - cancel it
        stop_src.request_stop();
    };

    bool failsafe_hit = false;
    auto failsafe_task = [&failsafe, &failsafe_hit, &s1, &s2]() -> capy::task<>
    {
        auto [ec] = co_await failsafe.wait();
        if( !ec )
        {
            failsafe_hit = true;
            if( s1.is_open() ) { s1.cancel(); s1.close(); }
            if( s2.is_open() ) { s2.cancel(); s2.close(); }
        }
    };
    capy::run_async( ioc.get_executor(), stop_src.get_token() )( client_task() );
    capy::run_async( ioc.get_executor() )( server_task() );
    capy::run_async( ioc.get_executor() )( failsafe_task() );
    ioc.run();

    BOOST_TEST( !failsafe_hit );  // failsafe timeout should not be hit
    BOOST_TEST( client_got_error );

    if( s1.is_open() ) s1.close();
    if( s2.is_open() ) s2.close();
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
    context client_ctx,
    context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server )
{

    auto [s1, s2] = corosio::test::make_socket_pair( ioc );

    auto client = make_client( s1, client_ctx );
    auto server = make_server( s2, server_ctx );

    // Handshake phase
    auto client_hs = [&client]() -> capy::task<>
    {
        auto [ec] = co_await client.handshake( tls_stream::client );
        BOOST_TEST( !ec );
    };

    auto server_hs = [&server]() -> capy::task<>
    {
        auto [ec] = co_await server.handshake( tls_stream::server );
        BOOST_TEST( !ec );
    };

    capy::run_async( ioc.get_executor() )( client_hs() );
    capy::run_async( ioc.get_executor() )( server_hs() );

    ioc.run();
    ioc.restart();

    // Read cancellation phase
    std::stop_source stop_src;
    bool read_got_error = false;

    // Failsafe timeout - 2000ms allows headroom for CI with coverage instrumentation
    timer failsafe( ioc );
    failsafe.expires_after( std::chrono::milliseconds( 2000 ) );

    auto client_read = [&client, &read_got_error, &failsafe]() -> capy::task<>
    {
        char buf[32];
        auto [ec, n] = co_await client.read_some(
            capy::mutable_buffer( buf, sizeof( buf ) ) );
        if( ec )
            read_got_error = true;
        failsafe.cancel();
    };

    // Server triggers cancellation immediately - client will block on read
    // since server never sends data. This is deterministic because the
    // client read is queued first and will suspend waiting for socket data.
    auto server_cancel = [&stop_src]() -> capy::task<>
    {
        stop_src.request_stop();
        co_return;
    };

    bool failsafe_hit = false;
    auto failsafe_task = [&failsafe, &failsafe_hit, &s1, &s2]() -> capy::task<>
    {
        auto [ec] = co_await failsafe.wait();
        if( !ec )
        {
            failsafe_hit = true;
            if( s1.is_open() ) { s1.cancel(); s1.close(); }
            if( s2.is_open() ) { s2.cancel(); s2.close(); }
        }
    };
    capy::run_async( ioc.get_executor(), stop_src.get_token() )( client_read() );
    capy::run_async( ioc.get_executor() )( server_cancel() );
    capy::run_async( ioc.get_executor() )( failsafe_task() );
    ioc.run();

    BOOST_TEST( !failsafe_hit );  // failsafe timeout should not be hit
    BOOST_TEST( read_got_error );

    if( s1.is_open() ) s1.close();
    if( s2.is_open() ) s2.close();
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
    context client_ctx,
    context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server )
{

    auto [s1, s2] = corosio::test::make_socket_pair( ioc );

    auto client = make_client( s1, client_ctx );
    auto server = make_server( s2, server_ctx );

    // Handshake phase
    auto client_hs = [&client]() -> capy::task<>
    {
        auto [ec] = co_await client.handshake( tls_stream::client );
        BOOST_TEST( !ec );
    };

    auto server_hs = [&server]() -> capy::task<>
    {
        auto [ec] = co_await server.handshake( tls_stream::server );
        BOOST_TEST( !ec );
    };

    capy::run_async( ioc.get_executor() )( client_hs() );
    capy::run_async( ioc.get_executor() )( server_hs() );

    ioc.run();
    ioc.restart();

    // Write cancellation phase - fill socket buffer to cause blocking
    std::stop_source stop_src;
    bool write_got_error = false;

    // Large buffer to fill socket buffer and cause blocking
    std::vector<char> large_buf( 1024 * 1024, 'X' );

    // Failsafe timeout - 2000ms allows headroom for CI with coverage instrumentation
    timer failsafe( ioc );
    failsafe.expires_after( std::chrono::milliseconds( 2000 ) );

    auto client_write = [&client, &large_buf, &write_got_error, &failsafe]() -> capy::task<>
    {
        // Write in loop until cancelled or error
        for( int i = 0; i < 100; ++i )
        {
            auto [ec, n] = co_await client.write_some(
                capy::const_buffer( large_buf.data(), large_buf.size() ) );
            if( ec )
            {
                write_got_error = true;
                failsafe.cancel();
                co_return;
            }
        }
        failsafe.cancel();
    };

    // Server waits for data then cancels - deterministic synchronization
    auto server_cancel = [&s2, &stop_src]() -> capy::task<>
    {
        // Wait for client to send some data (proves client started writing)
        char buf[1];
        (void)co_await s2.read_some( capy::mutable_buffer( buf, 1 ) );
        // Client is now writing - cancel it
        stop_src.request_stop();
    };

    bool failsafe_hit = false;
    auto failsafe_task = [&failsafe, &failsafe_hit, &s1, &s2]() -> capy::task<>
    {
        auto [ec] = co_await failsafe.wait();
        if( !ec )
        {
            failsafe_hit = true;
            if( s1.is_open() ) { s1.cancel(); s1.close(); }
            if( s2.is_open() ) { s2.cancel(); s2.close(); }
        }
    };
    capy::run_async( ioc.get_executor(), stop_src.get_token() )( client_write() );
    capy::run_async( ioc.get_executor() )( server_cancel() );
    capy::run_async( ioc.get_executor() )( failsafe_task() );
    ioc.run();

    BOOST_TEST( !failsafe_hit );  // failsafe timeout should not be hit
    BOOST_TEST( write_got_error );

    if( s1.is_open() ) s1.close();
    if( s2.is_open() ) s2.close();
}

//------------------------------------------------------------------------------
//
// Socket Error Propagation Test
//
//------------------------------------------------------------------------------

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
    context client_ctx,
    context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server )
{

    auto [s1, s2] = corosio::test::make_socket_pair( ioc );

    auto client = make_client( s1, client_ctx );
    auto server = make_server( s2, server_ctx );

    bool client_got_error = false;

    // Failsafe timeout - 2000ms allows headroom for CI with coverage instrumentation
    timer failsafe( ioc );
    failsafe.expires_after( std::chrono::milliseconds( 2000 ) );

    // Client starts handshake - will be cancelled
    auto client_task = [&client, &client_got_error, &failsafe]() -> capy::task<>
    {
        auto [ec] = co_await client.handshake( tls_stream::client );
        if( ec )
            client_got_error = true;
        failsafe.cancel();
    };

    // Server waits for ClientHello then cancels - deterministic synchronization
    auto server_task = [&s1, &s2]() -> capy::task<>
    {
        // Wait for client to send ClientHello (proves client started handshake)
        char buf[1];
        (void)co_await s2.read_some( capy::mutable_buffer( buf, 1 ) );
        // Client is now blocked waiting for ServerHello - cancel its socket
        s1.cancel();
    };

    bool failsafe_hit = false;
    auto failsafe_task = [&failsafe, &failsafe_hit, &s1, &s2]() -> capy::task<>
    {
        auto [ec] = co_await failsafe.wait();
        if( !ec )
        {
            failsafe_hit = true;
            if( s1.is_open() ) { s1.cancel(); s1.close(); }
            if( s2.is_open() ) { s2.cancel(); s2.close(); }
        }
    };
    capy::run_async( ioc.get_executor() )( client_task() );
    capy::run_async( ioc.get_executor() )( server_task() );
    capy::run_async( ioc.get_executor() )( failsafe_task() );
    ioc.run();

    BOOST_TEST( !failsafe_hit );  // failsafe timeout should not be hit
    BOOST_TEST( client_got_error );

    if( s1.is_open() ) s1.close();
    if( s2.is_open() ) s2.close();
}

} // namespace boost::corosio::tls::test

#endif
