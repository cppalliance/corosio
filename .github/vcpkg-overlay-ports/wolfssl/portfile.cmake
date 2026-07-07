# Overlay port: identical to the upstream vcpkg `wolfssl` port EXCEPT that
# WOLFSSL_OPENSSLEXTRA is disabled. The upstream port hardcodes
# -DWOLFSSL_OPENSSLEXTRA=yes, which (via wolfssl/wolfcrypt/settings.h) also
# defines WOLFSSL_ALWAYS_VERIFY_CB and WOLFSSL_VERIFY_CB_ALL_CERTS. That
# makes WolfSSL invoke a verify callback on success as well as failure.
#
# This overlay produces a *minimal* WolfSSL (OPENSSL_EXTRA off), matching a
# stock distribution build, so CI can exercise corosio's fail-closed path
# for set_verify_callback (which returns std::errc::function_not_supported
# when the callback cannot be honored on such a build). Used only by the
# dedicated "wolfssl-minimal" CI lane via VCPKG_OVERLAY_PORTS; all other
# lanes use the upstream (capable) port.
#
# Keep this in sync with the pinned upstream port
# (microsoft/vcpkg ports/wolfssl) apart from the single OPENSSLEXTRA line.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO wolfssl/wolfssl
    REF "v${VERSION}-stable"
    SHA512 29f52644966f21908e0d3f795c62b0f5af9cd2d766db20c6ed5c588611f19f048119827fe6e787ccc3ce676d8c97cf7ab409d996df0e3acb812d6cd01364de61
    HEAD_REF master
    PATCHES
    )

if ("asio" IN_LIST FEATURES)
    set(ENABLE_ASIO yes)
else()
    set(ENABLE_ASIO no)
endif()

if ("dtls" IN_LIST FEATURES)
    set(ENABLE_DTLS yes)
else()
    set(ENABLE_DTLS no)
endif()

if ("quic" IN_LIST FEATURES)
    set(ENABLE_QUIC yes)
else()
    set(ENABLE_QUIC no)
endif()

vcpkg_cmake_get_vars(cmake_vars_file)
include("${cmake_vars_file}")

foreach(config RELEASE DEBUG)
  string(APPEND VCPKG_COMBINED_C_FLAGS_${config} " -DHAVE_EX_DATA -DNO_WOLFSSL_STUB -DWOLFSSL_ALT_CERT_CHAINS -DWOLFSSL_DES_ECB -DWOLFSSL_CUSTOM_OID -DHAVE_OID_ENCODING -DWOLFSSL_CERT_GEN -DWOLFSSL_ASN_TEMPLATE -DWOLFSSL_KEY_GEN -DHAVE_PKCS7 -DHAVE_AES_KEYWRAP -DWOLFSSL_AES_DIRECT -DHAVE_X963_KDF")
  if ("secret-callback" IN_LIST FEATURES)
      string(APPEND VCPKG_COMBINED_C_FLAGS_${config} " -DHAVE_SECRET_CALLBACK")
  endif()
endforeach()

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
      -DWOLFSSL_BUILD_OUT_OF_TREE=yes
      -DWOLFSSL_EXAMPLES=no
      -DWOLFSSL_CRYPT_TESTS=no
      -DWOLFSSL_OPENSSLEXTRA=no
      -DWOLFSSL_TPM=yes
      -DWOLFSSL_TLSX=yes
      -DWOLFSSL_OCSP=yes
      -DWOLFSSL_OCSPSTAPLING=yes
      -DWOLFSSL_OCSPSTAPLING_V2=yes
      -DWOLFSSL_CRL=yes
      -DWOLFSSL_DES3=yes
      -DWOLFSSL_HPKE=yes
      -DWOLFSSL_SNI=yes
      -DWOLFSSL_ASIO=${ENABLE_ASIO}
      -DWOLFSSL_DTLS=${ENABLE_DTLS}
      -DWOLFSSL_DTLS13=${ENABLE_DTLS}
      -DWOLFSSL_DTLS_CID=${ENABLE_DTLS}
      -DWOLFSSL_QUIC=${ENABLE_QUIC}
      -DWOLFSSL_SESSION_TICKET=${ENABLE_QUIC}
    OPTIONS_RELEASE
      -DCMAKE_C_FLAGS=${VCPKG_COMBINED_C_FLAGS_RELEASE}
    OPTIONS_DEBUG
      -DCMAKE_C_FLAGS=${VCPKG_COMBINED_C_FLAGS_DEBUG}
      -DWOLFSSL_DEBUG=yes)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/wolfssl)

if(VCPKG_TARGET_IS_IOS OR VCPKG_TARGET_IS_OSX)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/wolfssl.pc" "Libs.private: " "Libs.private: -framework CoreFoundation -framework Security ")
    if(NOT VCPKG_BUILD_TYPE)
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/wolfssl.pc" "Libs.private: " "Libs.private: -framework CoreFoundation -framework Security ")
    endif()
endif()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
