# CMake toolchain configuration for aarch64-apple-ios
# Copyright Kyle Hayes (2026)
# Distributed under the Boost Software License, Version 1.0.
# See LICENSE file for details

set(ASM_FILES
    src/asm/arm64/make_context_arm64_aapcs_macho_gas.S
    src/asm/arm64/switch_arm64_aapcs_macho_gas.S
)
enable_language(ASM)
add_compile_options(-Wall -Wextra -Werror -g -O2)

set(CMAKE_CROSSCOMPILING_EMULATOR "/usr/bin/xcrun" "simctl" "spawn" "booted")

# Set Xcode attributes for iOS build (Bundle ID and Code Signing)
set(CMAKE_XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.libplctag.\${PRODUCT_NAME}")
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")