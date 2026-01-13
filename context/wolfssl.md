# wolfSSL Documentation

---

## Table of Contents

- [1. Introduction](#1-introduction)
  - [1.1 Why Choose wolfSSL?](#11-why-choose-wolfssl)
- [2. Building wolfSSL](#2-building-wolfssl)
  - [2.1 Getting wolfSSL Source Code](#21-getting-wolfssl-source-code)
  - [2.2 Building on Unix-like Systems](#22-building-on-unix-like-systems)
  - [2.3 Building on Windows](#23-building-on-windows)
  - [2.4 Building in a non-standard environment](#24-building-in-a-non-standard-environment)
  - [2.5 Features Defined as C Pre-processor Macro](#25-features-defined-as-c-pre-processor-macro)
  - [2.6 Build Options](#26-build-options)
  - [2.7 Special Math Optimization Flags](#27-special-math-optimization-flags)
  - [2.8 Cross Compiling](#28-cross-compiling)
  - [2.9 Building Ports](#29-building-ports)
  - [2.10 Building For NXP CAAM](#210-building-for-nxp-caam)
- [3. Getting Started](#3-getting-started)
  - [3.1 General Description](#31-general-description)
  - [3.2 Testsuite](#32-testsuite)
  - [3.3 Client Example](#33-client-example)
  - [3.4 Server Example](#34-server-example)
  - [3.5 EchoServer Example](#35-echoserver-example)
  - [3.6 EchoClient Example](#36-echoclient-example)
  - [3.7 Benchmark](#37-benchmark)
  - [3.8 Changing a Client Application to Use wolfSSL](#38-changing-a-client-application-to-use-wolfssl)
  - [3.9 Changing a Server Application to Use wolfSSL](#39-changing-a-server-application-to-use-wolfssl)
- [4. Features](#4-features)
  - [4.1 Features Overview](#41-features-overview)
  - [4.2 Protocol Support](#42-protocol-support)
  - [4.3 Cipher Support](#43-cipher-support)
  - [4.4 Hardware Accelerated Crypto](#44-hardware-accelerated-crypto)
  - [4.5 SSL Inspection (Sniffer)](#45-ssl-inspection-sniffer)
  - [4.6 Static Buffer Allocation Option](#46-static-buffer-allocation-option)
  - [4.7 Compression](#47-compression)
  - [4.8 Pre-Shared Keys](#48-pre-shared-keys)
  - [4.9 Client Authentication](#49-client-authentication)
  - [4.10 Server Name Indication](#410-server-name-indication)
  - [4.11 Handshake Modifications](#411-handshake-modifications)
  - [4.12 Truncated HMAC](#412-truncated-hmac)
  - [4.13 Timing-Resistance in wolfSSL](#413-timing-resistance-in-wolfssl)
  - [4.14 Fixed ABI](#414-fixed-abi)
- [5. Portability](#5-portability)
  - [5.1 Abstraction Layers](#51-abstraction-layers)
  - [5.2 Supported Operating Systems](#52-supported-operating-systems)
  - [5.3 Supported Chipmakers](#53-supported-chipmakers)
  - [5.4 C# Wrapper](#54-c-wrapper)
- [6. Callbacks](#6-callbacks)
  - [6.1 HandShake Callback](#61-handshake-callback)
  - [6.2 Timeout Callback](#62-timeout-callback)
  - [6.3 User Atomic Record Layer Processing](#63-user-atomic-record-layer-processing)
  - [6.4 Public Key Callbacks](#64-public-key-callbacks)
  - [6.5 Crypto Callbacks (cryptocb)](#65-crypto-callbacks-cryptocb)
- [7. Keys and Certificates](#7-keys-and-certificates)
  - [7.1 Supported Formats and Sizes](#71-supported-formats-and-sizes)
  - [7.2 Supported Certificate Extensions](#72-supported-certificate-extensions)
  - [7.3 Certificate Loading](#73-certificate-loading)
  - [7.4 Certificate Chain Verification](#74-certificate-chain-verification)
  - [7.5 Domain Name Check for Server Certificates](#75-domain-name-check-for-server-certificates)
  - [7.6 No File System and using Certificates](#76-no-file-system-and-using-certificates)
  - [7.7 Serial Number Retrieval](#77-serial-number-retrieval)
  - [7.8 RSA Key Generation](#78-rsa-key-generation)
  - [7.9 Certificate Generation](#79-certificate-generation)
  - [7.10 Certificate Signing Request (CSR) Generation](#710-certificate-signing-request-csr-generation)
  - [7.11 Convert to raw ECC key](#711-convert-to-raw-ecc-key)
- [8. Debugging](#8-debugging)
  - [8.1 Debugging and Logging](#81-debugging-and-logging)
  - [8.2 Error Codes](#82-error-codes)
- [9. Library Design](#9-library-design)
  - [9.1 Library Headers](#91-library-headers)
  - [9.2 Startup and Exit](#92-startup-and-exit)
  - [9.3 Structure Usage](#93-structure-usage)
  - [9.4 Thread Safety](#94-thread-safety)
  - [9.5 Input and Output Buffers](#95-input-and-output-buffers)
- [10. wolfCrypt Usage Reference](#10-wolfcrypt-usage-reference)
  - [10.1 Hash Functions](#101-hash-functions)
  - [10.2 Keyed Hash Functions](#102-keyed-hash-functions)
  - [10.3 Block Ciphers](#103-block-ciphers)
  - [10.4 Stream Ciphers](#104-stream-ciphers)
  - [10.5 Public Key Cryptography](#105-public-key-cryptography)
- [11. SSL Tutorial](#11-ssl-tutorial)
  - [11.1 Introduction](#111-introduction)
  - [11.2 Quick Summary of SSL/TLS](#112-quick-summary-of-ssltls)
  - [11.3 Getting the Source Code](#113-getting-the-source-code)
  - [11.4 Base Example Modifications](#114-base-example-modifications)
  - [11.5 Building and Installing wolfSSL](#115-building-and-installing-wolfssl)
  - [11.6 Initial Compilation](#116-initial-compilation)
  - [11.7 Libraries](#117-libraries)
  - [11.8 Headers](#118-headers)
  - [11.9 Startup/Shutdown](#119-startupshutdown)
  - [11.10 WOLFSSL_CTX Factory](#1110-wolfssl_ctx-factory)
  - [11.11 WOLFSSL Object](#1111-wolfssl-object)
  - [11.12 Sending/Receiving Data](#1112-sendingreceiving-data)
  - [11.13 Signal Handling](#1113-signal-handling)
  - [11.14 Certificates](#1114-certificates)
  - [11.15 Conclusion](#1115-conclusion)
- [12. Best Practices for Embedded Devices](#12-best-practices-for-embedded-devices)
  - [12.1 Creating Private Keys](#121-creating-private-keys)
  - [12.2 Digitally Signing and Authenticating with wolfSSL](#122-digitally-signing-and-authenticating-with-wolfssl)
- [13. OpenSSL Compatibility](#13-openssl-compatibility)
  - [13.1 Compatibility with OpenSSL](#131-compatibility-with-openssl)
  - [13.2 Differences Between wolfSSL and OpenSSL](#132-differences-between-wolfssl-and-openssl)
  - [13.3 Supported OpenSSL Structures](#133-supported-openssl-structures)
  - [13.4 Supported OpenSSL Functions](#134-supported-openssl-functions)
  - [13.5 x509 Certificates](#135-x509-certificates)
- [14. Licensing](#14-licensing)
  - [14.1 Open Source](#141-open-source)
  - [14.2 Commercial Licensing](#142-commercial-licensing)
  - [14.3 FIPS 140-2/3 Validation](#143-fips-140-23-validation)
  - [14.4 Support Packages](#144-support-packages)
- [15. Support and Consulting](#15-support-and-consulting)
  - [15.1 How to Get Support](#151-how-to-get-support)
  - [15.2 Consulting](#152-consulting)
- [16. wolfSSL Updates](#16-wolfssl-updates)
  - [16.1 Product Release Information](#161-product-release-information)
- [Appendix A: wolfSSL API Reference](#appendix-a-wolfssl-api-reference)
- [Appendix B: wolfCrypt API Reference](#appendix-b-wolfcrypt-api-reference)
- [Appendix C: API Header Files](#appendix-c-api-header-files)
- [Appendix D: Error Codes](#appendix-d-error-codes)
- [Appendix E: Library Comparison](#appendix-e-library-comparison)
- [Appendix F: wolfSSL Release Comparison](#appendix-f-wolfssl-release-comparison)
- [Appendix G: wolfSSL Porting Guide](#appendix-g-wolfssl-porting-guide)
- [Appendix H: wolfSM (ShangMi)](#appendix-h-wolfsm-shangmi)

---

# 1. Introduction

This manual is written as a technical guide to the wolfSSL embedded SSL/TLS library. It will explain how to build and get started with wolfSSL, provide an overview of build options, features, portability enhancements, support, and much more.

## 1.1 Why Choose wolfSSL?

There are many reasons to choose wolfSSL as your embedded SSL solution. Some of the top reasons include size (typical footprint sizes range from 20-100 kB), support for the newest standards (SSL 3.0, TLS 1.0, TLS 1.1, TLS 1.2, TLS 1.3, DTLS 1.0, DTLS 1.2, and DTLS 1.3), current and progressive cipher support (including stream ciphers), multi-platform, royalty free, and an OpenSSL compatibility API to ease porting into existing applications which have previously used the OpenSSL package. For a complete feature list, see [Features Overview](#41-features-overview).

---

# 2. Building wolfSSL

wolfSSL was written with portability in mind and should generally be easy to build on most systems. If you have difficulty building wolfSSL, please don't hesitate to seek support through our support forums (https://www.wolfssl.com/forums) or contact us directly at support@wolfssl.com.

This chapter explains how to build wolfSSL on Unix and Windows, and provides guidance for building wolfSSL in a non-standard environment. You will find the "getting started" guide in [Chapter 3](#3-getting-started) and an SSL tutorial in [Chapter 11](#11-ssl-tutorial).

When using the autoconf / automake system to build wolfSSL, wolfSSL uses a single Makefile to build all parts and examples of the library, which is both simpler and faster than using Makefiles recursively.

## 2.1 Getting wolfSSL Source Code

The most recent version of wolfSSL can be downloaded from the wolfSSL website as a ZIP file:

https://www.wolfssl.com/download

After downloading the ZIP file, unzip the file using the `unzip` command. To use native line endings, enable the `-a` modifier when using unzip. From the unzip man page, the `-a` modifier functionality is described:

> The -a option causes files identified by zip as text files (those with the 't' label in zipinfo listings, rather than 'b') to be automatically extracted as such, converting line endings, end-of-file characters and the character set itself as necessary.

**NOTE:** Beginning with the release of wolfSSL 2.0.0rc3, the directory structure of wolfSSL was changed as well as the standard install location. These changes were made to make it easier for open source projects to integrate wolfSSL. For more information on header and structure changes, please see [Library Headers](#91-library-headers) and [Structure Usage](#93-structure-usage).

## 2.2 Building on Unix-like Systems

When building wolfSSL on Linux, *BSD, OS X, Solaris, or other *nix-like systems, use the autoconf system. To build wolfSSL you only need to run two commands from the wolfSSL root directory, `./configure` and `make`.

The `./configure` script sets up the build environment and you can append any number of build options to `./configure`. For a list of available build options, please see [Build Options](#26-build-options) or run the following command:

```bash
./configure --help
```

Once `./configure` has successfully executed, to build wolfSSL, run:

```bash
make
```

To install wolfSSL run:

```bash
make install
```

You may need superuser privileges to install, in which case precede the command with sudo:

```bash
sudo make install
```

To test the build, run the testsuite program from the root wolfSSL directory:

```bash
./testsuite/testsuite.test
```

Alternatively you can use autoconf to run the testsuite as well as the standard wolfSSL API and crypto tests:

```bash
make test
```

If you want to build only the wolfSSL library and not the additional items (examples, testsuite, benchmark app, etc.):

```bash
make src/libwolfssl.la
```

## 2.3 Building on Windows

### 2.3.1 VS 2008

Solutions are included for Visual Studio 2008 in the root directory of the install. For use with Visual Studio 2010 and later, the existing project files should be able to be converted during the import process.

### 2.3.2 VS 2010

You will need to download Service Pack 1 to build wolfSSL solution once it has been updated.

### 2.3.3 VS 2013 (64 bit solution)

You will need to download Service Pack 4 to build wolfSSL solution once it has been updated.

**Note:** After the wolfSSL v3.8.0 release the build preprocessor macros were moved to a centralized file located at `IDE/WIN/user_settings.h`. To add features such as ECC or ChaCha20/Poly1305, add `#defines` here such as `HAVE_ECC` or `HAVE_CHACHA` / `HAVE_POLY1305`.

### 2.3.4 Cygwin

If building wolfSSL for Windows on a Windows development machine, we recommend using the included Visual Studio project files. However, if Cygwin is required, the additional packages needed include:

- unzip
- autoconf
- automake
- gcc-core
- git
- libtool
- make

After installing Cygwin with those packages:

```bash
git clone https://github.com/wolfssl/wolfssl.git
cd wolfssl
./autogen.sh
./configure
make
make check
```

## 2.4 Building in a non-standard environment

While not officially supported, we try to help users wishing to build wolfSSL in a non-standard environment, particularly with embedded and cross-compilation systems. Key notes:

1. The source and header files need to remain in the same directory structure as they are in the wolfSSL download package.
2. wolfSSL header files are located in the `<wolfssl_root>/wolfssl` directory.
3. wolfSSL defaults to a little endian system unless the configure process detects big endian. Define `BIG_ENDIAN_ORDER` if using a big endian system.
4. wolfSSL benefits speed-wise from having a 64-bit type available. Define `SIZEOF_LONG 8` or `SIZEOF_LONG_LONG 8` as appropriate.

### 2.4.1 Building into Yocto Linux

wolfSSL includes recipes for building on Yocto Linux and OpenEmbedded. These recipes are maintained within the meta-wolfSSL layer: https://github.com/wolfSSL/meta-wolfssl

### 2.4.2 Building with Atollic TrueSTUDIO

Versions of wolfSSL following 3.15.5 include a TrueSTUDIO project file for ARM M4-Cortex devices.

### 2.4.3 Building with IAR

The `<wolfssl_root>/IDE/IAR-EWARM` directory contains workspace and project files for IAR Embedded Workbench.

### 2.4.4 Building on OS X and iOS

The `<wolfssl_root>/IDE/XCODE` directory contains Xcode workspace and project files.

### 2.4.5 Building with GCC ARM

In the `<wolfssl_root>/IDE/GCC-ARM` directory, you will find an example wolfSSL project for Cortex M series.

Cross-compile example:

```bash
./configure \
    --host=arm-none-eabi \
    CC=arm-none-eabi-gcc \
    AR=arm-none-eabi-ar \
    STRIP=arm-none-eabi-strip \
    RANLIB=arm-none-eabi-ranlib \
    --prefix=/path/to/build/wolfssl-arm \
    CFLAGS="-march=armv8-a --specs=nosys.specs \
        -DHAVE_PK_CALLBACKS -DWOLFSSL_USER_IO -DWOLFSSL_NO_SOCK -DNO_WRITEV" \
    --disable-filesystem --enable-crypttests \
    --disable-shared
make
make install
```

### 2.4.6 Building on Keil MDK-ARM

Detailed instructions for building wolfSSL on Keil MDK-ARM are available on the wolfSSL website.

## 2.5 Features Defined as C Pre-processor Macro

### 2.5.1 Removing Features

The following defines can be used to remove features from wolfSSL to reduce library footprint size:

| Define | Description |
|--------|-------------|
| `NO_WOLFSSL_CLIENT` | Removes client-specific calls (server-only builds) |
| `NO_WOLFSSL_SERVER` | Removes server-specific calls |
| `NO_DES3` | Removes DES3 encryption |
| `NO_DSA` | Removes DSA support |
| `NO_ERROR_STRINGS` | Disables error strings |
| `NO_HMAC` | Removes HMAC (note: SSL/TLS depends on HMAC) |
| `NO_MD4` | Removes MD4 (broken, shouldn't be used) |
| `NO_MD5` | Removes MD5 |
| `NO_SHA` | Removes SHA-1 |
| `NO_SHA256` | Removes SHA-256 |
| `NO_PSK` | Turns off pre-shared key extension |
| `NO_PWDBASED` | Disables password-based key derivation (PBKDF1, PBKDF2) |
| `NO_RC4` | Removes ARC4 stream cipher |
| `NO_SESSION_CACHE` | Removes session cache (~3 kB savings) |
| `NO_TLS` | Turns off TLS (not recommended) |
| `SMALL_SESSION_CACHE` | Limits session cache from 33 to 6 sessions (~2.5 kB savings) |
| `NO_RSA` | Removes RSA algorithm support |
| `NO_AES` | Disables AES algorithm support |
| `NO_DH` | Disables Diffie-Hellman support |
| `WOLFCRYPT_ONLY` | Enables wolfCrypt only, disables TLS |

### 2.5.2 Enabling Features (On by Default)

| Define | Description |
|--------|-------------|
| `HAVE_TLS_EXTENSIONS` | TLS extensions support (required for most builds) |
| `HAVE_SUPPORTED_CURVES` | TLS supported curves and key share extensions |
| `HAVE_EXTENDED_MASTER` | Extended master secret PRF for TLS v1.2 and older |
| `HAVE_ENCRYPT_THEN_MAC` | Encrypt-then-mac support for block ciphers |
| `WOLFSSL_ASN_TEMPLATE` | Template-based ASN.1 processing |

### 2.5.3 Enabling Features Disabled by Default

| Define | Description |
|--------|-------------|
| `WOLFSSL_CERT_GEN` | Certificate generation functionality |
| `WOLFSSL_DER_LOAD` | Loading DER-formatted CA certs |
| `WOLFSSL_DTLS` | DTLS support |
| `WOLFSSL_KEY_GEN` | RSA key generation |
| `DEBUG_WOLFSSL` | Debug support |
| `HAVE_AESCCM` | AES-CCM support |
| `HAVE_AESGCM` | AES-GCM support |
| `WOLFSSL_AES_XTS` | AES-XTS support |
| `HAVE_CHACHA` | ChaCha20 support |
| `HAVE_POLY1305` | Poly1305 support |
| `HAVE_CRL` | Certificate Revocation List support |
| `HAVE_ECC` | Elliptical Curve Cryptography support |
| `HAVE_OCSP` | Online Certificate Status Protocol support |
| `OPENSSL_EXTRA` | Extended OpenSSL compatibility |
| `WOLFSSL_SHA3` | SHA3 support |
| `WOLFSSL_SHA512` | SHA-512 support |
| `WOLFSSL_TLS13` | TLS 1.3 protocol support |
| `WOLFSSL_DTLS13` | DTLS 1.3 support |

### 2.5.4 Customizing or Porting wolfSSL

| Define | Description |
|--------|-------------|
| `WOLFSSL_USER_SETTINGS` | Enables user-specific settings file (`user_settings.h`) |
| `WOLFSSL_USER_IO` | Custom I/O abstraction layer |
| `NO_FILESYSTEM` | Disables stdio for loading certificates/keys |
| `NO_INLINE` | Disables automatic function inlining |
| `NO_DEV_RANDOM` | Disables `/dev/random` |
| `SINGLE_THREADED` | Disables mutex usage |
| `USER_TICKS` | Custom clock tick function |
| `USE_CERT_BUFFERS_2048` | Enables 2048-bit test certificate buffers |

### 2.5.5 Reducing Memory or Code Usage

| Define | Description |
|--------|-------------|
| `TFM_TIMING_RESISTANT` | Reduces stack usage with fast math |
| `ECC_TIMING_RESISTANT` | Prevents side channel attacks in ECC |
| `WOLFSSL_SMALL_STACK` | Uses dynamic memory instead of stack |
| `ALT_ECC_SIZE` | Allocates ECC points from heap instead of stack |
| `RSA_LOW_MEM` | Disables CRT, saves memory but slower |
| `WOLFSSL_SHA3_SMALL` | Reduces SHA3 build size |
| `GCM_SMALL` | Calculates AES-GCM at runtime instead of using tables |
| `USE_SLOW_SHA` | Reduces SHA code size |
| `USE_SLOW_SHA256` | Reduces SHA-256 code size |
| `USE_SLOW_SHA512` | Reduces SHA-512 code size |

### 2.5.6 Increasing Performance

| Define | Description |
|--------|-------------|
| `USE_INTEL_SPEEDUP` | Intel AVX/AVX2 instructions for AES, ChaCha20, etc. |
| `WOLFSSL_AESNI` | AES-NI hardware acceleration |
| `HAVE_INTEL_RDSEED` | Intel RDSEED for DRBG seed |
| `HAVE_INTEL_RDRAND` | Intel RDRAND for random source |
| `FP_ECC` | ECC Fixed Point Cache |
| `WOLFSSL_ASYNC_CRYPT` | Asynchronous cryptography support |

### 2.5.7 wolfSSL's Math Options

wolfSSL has three math libraries:

1. **Big Integer Library** (deprecated) - Most portable, written in C, uses heap memory
2. **Fast Math Library** - Uses assembly optimizations, stack-based, timing resistant with `TFM_TIMING_RESISTANT`
3. **Single Precision (SP) Math Library** (recommended) - Optimized for speed, always timing resistant, DO-178C certified

**SP Math Defines:**

| Define | Description |
|--------|-------------|
| `WOLFSSL_SP` | Enable Single Precision math |
| `WOLFSSL_SP_MATH` | SP math only (restricted algorithms) |
| `WOLFSSL_SP_MATH_ALL` | SP math with full algorithm suite |
| `WOLFSSL_SP_SMALL` | Smaller SP code, avoids large stack variables |
| `WOLFSSL_HAVE_SP_RSA` | SP RSA for 2048, 3072, 4096 bit |
| `WOLFSSL_HAVE_SP_DH` | SP DH for 2048, 3072, 4096 bit |
| `WOLFSSL_HAVE_SP_ECC` | SP ECC for SECP256R1 and SECP384R1 |

## 2.6 Build Options

The following are options for the `./configure` script:

### Debug and Threading

| Option | Description |
|--------|-------------|
| `--enable-debug` | Enable debugging support |
| `--enable-singlethread` | Single threaded mode |

### Protocol Options

| Option | Description |
|--------|-------------|
| `--enable-dtls` | Enable DTLS support |
| `--enable-tls13` | Enable TLS 1.3 |
| `--disable-tlsv12` | Disable TLS 1.2 |
| `--disable-oldtls` | Disable TLS < 1.2 |

### OpenSSL Compatibility

| Option | Description |
|--------|-------------|
| `--enable-opensslextra` | Extra OpenSSL API compatibility |
| `--enable-opensslall` | All OpenSSL API |

### Cipher Options

| Option | Description |
|--------|-------------|
| `--enable-aesgcm` | AES-GCM support |
| `--enable-aesccm` | AES-CCM support |
| `--enable-aesni` | Intel AES-NI support |
| `--enable-chacha` | ChaCha20 support |
| `--enable-poly1305` | Poly1305 support |
| `--enable-camellia` | Camellia support |

### ECC Options

| Option | Description |
|--------|-------------|
| `--enable-ecc` | ECC support |
| `--enable-curve25519` | Curve25519 support |
| `--enable-ed25519` | Ed25519 support |

### Certificate Options

| Option | Description |
|--------|-------------|
| `--enable-certgen` | Certificate generation |
| `--enable-certreq` | Certificate request generation |
| `--enable-certext` | Certificate extensions |
| `--enable-keygen` | RSA key generation |
| `--enable-ocsp` | OCSP support |
| `--enable-crl` | CRL support |

### Math Options

| Option | Description |
|--------|-------------|
| `--enable-fastmath` | FastMath implementation |
| `--enable-sp-math` | Single-Precision math only |
| `--enable-sp-math-all` | SP math with full algorithm suite (default) |
| `--enable-sp=OPT` | SP math for specific algorithms |

## 2.7 Special Math Optimization Flags

See section 2.5.8 for detailed math library options.

## 2.8 Cross Compiling

When cross compiling, specify the host to `./configure`:

```bash
./configure --host=arm-linux
```

You may also need to specify compiler tools:

```bash
./configure --host=arm-linux CC=arm-linux-gcc AR=arm-linux-ar RANLIB=arm-linux-ranlib
```

## 2.9 Building Ports

wolfSSL has been ported to many environments. Port-specific files are in `wolfssl-X.X.X/IDE/`:

- Arduino, LPCXPRESSO, Wiced Studio
- SGX (Windows and Linux)
- Hexagon, Hexiwear
- NetBurner M68K, Renesas
- XCode, Eclipse, Espressif
- IAR-EWARM, Kinetis Design Studio
- OpenSTM32, RISCV, Zephyr, Mynewt

---

# 3. Getting Started

## 3.1 General Description

wolfSSL (formerly CyaSSL) is about 10 times smaller than yaSSL, and up to 20 times smaller than OpenSSL when using optimized compile options. User benchmarking reports dramatically better performance from wolfSSL vs. OpenSSL in standard SSL operations.

## 3.2 Testsuite

The testsuite program tests wolfSSL and wolfCrypt functionality. Run from the wolfSSL home directory:

```bash
./testsuite/testsuite.test
```

Or with autoconf:

```bash
make test
```

A successful run shows output like:

```
------------------------------------------------------------------------------
wolfSSL version 4.8.1
------------------------------------------------------------------------------
error test passed!
MEMORY test passed!
base64 test passed!
...
SHA-256 test passed!
...
AES test passed!
AES-GCM test passed!
RSA test passed!
DH test passed!
ECC test passed!
...
Test complete
```

## 3.3 Client Example

Test wolfSSL against any SSL server using the client example:

```bash
./examples/client/client --help
```

Example connecting to a public server:

```bash
./examples/client/client -h example.com -p 443 -d -g
```

## 3.4 Server Example

The server example listens for client connections:

```bash
./examples/server/server --help
```

Start a basic server:

```bash
./examples/server/server
```

Then connect with the client:

```bash
./examples/client/client
```

## 3.5 EchoServer Example

The echoserver echoes any received data back to the client:

```bash
./examples/echoserver/echoserver
```

## 3.6 EchoClient Example

Connect to the echoserver:

```bash
./examples/echoclient/echoclient
```

## 3.7 Benchmark

The benchmark application tests wolfCrypt algorithm performance:

```bash
./wolfcrypt/benchmark/benchmark
```

Example output:

```
wolfCrypt Benchmark (block bytes 1048576, min 1.0 sec each)
RNG                105 MiB took 1.004 seconds,  104.576 MiB/s
AES-128-CBC-enc    395 MiB took 1.001 seconds,  394.478 MiB/s
AES-128-CBC-dec    380 MiB took 1.003 seconds,  378.723 MiB/s
AES-256-CBC-enc    290 MiB took 1.002 seconds,  289.532 MiB/s
AES-256-CBC-dec    285 MiB took 1.001 seconds,  284.695 MiB/s
AES-128-GCM-enc    185 MiB took 1.009 seconds,  183.510 MiB/s
AES-128-GCM-dec    185 MiB took 1.001 seconds,  184.740 MiB/s
...
RSA     2048   public       6400 ops took 1.008 sec, avg 0.158 ms, 6349.206 ops/sec
RSA     2048  private        300 ops took 1.073 sec, avg 3.576 ms, 279.647 ops/sec
ECC   [SECP256R1]   256  key gen      2200 ops took 1.012 sec, avg 0.460 ms, 2173.913 ops/sec
ECDHE [SECP256R1]   256    agree      2500 ops took 1.006 sec, avg 0.403 ms, 2483.559 ops/sec
ECDSA [SECP256R1]   256     sign      3100 ops took 1.001 sec, avg 0.323 ms, 3096.904 ops/sec
ECDSA [SECP256R1]   256   verify      1200 ops took 1.007 sec, avg 0.839 ms, 1191.658 ops/sec
```

## 3.8 Changing a Client Application to Use wolfSSL

Basic steps to add wolfSSL to an existing client:

1. Include the wolfSSL header:
   ```c
   #include <wolfssl/ssl.h>
   ```

2. Initialize wolfSSL:
   ```c
   wolfSSL_Init();
   ```

3. Create a context:
   ```c
   WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfTLSv1_2_client_method());
   ```

4. Load CA certificates:
   ```c
   wolfSSL_CTX_load_verify_locations(ctx, "ca-cert.pem", NULL);
   ```

5. Create an SSL object and associate with socket:
   ```c
   WOLFSSL* ssl = wolfSSL_new(ctx);
   wolfSSL_set_fd(ssl, sockfd);
   ```

6. Perform SSL handshake:
   ```c
   wolfSSL_connect(ssl);
   ```

7. Send/receive data:
   ```c
   wolfSSL_write(ssl, data, len);
   wolfSSL_read(ssl, buffer, sizeof(buffer));
   ```

8. Cleanup:
   ```c
   wolfSSL_free(ssl);
   wolfSSL_CTX_free(ctx);
   wolfSSL_Cleanup();
   ```

## 3.9 Changing a Server Application to Use wolfSSL

Similar to the client, but use server methods and load server certificates:

```c
WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfTLSv1_2_server_method());
wolfSSL_CTX_use_certificate_file(ctx, "server-cert.pem", SSL_FILETYPE_PEM);
wolfSSL_CTX_use_PrivateKey_file(ctx, "server-key.pem", SSL_FILETYPE_PEM);

// After accept()
WOLFSSL* ssl = wolfSSL_new(ctx);
wolfSSL_set_fd(ssl, clientfd);
wolfSSL_accept(ssl);
```

---

# 4. Features

wolfSSL supports the C programming language as a primary interface, but also supports several other host languages, including Java, PHP, Perl, and Python (through a SWIG interface).

## 4.1 Features Overview

For an overview of wolfSSL features, please reference the wolfSSL product webpage: https://www.wolfssl.com/products/wolfssl

## 4.2 Protocol Support

wolfSSL supports SSL 3.0, TLS (1.0, 1.1, 1.2, 1.3), and DTLS (1.0, 1.2, 1.3). wolfSSL does not support SSL 2.0, as it has been insecure for several years.

### 4.2.1 Server Functions

| Function | Protocol |
|----------|----------|
| `wolfDTLSv1_server_method()` | DTLS 1.0 |
| `wolfDTLSv1_2_server_method()` | DTLS 1.2 |
| `wolfSSLv3_server_method()` | SSL 3.0 |
| `wolfTLSv1_server_method()` | TLS 1.0 |
| `wolfTLSv1_1_server_method()` | TLS 1.1 |
| `wolfTLSv1_2_server_method()` | TLS 1.2 |
| `wolfTLSv1_3_server_method()` | TLS 1.3 |
| `wolfSSLv23_server_method()` | SSLv3 - TLS 1.2 (highest available) |

### 4.2.2 Client Functions

| Function | Protocol |
|----------|----------|
| `wolfDTLSv1_client_method()` | DTLS 1.0 |
| `wolfDTLSv1_2_client_method_ex()` | DTLS 1.2 |
| `wolfSSLv3_client_method()` | SSL 3.0 |
| `wolfTLSv1_client_method()` | TLS 1.0 |
| `wolfTLSv1_1_client_method()` | TLS 1.1 |
| `wolfTLSv1_2_client_method()` | TLS 1.2 |
| `wolfTLSv1_3_client_method()` | TLS 1.3 |
| `wolfSSLv23_client_method()` | SSLv3 - TLS 1.2 (highest available) |

### 4.2.3 Robust Client and Server Downgrade

Both wolfSSL clients and servers have robust version downgrade capability. Using `wolfSSLv23_client_method()` or `wolfSSLv23_server_method()` allows negotiation to the highest protocol version supported by both sides.

### 4.2.4 IPv6 Support

wolfSSL supports IPv6. The library was designed as IP neutral and will work with both IPv4 and IPv6. To enable IPv6 in test applications:

```bash
./configure --enable-ipv6
```

### 4.2.5 DTLS

wolfSSL has support for DTLS ("Datagram" TLS) for both client and server. Supported versions are DTLS 1.0, 1.2, and 1.3.

```bash
./configure --enable-dtls
```

### 4.2.6 TLS Extensions

| RFC | Extension | wolfSSL Type |
|-----|-----------|--------------|
| 6066 | Server Name Indication | `TLSX_SERVER_NAME` |
| 6066 | Maximum Fragment Length | `TLSX_MAX_FRAGMENT_LENGTH` |
| 6066 | Truncated HMAC | `TLSX_TRUNCATED_HMAC` |
| 6066 | Status Request | `TLSX_STATUS_REQUEST` |
| 7919 | Supported Groups | `TLSX_SUPPORTED_GROUPS` |
| 5246 | Signature Algorithm | `TLSX_SIGNATURE_ALGORITHMS` |
| 7301 | ALPN | `TLSX_APPLICATION_LAYER_PROTOCOL` |
| 5077 | Session Ticket | `TLSX_SESSION_TICKET` |
| 5746 | Renegotiation Indication | `TLSX_RENEGOTIATION_INFO` |
| 8446 | Key Share | `TLSX_KEY_SHARE` |
| 8446 | Pre Shared Key | `TLSX_PRE_SHARED_KEY` |
| 8446 | PSK Key Exchange Modes | `TLSX_PSK_KEY_EXCHANGE_MODES` |
| 8446 | Early Data | `TLSX_EARLY_DATA` |
| 8446 | Supported Versions | `TLSX_SUPPORTED_VERSIONS` |

## 4.3 Cipher Support

### 4.3.1 Cipher Suite Strength

Cipher suite strength classification based on symmetric encryption algorithm:

- **LOW** - bits of security smaller than 128 bits
- **MEDIUM** - bits of security equal to 128 bits
- **HIGH** - bits of security larger than 128 bits

### 4.3.2 Supported Cipher Suites

**ECC cipher suites:**
- `TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA`
- `TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA`
- `TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA`
- `TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA`
- `TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256`
- `TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256`
- `TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384`
- `TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384`

**AES-GCM cipher suites:**
- `TLS_RSA_WITH_AES_128_GCM_SHA256`
- `TLS_RSA_WITH_AES_256_GCM_SHA384`
- `TLS_DHE_RSA_WITH_AES_128_GCM_SHA256`
- `TLS_DHE_RSA_WITH_AES_256_GCM_SHA384`
- `TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256`
- `TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384`
- `TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256`
- `TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384`

**ChaCha cipher suites:**
- `TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256`
- `TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256`
- `TLS_DHE_RSA_WITH_CHACHA20_POLY1305_SHA256`

**PSK cipher suites:**
- `TLS_PSK_WITH_AES_256_CBC_SHA`
- `TLS_PSK_WITH_AES_128_CBC_SHA256`
- `TLS_PSK_WITH_AES_128_GCM_SHA256`
- `TLS_PSK_WITH_AES_256_GCM_SHA384`
- `TLS_PSK_WITH_CHACHA20_POLY1305_SHA256`

### 4.3.3 AEAD Suites

wolfSSL supports AEAD suites including AES-GCM, AES-CCM, and CHACHA-POLY1305. These authenticate encrypted data with any additional cleartext data, helping mitigate man-in-the-middle attacks.

### 4.3.4 Block and Stream Ciphers

- **Block ciphers:** AES, DES, 3DES, Camellia
- **Stream ciphers:** RC4, ChaCha20

AES modes: CBC (default), GCM (`--enable-aesgcm`), CCM (`--enable-aesccm`)

### 4.3.5 Hashing Functions

Supported hash functions: MD2, MD4, MD5, SHA-1, SHA-2 (SHA-224, SHA-256, SHA-384, SHA-512), SHA-3, BLAKE2, RIPEMD-160.

### 4.3.6 Public Key Options

wolfSSL supports RSA, ECC, DSA/DSS, and DH public key options, with support for EDH (Ephemeral Diffie-Hellman).

**Post-Quantum Algorithms:**
- **ML-KEM** (Module Lattice Key Encapsulation Mechanism) - NIST FIPS 203
- **ML-DSA** (Module Lattice Digital Signature Algorithm) - NIST FIPS 204

### 4.3.7 ECC Support

Enable ECC with:

```bash
./configure --enable-ecc
```

Example server with ECDHE-ECDSA:

```bash
./examples/server/server -d -l ECDHE-ECDSA-AES256-SHA -c ./certs/server-ecc.pem -k ./certs/ecc-key.pem
```

### 4.3.8 PKCS Support

wolfSSL supports PKCS #1, #3, #5, #7, #8, #9, #10, #11, and #12.

**PKCS #5 (PBKDF):**

```c
int PBKDF2(byte* output, const byte* passwd, int pLen,
           const byte* salt, int sLen, int iterations,
           int kLen, int hashType);
```

Enable with `--enable-pwdbased`.

**PKCS #7:**

Enable with `--enable-pkcs7`. Supports:
- Degenerate bundles
- KARI, KEKRI, PWRI, ORI, KTRI bundles
- Detached signatures
- Compressed and Firmware package bundles

### 4.3.9 Forcing a Specific Cipher

```c
ctx = wolfSSL_CTX_new(method);
wolfSSL_CTX_set_cipher_list(ctx, "AES128-SHA");
```

## 4.4 Hardware Accelerated Crypto

### 4.4.1 AES-NI

Intel AES-NI provides 5-10x faster AES operations:

```bash
./configure --enable-aesni
```

AES-NI accelerates: AES-CBC, AES-GCM, AES-CCM-8, AES-CCM, and AES-CTR.

### 4.4.2 STM32F2

Enable with `WOLFSSL_STM32F2` define. Supports hardware crypto and RNG.

### 4.4.3 Cavium NITROX

```bash
./configure --with-cavium=/home/user/cavium/software
```

Supports RNG, AES, 3DES, RC4, HMAC, and RSA.

### 4.4.4 ESP32-WROOM-32

Enable with `WOLFSSL_ESPWROOM32`. Supports RNG, AES, SHA, and RSA.

### 4.4.5 EFR32

Enable with `WOLFSSL_SILABS_SE_ACCEL`. Supports RNG, AES-CBC, AES-GCM, AES-CCM, SHA-1, SHA-2, ECDHE, and ECDSA.

## 4.5 SSL Inspection (Sniffer)

Build with sniffer support:

```bash
./configure --enable-sniffer
```

Key functions in `sniffer.h`:
- `ssl_SetPrivateKey` - Sets private key for server/port
- `ssl_DecodePacket` - Decodes TCP/IP packet
- `ssl_Trace` - Enables/disables debug tracing
- `ssl_InitSniffer` - Initialize sniffer
- `ssl_FreeSniffer` - Free sniffer
- `ssl_EnableRecovery` - Handle lost packets

**Note:** The sniffer can only decode streams encrypted with AES-CBC, DES3-CBC, ARC4, and Camellia-CBC. ECDHE or DHE key exchange cannot be sniffed.

## 4.6 Static Buffer Allocation Option

wolfSSL can be configured without dynamic memory allocation for environments without malloc/free or safety-critical applications.

### 4.6.1 Enabling Static Buffer Allocation

```bash
./configure --enable-staticmemory
```

Or in `user_settings.h`:

```c
#define WOLFSSL_STATIC_MEMORY
#define WOLFSSL_NO_MALLOC  // if no malloc available
```

### 4.6.2 Using Static Buffer Allocation

```c
WOLFSSL_CTX* ctx = NULL;
unsigned char GEN_MEM[GEN_MEM_SIZE];
unsigned char IO_MEM[IO_MEM_SIZE];

// First call: set up general-purpose buffer and generate WOLFSSL_CTX
wolfSSL_CTX_load_static_memory(
    &ctx,
    wolfSSLv23_client_method_ex(),
    GEN_MEM, GEN_MEM_SIZE,
    WOLFMEM_GENERAL,
    MAX_CONCURRENT_TLS);

// Second call: set up I/O buffer
wolfSSL_CTX_load_static_memory(
    &ctx,
    NULL,
    IO_MEM, IO_MEM_SIZE,
    WOLFMEM_IO_FIXED,
    MAX_CONCURRENT_IO);
```

## 4.7 Compression

wolfSSL supports data compression with zlib. Define `HAVE_LIBZ` and use:

```c
wolfSSL_set_compression(ssl);  // before connect/accept
```

Both client and server must have compression enabled.

## 4.8 Pre-Shared Keys

Supported PSK cipher suites include:
- `TLS_PSK_WITH_AES_256_CBC_SHA`
- `TLS_PSK_WITH_AES_128_GCM_SHA256`
- `TLS_PSK_WITH_CHACHA20_POLY1305`
- `ECDHE-PSK-AES128-CBC-SHA256`
- `DHE-PSK-AES256-GCM-SHA384`

**Client setup:**

```c
wolfSSL_CTX_set_psk_client_callback(ctx, my_psk_client_cb);
```

**Server setup:**

```c
wolfSSL_CTX_set_psk_server_callback(ctx, my_psk_server_cb);
wolfSSL_CTX_use_psk_identity_hint(ctx, "wolfssl server");
```

## 4.9 Client Authentication

To require client certificates on the server:

```c
wolfSSL_CTX_load_verify_locations(ctx, caCert, 0);
wolfSSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, 0);
```

## 4.10 Server Name Indication (SNI)

Enable SNI:

```bash
./configure --enable-sni
```

**Client side:**

```c
wolfSSL_CTX_UseSNI(ctx, WOLFSSL_SNI_HOST_NAME, "example.com", strlen("example.com"));
// or per-session:
wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, "example.com", strlen("example.com"));
```

## 4.11 Handshake Modifications

### 4.11.1 Grouping Handshake Messages

```c
wolfSSL_CTX_set_group_messages(ctx);
```

## 4.12 Truncated HMAC

Enable truncated (80-bit) HMAC for bandwidth-constrained environments:

```bash
./configure --enable-truncatedhmac
```

```c
wolfSSL_CTX_UseTruncatedHMAC(ctx);
// or per-session:
wolfSSL_UseTruncatedHMAC(ssl);
```

## 4.13 Timing-Resistance

wolfSSL provides timing-resistant implementations to prevent side-channel attacks:

- `ConstantCompare` - constant-time comparison
- `ECC_TIMING_RESISTANT` - ECC timing resistance
- `TFM_TIMING_RESISTANT` - RSA timing resistance
- `WC_RSA_BLINDING` - RSA blinding mode

Enable with `--enable-harden`.

## 4.14 Fixed ABI

Starting with wolfSSL v4.3.0, a subset of API functions have a fixed ABI for binary compatibility across releases:

- `wolfSSL_Init()`, `wolfSSL_Cleanup()`
- `wolfSSL_CTX_new()`, `wolfSSL_CTX_free()`
- `wolfSSL_new()`, `wolfSSL_free()`
- `wolfSSL_connect()`, `wolfSSL_accept()`
- `wolfSSL_read()`, `wolfSSL_write()`
- `wolfSSL_set_fd()`, `wolfSSL_get_error()`
- And many more...

---

# 5. Portability

## 5.1 Abstraction Layers

### 5.1.1 C Standard Library Abstraction Layer

wolfSSL can be built without the C standard library for maximum portability.

**Memory:**

wolfSSL uses `XMALLOC()` and `XFREE()` instead of malloc/free. Define `XMALLOC_USER` to provide custom implementations:

```c
int wolfSSL_SetAllocators(wolfSSL_Malloc_cb malloc_function,
                          wolfSSL_Free_cb free_function,
                          wolfSSL_Realloc_cb realloc_function);
```

**String functions:**

Define `STRING_USER` to override `XMEMCPY()`, `XMEMSET()`, `XMEMCMP()`, etc.

**File System:**

Define `NO_FILESYSTEM` to disable file system usage, or use `XFILE()` layer for custom file operations.

### 5.1.2 Custom Input/Output Abstraction Layer

For custom I/O or non-TCP transport:

```c
typedef int (*CallbackIORecv)(WOLFSSL *ssl, char *buf, int sz, void *ctx);
typedef int (*CallbackIOSend)(WOLFSSL *ssl, char *buf, int sz, void *ctx);

wolfSSL_SetIORecv(ctx, my_recv_callback);
wolfSSL_SetIOSend(ctx, my_send_callback);

// Set context per session
wolfSSL_SetIOReadCtx(ssl, my_context);
wolfSSL_SetIOWriteCtx(ssl, my_context);
```

Define `WOLFSSL_USER_IO` to disable default I/O functions.

### 5.1.3 Operating System Abstraction Layer

OS-specific defines are in `wolfssl/wolfcrypt/types.h` and `wolfssl/internal.h`.

## 5.2 Supported Operating Systems

- Windows (Win32/64, WinCE)
- Linux (embedded, Yocto, OpenEmbedded, OpenWRT)
- Mac OS X, iOS
- FreeBSD, NetBSD, OpenBSD
- Android
- ThreadX, VxWorks, QNX
- FreeRTOS, SafeRTOS
- Micrium µC/OS-III
- TRON/ITRON/µITRON
- Nucleus, TinyOS
- Solaris, HP/UX, AIX
- Haiku
- Nintendo Wii/Gamecube
- And many more...

## 5.3 Supported Chipmakers

wolfSSL has been ported to chips from:

- ARM
- Intel
- AMD
- Motorola
- Texas Instruments
- STMicroelectronics
- NXP/Freescale
- Analog Devices
- Microchip
- Atmel
- Marvell
- Silicon Labs
- Espressif
- Nordic Semiconductor
- Renesas
- Infineon
- And many more...

## 5.4 C# Wrapper

wolfSSL provides a C# wrapper for .NET applications. See the wolfSSL C# wrapper documentation for details.

---

# 6. Callbacks

wolfSSL provides several callback options for customization.

## 6.1 Handshake Callback

```c
typedef int (*HandShakeCallBack)(HandShakeInfo*);

int wolfSSL_CTX_SetHandShakeCallback(WOLFSSL_CTX* ctx, HandShakeCallBack cb);
```

## 6.2 Timeout Callback

For non-blocking sockets with custom timeout handling:

```c
typedef int (*TimeoutCallBack)(TimeoutInfo*);

int wolfSSL_CTX_SetTimeoutCallback(WOLFSSL_CTX* ctx, TimeoutCallBack cb);
```

## 6.3 Verification Callback

Custom certificate verification:

```c
typedef int (*VerifyCallback)(int, WOLFSSL_X509_STORE_CTX*);

void wolfSSL_CTX_set_verify(WOLFSSL_CTX* ctx, int mode, VerifyCallback cb);
```

## 6.4 I/O Callbacks

See section 5.1.2 for custom I/O callbacks.

## 6.5 Public Key Callbacks

For hardware crypto or custom key operations:

```c
wolfSSL_CTX_SetEccSignCb(ctx, my_ecc_sign_cb);
wolfSSL_CTX_SetEccVerifyCb(ctx, my_ecc_verify_cb);
wolfSSL_CTX_SetRsaSignCb(ctx, my_rsa_sign_cb);
wolfSSL_CTX_SetRsaVerifyCb(ctx, my_rsa_verify_cb);
wolfSSL_CTX_SetRsaEncCb(ctx, my_rsa_enc_cb);
wolfSSL_CTX_SetRsaDecCb(ctx, my_rsa_dec_cb);
```

## 6.6 Atomic Record Layer Callbacks

For custom MAC/encrypt and decrypt/verify:

```c
wolfSSL_CTX_SetMacEncryptCb(ctx, my_mac_encrypt_cb);
wolfSSL_CTX_SetDecryptVerifyCb(ctx, my_decrypt_verify_cb);
```

## 6.7 Crypto Callbacks (cryptocb)

The crypto callback framework enables users to override default cryptographic algorithm implementations at runtime.

### 6.7.1 Enabling Crypto Callbacks

```bash
./configure --enable-cryptocb
```

Or define `WOLF_CRYPTO_CB`.

### 6.7.2 Registering a Callback

```c
typedef int (*CryptoDevCallbackFunc)(int devId, wc_CryptoInfo* info, void* ctx);

wc_CryptoCb_RegisterDevice(devId, my_crypto_callback, my_context);
```

### 6.7.3 Using with TLS

```c
wolfSSL_CTX_SetDevId(ctx, devId);
wolfSSL_SetDevId(ssl, devId);
```

### 6.7.4 Callback Implementation

```c
int myCryptoCallback(int devId, wc_CryptoInfo* info, void* ctx)
{
    int ret = CRYPTOCB_UNAVAILABLE;  // Fall back to default
    
    switch (info->algo_type) {
        case WC_ALGO_TYPE_PK:
            // Handle public key operations
            break;
        case WC_ALGO_TYPE_HASH:
            // Handle hashing
            break;
        case WC_ALGO_TYPE_CIPHER:
            // Handle cipher operations
            break;
        case WC_ALGO_TYPE_RNG:
            // Handle RNG
            break;
    }
    return ret;
}
```

---

# 7. Keys and Certificates

## 7.1 Supported Formats and Sizes

wolfSSL supports:
- **PEM** - Base64 encoded ASCII (`.pem`, `.crt`, `.cer`, `.key`)
- **DER** - Binary ASN.1 encoding (`.der`, `.cer`)
- **PKCS#8** - Private key format (with PKCS#5 or PKCS#12 encryption)

## 7.2 Supported Certificate Extensions

| Extension | Supported |
|-----------|-----------|
| Authority Key Identifier | Yes |
| Subject Key Identifier | Yes |
| Key Usage | Yes |
| Certificate Policies | Yes |
| Subject Alternative Name | Yes |
| Basic Constraints | Yes |
| Name Constraints | Yes |
| Extended Key Usage | Yes |
| CRL Distribution Points | Yes |
| Custom OID | Yes |

### 7.2.1 Subject Alternative Names

Supported types: email, DNS name, IP address, URI.

### 7.2.2 Key Usage

Supported: digitalSignature, nonRepudiation, keyEncipherment, dataEncipherment, keyAgreement, keyCertSign, cRLSign, encipherOnly, decipherOnly.

### 7.2.3 Extended Key Usage

Supported: anyExtendedKeyUsage, serverAuth, clientAuth, codeSigning, emailProtection, timeStamping, OCSPSigning.

## 7.3 Certificate Loading

### 7.3.1 Loading CA Certificates

```c
wolfSSL_CTX_load_verify_locations(ctx, "ca-cert.pem", NULL);
```

**Note:** Load certificates in order of trust: ROOT CA first, then intermediates.

### 7.3.2 Loading Client/Server Certificates

```c
// Single certificate
wolfSSL_CTX_use_certificate_file(ctx, "server-cert.pem", SSL_FILETYPE_PEM);

// Certificate chain (subject cert first, then intermediates, root last)
wolfSSL_CTX_use_certificate_chain_file(ctx, "server-chain.pem");
```

### 7.3.3 Loading Private Keys

```c
wolfSSL_CTX_use_PrivateKey_file(ctx, "server-key.pem", SSL_FILETYPE_PEM);
```

### 7.3.4 Loading Trusted Peer Certificates

```c
wolfSSL_CTX_trust_peer_cert(ctx, "peer-cert.pem", SSL_FILETYPE_PEM);
```

## 7.4 Certificate Chain Verification

wolfSSL only requires the root certificate to be loaded as trusted to verify an entire chain.

## 7.5 Domain Name Check

```c
wolfSSL_check_domain_name(ssl, "example.com");
```

Returns `DOMAIN_NAME_MISMATCH` if verification fails.

## 7.6 No File System (Buffer Loading)

Define `NO_FILESYSTEM` and use buffer functions:

```c
wolfSSL_CTX_load_verify_buffer(ctx, cert_buffer, cert_size, SSL_FILETYPE_PEM);
wolfSSL_CTX_use_certificate_buffer(ctx, cert_buffer, cert_size, SSL_FILETYPE_PEM);
wolfSSL_CTX_use_PrivateKey_buffer(ctx, key_buffer, key_size, SSL_FILETYPE_PEM);
wolfSSL_CTX_use_certificate_chain_buffer(ctx, chain_buffer, chain_size);
```

## 7.7 RSA Key Generation

Enable with `--enable-keygen` or `WOLFSSL_KEY_GEN`.

```c
RsaKey genKey;
RNG rng;

wc_InitRng(&rng);
wc_InitRsaKey(&genKey, NULL);
wc_MakeRsaKey(&genKey, 2048, 65537, &rng);

// Export to DER
byte der[4096];
int derSz = wc_RsaKeyToDer(&genKey, der, sizeof(der));

// Export to PEM
byte pem[4096];
int pemSz = wc_DerToPem(der, derSz, pem, sizeof(pem), PRIVATEKEY_TYPE);
```

## 7.8 Certificate Generation

Enable with `--enable-certgen` or `WOLFSSL_CERT_GEN`.

```c
Cert myCert;
wc_InitCert(&myCert);

// Set subject info
strncpy(myCert.subject.country, "US", CTC_NAME_SIZE);
strncpy(myCert.subject.state, "OR", CTC_NAME_SIZE);
strncpy(myCert.subject.org, "MyOrg", CTC_NAME_SIZE);
strncpy(myCert.subject.commonName, "www.example.com", CTC_NAME_SIZE);

// Self-signed certificate
byte derCert[4096];
int certSz = wc_MakeSelfCert(&myCert, derCert, sizeof(derCert), &key, &rng);

// CA-signed certificate
wc_SetIssuer(&myCert, "ca-cert.pem");
certSz = wc_MakeCert(&myCert, derCert, sizeof(derCert), &key, NULL, &rng);
certSz = wc_SignCert(myCert.bodySz, myCert.sigType, derCert, sizeof(derCert), &caKey, NULL, &rng);
```

## 7.9 Certificate Signing Request (CSR) Generation

Enable with `--enable-certreq --enable-certgen`.

```c
Cert request;
wc_InitCert(&request);

// Set subject info...

byte der[4096];
int ret = wc_MakeCertReq(&request, der, sizeof(der), NULL, &key);
request.sigType = CTC_SHA256wECDSA;
int derSz = wc_SignCert(request.bodySz, request.sigType, der, sizeof(der), NULL, &key, &rng);

byte pem[4096];
wc_DerToPem(der, derSz, pem, sizeof(pem), CERTREQ_TYPE);
```

---

# 8. Debugging

## 8.1 Debugging and Logging

Enable debug output:

```c
wolfSSL_Debugging_ON();
wolfSSL_Debugging_OFF();
```

Requires building with `DEBUG_WOLFSSL` defined.

### 8.1.1 Custom Logging Callback

```c
typedef void (*wolfSSL_Logging_cb)(const int logLevel, const char* const logMessage);

wolfSSL_SetLoggingCb(my_logging_function);
```

## 8.2 Error Codes

```c
int err = wolfSSL_get_error(ssl, result);

char errorString[80];
wolfSSL_ERR_error_string(err, errorString);
```

For non-blocking sockets, test for `SSL_ERROR_WANT_READ` or `SSL_ERROR_WANT_WRITE`.

---

# 9. Library Design

## 9.1 Library Headers

```c
// Native wolfSSL API
#include <wolfssl/ssl.h>

// OpenSSL Compatibility Layer
#include <wolfssl/openssl/ssl.h>
```

## 9.2 Startup and Exit

```c
wolfSSL_Init();      // Call at startup
wolfSSL_Cleanup();   // Call at shutdown
```

## 9.3 Structure Usage

| Native API | OpenSSL Compat |
|------------|----------------|
| `WOLFSSL` | `SSL` |
| `WOLFSSL_CTX` | `SSL_CTX` |
| `WOLFSSL_METHOD` | `SSL_METHOD` |
| `WOLFSSL_SESSION` | `SSL_SESSION` |
| `WOLFSSL_X509` | `X509` |

## 9.4 Thread Safety

wolfSSL is thread-safe by design:

1. **WOLFSSL objects** - Can be shared across threads but access must be synchronized
2. **WOLFSSL_CTX** - Initialize completely before creating WOLFSSL objects
3. **ECC cache** - Call `wc_ecc_fp_free()` before thread exit if using fixed-point ECC

## 9.5 Input and Output Buffers

- Default buffer size: controlled by `RECORD_SIZE` (default 128 bytes)
- Maximum: `MAX_RECORD_SIZE` (16,384 bytes)
- Define `LARGE_STATIC_BUFFERS` for 16KB static buffers
- Define `STATIC_CHUNKS_ONLY` to limit writes to `RECORD_SIZE`

---

# 10. wolfCrypt Usage Reference

## 10.1 Hash Functions

### 10.1.1 SHA Family

```c
#include <wolfssl/wolfcrypt/sha256.h>

Sha256 sha;
byte hash[SHA256_DIGEST_SIZE];
byte data[1024];

wc_InitSha256(&sha);
wc_Sha256Update(&sha, data, sizeof(data));
wc_Sha256Final(&sha, hash);
```

Similar API for SHA, SHA224, SHA384, SHA512.

### 10.1.2 BLAKE2b

```c
#include <wolfssl/wolfcrypt/blake2.h>

Blake2b b2b;
byte digest[64];

wc_InitBlake2b(&b2b, 64);
wc_Blake2bUpdate(&b2b, input, inputLen);
wc_Blake2bFinal(&b2b, digest, 64);
```

## 10.2 Keyed Hash Functions

### 10.2.1 HMAC

```c
#include <wolfssl/wolfcrypt/hmac.h>

Hmac hmac;
byte key[32];
byte digest[SHA256_DIGEST_SIZE];

wc_HmacSetKey(&hmac, SHA256, key, sizeof(key));
wc_HmacUpdate(&hmac, data, dataLen);
wc_HmacFinal(&hmac, digest);
```

### 10.2.2 Poly1305

```c
#include <wolfssl/wolfcrypt/poly1305.h>

Poly1305 pmac;
byte key[32];
byte digest[16];

wc_Poly1305SetKey(&pmac, key, sizeof(key));
wc_Poly1305Update(&pmac, data, dataLen);
wc_Poly1305Final(&pmac, digest);
```

## 10.3 Block Ciphers

### 10.3.1 AES

```c
#include <wolfssl/wolfcrypt/aes.h>

Aes aes;
byte key[32];
byte iv[16];
byte plain[32], cipher[32];

wc_AesInit(&aes, NULL, INVALID_DEVID);
wc_AesSetKey(&aes, key, sizeof(key), iv, AES_ENCRYPTION);
wc_AesCbcEncrypt(&aes, cipher, plain, sizeof(plain));

wc_AesSetKey(&aes, key, sizeof(key), iv, AES_DECRYPTION);
wc_AesCbcDecrypt(&aes, plain, cipher, sizeof(cipher));
```

**AES-GCM:**

```c
wc_AesGcmSetKey(&aes, key, keyLen);
wc_AesGcmEncrypt(&aes, cipher, plain, plainLen, iv, ivLen, tag, tagLen, aad, aadLen);
wc_AesGcmDecrypt(&aes, plain, cipher, cipherLen, iv, ivLen, tag, tagLen, aad, aadLen);
```

### 10.3.2 3DES

```c
#include <wolfssl/wolfcrypt/des3.h>

Des3 des3;
byte key[24], iv[8];

wc_Des3_SetKey(&des3, key, iv, DES_ENCRYPTION);
wc_Des3_CbcEncrypt(&des3, cipher, plain, sizeof(plain));
```

## 10.4 Stream Ciphers

### 10.4.1 ChaCha20

```c
#include <wolfssl/wolfcrypt/chacha.h>

ChaCha cha;
byte key[32], iv[12];

wc_Chacha_SetKey(&cha, key, sizeof(key));
wc_Chacha_SetIV(&cha, iv, 0);
wc_Chacha_Process(&cha, cipher, plain, sizeof(plain));
```

## 10.5 Public Key Cryptography

### 10.5.1 RSA

```c
#include <wolfssl/wolfcrypt/rsa.h>

RsaKey rsa;
RNG rng;

wc_InitRsaKey(&rsa, NULL);
wc_InitRng(&rng);

// Encrypt
word32 outLen = wc_RsaPublicEncrypt(in, inLen, out, outSz, &rsa, &rng);

// Decrypt
word32 plainLen = wc_RsaPrivateDecrypt(cipher, cipherLen, plain, plainSz, &rsa);

wc_FreeRsaKey(&rsa);
```

### 10.5.2 ECC

```c
#include <wolfssl/wolfcrypt/ecc.h>

ecc_key key;
RNG rng;

wc_ecc_init(&key);
wc_InitRng(&rng);
wc_ecc_make_key(&rng, 32, &key);  // 256-bit key

// Sign
byte sig[ECC_MAX_SIG_SIZE];
word32 sigLen = sizeof(sig);
wc_ecc_sign_hash(hash, hashLen, sig, &sigLen, &rng, &key);

// Verify
int verified = 0;
wc_ecc_verify_hash(sig, sigLen, hash, hashLen, &verified, &key);

wc_ecc_free(&key);
```

### 10.5.3 Diffie-Hellman

```c
#include <wolfssl/wolfcrypt/dh.h>

DhKey dh;
byte priv[128], pub[128], agree[128];
word32 privSz, pubSz, agreeSz;

wc_InitDhKey(&dh);
wc_DhKeyDecode(dhParams, &idx, &dh, dhParamsSz);
wc_DhGenerateKeyPair(&dh, &rng, priv, &privSz, pub, &pubSz);
wc_DhAgree(&dh, agree, &agreeSz, priv, privSz, peerPub, peerPubSz);

wc_FreeDhKey(&dh);
```

---

# 11. SSL Tutorial

## 11.1 Quick Summary

1. Initialize wolfSSL: `wolfSSL_Init()`
2. Create context: `wolfSSL_CTX_new(method)`
3. Load certificates and keys
4. Create SSL object: `wolfSSL_new(ctx)`
5. Associate socket: `wolfSSL_set_fd(ssl, sockfd)`
6. Handshake: `wolfSSL_connect()` or `wolfSSL_accept()`
7. Transfer data: `wolfSSL_read()`, `wolfSSL_write()`
8. Cleanup: `wolfSSL_free()`, `wolfSSL_CTX_free()`, `wolfSSL_Cleanup()`

## 11.2 Client Example

```c
#include <wolfssl/ssl.h>

int main() {
    wolfSSL_Init();
    
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    wolfSSL_CTX_load_verify_locations(ctx, "ca-cert.pem", NULL);
    
    int sockfd = /* create and connect socket */;
    
    WOLFSSL* ssl = wolfSSL_new(ctx);
    wolfSSL_set_fd(ssl, sockfd);
    
    if (wolfSSL_connect(ssl) != SSL_SUCCESS) {
        // Handle error
    }
    
    wolfSSL_write(ssl, "Hello", 5);
    
    char buffer[256];
    int ret = wolfSSL_read(ssl, buffer, sizeof(buffer));
    
    wolfSSL_free(ssl);
    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return 0;
}
```

## 11.3 Server Example

```c
#include <wolfssl/ssl.h>

int main() {
    wolfSSL_Init();
    
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
    wolfSSL_CTX_use_certificate_file(ctx, "server-cert.pem", SSL_FILETYPE_PEM);
    wolfSSL_CTX_use_PrivateKey_file(ctx, "server-key.pem", SSL_FILETYPE_PEM);
    
    int listenfd = /* create and bind socket */;
    listen(listenfd, 5);
    
    int clientfd = accept(listenfd, NULL, NULL);
    
    WOLFSSL* ssl = wolfSSL_new(ctx);
    wolfSSL_set_fd(ssl, clientfd);
    
    if (wolfSSL_accept(ssl) != SSL_SUCCESS) {
        // Handle error
    }
    
    char buffer[256];
    int ret = wolfSSL_read(ssl, buffer, sizeof(buffer));
    wolfSSL_write(ssl, buffer, ret);  // Echo back
    
    wolfSSL_free(ssl);
    close(clientfd);
    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return 0;
}
```

## 11.4 Non-Blocking I/O

```c
wolfSSL_set_using_nonblock(ssl, 1);

int ret = wolfSSL_connect(ssl);
if (ret != SSL_SUCCESS) {
    int err = wolfSSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        // Retry later
    }
}
```

## 11.5 Session Resumption

```c
// After successful connection
WOLFSSL_SESSION* session = wolfSSL_get_session(ssl);

// For new connection
WOLFSSL* ssl2 = wolfSSL_new(ctx);
wolfSSL_set_session(ssl2, session);
wolfSSL_connect(ssl2);
```

---

# Appendix A: SSL/TLS Overview

## A.1 SSL/TLS Protocol Versions

| Protocol | Status |
|----------|--------|
| SSL 2.0 | Deprecated, insecure |
| SSL 3.0 | Deprecated (POODLE vulnerability) |
| TLS 1.0 | Legacy, being phased out |
| TLS 1.1 | Legacy, being phased out |
| TLS 1.2 | Widely deployed, secure |
| TLS 1.3 | Current standard, recommended |
| DTLS 1.0 | Based on TLS 1.1 |
| DTLS 1.2 | Based on TLS 1.2 |
| DTLS 1.3 | Based on TLS 1.3 |

## A.2 TLS Handshake Overview

### TLS 1.2 Handshake

1. **ClientHello** - Client sends supported versions, cipher suites, random
2. **ServerHello** - Server selects version, cipher suite, sends random
3. **Certificate** - Server sends certificate chain
4. **ServerKeyExchange** - Server sends key exchange parameters (if needed)
5. **CertificateRequest** - Server requests client certificate (optional)
6. **ServerHelloDone** - Server signals end of hello messages
7. **Certificate** - Client sends certificate (if requested)
8. **ClientKeyExchange** - Client sends key exchange data
9. **CertificateVerify** - Client proves certificate ownership
10. **ChangeCipherSpec** - Client switches to encrypted mode
11. **Finished** - Client sends encrypted verification
12. **ChangeCipherSpec** - Server switches to encrypted mode
13. **Finished** - Server sends encrypted verification

### TLS 1.3 Handshake (Simplified)

1. **ClientHello** - Client sends supported versions, cipher suites, key shares
2. **ServerHello** - Server selects parameters, sends key share
3. **EncryptedExtensions** - Server extensions (encrypted)
4. **Certificate** - Server certificate (encrypted)
5. **CertificateVerify** - Server signature (encrypted)
6. **Finished** - Server verification (encrypted)
7. **Certificate** - Client certificate (encrypted, if requested)
8. **CertificateVerify** - Client signature (encrypted)
9. **Finished** - Client verification (encrypted)

TLS 1.3 achieves 1-RTT handshake (or 0-RTT with early data).

## A.3 Certificate Chain

```
Root CA (self-signed, trusted)
    └── Intermediate CA (signed by Root)
            └── Server/Client Certificate (signed by Intermediate)
```

---

# Appendix B: wolfSSL API Quick Reference

## B.1 Initialization and Cleanup

| Function | Description |
|----------|-------------|
| `wolfSSL_Init()` | Initialize wolfSSL library |
| `wolfSSL_Cleanup()` | Cleanup wolfSSL library |

## B.2 Context Functions

| Function | Description |
|----------|-------------|
| `wolfSSL_CTX_new(method)` | Create new context |
| `wolfSSL_CTX_free(ctx)` | Free context |
| `wolfSSL_CTX_load_verify_locations(ctx, file, path)` | Load CA certificates |
| `wolfSSL_CTX_use_certificate_file(ctx, file, type)` | Load certificate |
| `wolfSSL_CTX_use_PrivateKey_file(ctx, file, type)` | Load private key |
| `wolfSSL_CTX_use_certificate_chain_file(ctx, file)` | Load certificate chain |
| `wolfSSL_CTX_set_verify(ctx, mode, cb)` | Set verification mode |
| `wolfSSL_CTX_set_cipher_list(ctx, list)` | Set cipher suites |

## B.3 SSL Session Functions

| Function | Description |
|----------|-------------|
| `wolfSSL_new(ctx)` | Create new SSL object |
| `wolfSSL_free(ssl)` | Free SSL object |
| `wolfSSL_set_fd(ssl, fd)` | Associate socket |
| `wolfSSL_connect(ssl)` | Client handshake |
| `wolfSSL_accept(ssl)` | Server handshake |
| `wolfSSL_read(ssl, buf, len)` | Read data |
| `wolfSSL_write(ssl, buf, len)` | Write data |
| `wolfSSL_shutdown(ssl)` | Shutdown connection |
| `wolfSSL_get_error(ssl, ret)` | Get error code |

## B.4 Protocol Methods

**Client Methods:**
- `wolfTLSv1_2_client_method()`
- `wolfTLSv1_3_client_method()`
- `wolfSSLv23_client_method()` (flexible)
- `wolfDTLSv1_2_client_method()`

**Server Methods:**
- `wolfTLSv1_2_server_method()`
- `wolfTLSv1_3_server_method()`
- `wolfSSLv23_server_method()` (flexible)
- `wolfDTLSv1_2_server_method()`

## B.5 Certificate Functions

| Function | Description |
|----------|-------------|
| `wolfSSL_get_peer_certificate(ssl)` | Get peer certificate |
| `wolfSSL_X509_get_subject_name(x509)` | Get subject name |
| `wolfSSL_X509_get_issuer_name(x509)` | Get issuer name |
| `wolfSSL_X509_NAME_oneline(name, buf, len)` | Convert name to string |
| `wolfSSL_X509_free(x509)` | Free certificate |

---

# Appendix C: Error Codes

## C.1 Common SSL Errors

| Code | Name | Description |
|------|------|-------------|
| -155 | `DOMAIN_NAME_MISMATCH` | Domain name verification failed |
| -188 | `ASN_NO_SIGNER_E` | No CA signer for certificate |
| -155 | `DATE_E` | Certificate date invalid |
| -173 | `VERIFY_CERT_ERROR` | Certificate verification failed |
| -308 | `SOCKET_ERROR_E` | Socket error |
| -323 | `WANT_READ` | Need more data (non-blocking) |
| -324 | `WANT_WRITE` | Need to write (non-blocking) |

## C.2 wolfCrypt Errors

| Code | Name | Description |
|------|------|-------------|
| -140 | `BAD_FUNC_ARG` | Bad function argument |
| -141 | `MEMORY_E` | Memory allocation failed |
| -143 | `BUFFER_E` | Buffer too small |
| -170 | `RSA_BUFFER_E` | RSA buffer error |
| -212 | `ECC_BAD_ARG_E` | ECC bad argument |

---

# Appendix D: Build Options Reference

## D.1 Common Configure Options

| Option | Description |
|--------|-------------|
| `--enable-debug` | Enable debug mode |
| `--enable-opensslextra` | Enable OpenSSL compatibility |
| `--enable-opensslall` | Enable all OpenSSL compatibility |
| `--enable-sni` | Enable Server Name Indication |
| `--enable-alpn` | Enable ALPN |
| `--enable-dtls` | Enable DTLS |
| `--enable-tls13` | Enable TLS 1.3 |
| `--enable-ecc` | Enable ECC |
| `--enable-curve25519` | Enable Curve25519 |
| `--enable-ed25519` | Enable Ed25519 |
| `--enable-aesni` | Enable AES-NI |
| `--enable-intelasm` | Enable Intel assembly |
| `--enable-keygen` | Enable key generation |
| `--enable-certgen` | Enable certificate generation |
| `--enable-certreq` | Enable CSR generation |
| `--enable-pkcs7` | Enable PKCS#7 |
| `--enable-pkcs11` | Enable PKCS#11 |
| `--enable-staticmemory` | Enable static memory |
| `--enable-sniffer` | Enable SSL sniffer |

## D.2 Common Preprocessor Macros

| Macro | Description |
|-------|-------------|
| `DEBUG_WOLFSSL` | Enable debug output |
| `NO_FILESYSTEM` | Disable filesystem |
| `NO_INLINE` | Disable inline functions |
| `SINGLE_THREADED` | Single-threaded mode |
| `WOLFSSL_STATIC_MEMORY` | Static memory allocation |
| `HAVE_ECC` | Enable ECC |
| `HAVE_AESGCM` | Enable AES-GCM |
| `HAVE_CHACHA` | Enable ChaCha20 |
| `HAVE_POLY1305` | Enable Poly1305 |
| `WOLFSSL_TLS13` | Enable TLS 1.3 |
| `WOLFSSL_DTLS` | Enable DTLS |
| `OPENSSL_EXTRA` | OpenSSL compatibility |
| `WOLFSSL_SHA384` | Enable SHA-384 |
| `WOLFSSL_SHA512` | Enable SHA-512 |

---

# Appendix E: Platform-Specific Notes

## E.1 Embedded Systems

For embedded systems with limited resources:

```c
// user_settings.h
#define WOLFSSL_SMALL_STACK
#define SMALL_SESSION_CACHE
#define NO_FILESYSTEM
#define NO_WRITEV
#define NO_DEV_RANDOM
#define WOLFSSL_USER_IO
#define SINGLE_THREADED
```

## E.2 RTOS Integration

**FreeRTOS:**
```c
#define FREERTOS
#define FREERTOS_TCP
```

**ThreadX:**
```c
#define THREADX
#define NETX
```

**Zephyr:**
```c
#define WOLFSSL_ZEPHYR
```

## E.3 Hardware Crypto Acceleration

**STM32:**
```c
#define WOLFSSL_STM32_CUBEMX
#define STM32_CRYPTO
#define STM32_RNG
```

**ESP32:**
```c
#define WOLFSSL_ESPWROOM32
```

---

# Appendix F: Comparison with Other Libraries

| Feature | wolfSSL | OpenSSL | mbedTLS |
|---------|---------|---------|---------|
| Footprint | 20-100 KB | 500+ KB | 60-100 KB |
| TLS 1.3 | Yes | Yes | Yes |
| DTLS 1.3 | Yes | No | No |
| FIPS 140-2/3 | Yes | Yes | No |
| Hardware Accel | Extensive | Limited | Moderate |
| Commercial License | Yes | Apache 2.0 | Apache 2.0 |
| Embedded Focus | Primary | Secondary | Primary |

---

# Appendix G: Post-Quantum Cryptography

wolfSSL supports post-quantum cryptographic algorithms for protection against quantum computer attacks.

## G.1 Supported Algorithms

**Key Encapsulation:**
- ML-KEM (FIPS 203, formerly Kyber)

**Digital Signatures:**
- ML-DSA (FIPS 204, formerly Dilithium)

## G.2 Hybrid Mode

wolfSSL supports hybrid key exchange combining classical (ECDHE) and post-quantum (ML-KEM) algorithms for defense-in-depth.

## G.3 Enabling Post-Quantum Support

```bash
./configure --with-liboqs
```

Or use wolfSSL's native implementations with appropriate configure options.

---

# Appendix H: wolfSM (ShangMi) Support

wolfSSL supports Chinese National Cryptographic Standards (ShangMi):

| Algorithm | Description |
|-----------|-------------|
| SM2 | Elliptic curve cryptography |
| SM3 | Cryptographic hash function |
| SM4 | Block cipher |

## H.1 Enabling ShangMi

```bash
./configure --enable-sm2 --enable-sm3 --enable-sm4
```

## H.2 Usage Example

```c
#include <wolfssl/wolfcrypt/sm3.h>
#include <wolfssl/wolfcrypt/sm4.h>

// SM3 Hash
wc_Sm3 sm3;
wc_InitSm3(&sm3, NULL, INVALID_DEVID);
wc_Sm3Update(&sm3, data, dataLen);
wc_Sm3Final(&sm3, hash);

// SM4 Encryption
Sm4 sm4;
wc_Sm4SetKey(&sm4, key, SM4_KEY_SIZE);
wc_Sm4CbcEncrypt(&sm4, cipher, plain, plainLen);
```

---

# Appendix I: Resources and Support

## I.1 Documentation

- **Manual:** https://www.wolfssl.com/documentation/manuals/wolfssl/
- **API Reference:** https://www.wolfssl.com/documentation/manuals/wolfssl/
- **Examples:** https://github.com/wolfSSL/wolfssl-examples

## I.2 Support

- **Forums:** https://www.wolfssl.com/forums/
- **Email:** support@wolfssl.com
- **GitHub Issues:** https://github.com/wolfSSL/wolfssl/issues

## I.3 Licensing

wolfSSL is dual-licensed:
- **GPLv2** - Free for open source projects
- **Commercial** - Available for proprietary applications

---

*This documentation is derived from the wolfSSL Manual. For the most current and complete documentation, please visit https://www.wolfssl.com/documentation/*

