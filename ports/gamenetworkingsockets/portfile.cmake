# Upstream port, pointed at our GNS fork until the ICE fixes are merged upstream.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO tintinhamans/GNS
    REF "931109d6fa750ba1a84f8b843d7d804e8f761953" # ice-interface-ranking-ipv6
    SHA512 ea6d5f56f5a4bb3a4d183798991a1635d89691ff20b23b5408260fc1c0f6ee47e7d73d0878e3a30d253312ba1a48d8d3453891ef0a788ce57b9688bc57ca90aa
    HEAD_REF ice-interface-ranking-ipv6
)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        ice             ENABLE_ICE
        webrtc          USE_STEAMWEBRTC
)

if("webrtc" IN_LIST FEATURES)
    # WebRTC's own source is the pinned googlesource snapshot GNS builds
    # "webrtc-lite" from (not a full gclient checkout), and isn't fetchable as
    # a normal vcpkg dependency, so pull it straight into GNS's expected
    # submodule layout.
    vcpkg_from_git(
        OUT_SOURCE_PATH WEBRTC_SOURCE_PATH
        URL https://webrtc.googlesource.com/src
        REF 30a3e787948dd6cdd541773101d664b85eb332a6
    )
    file(REMOVE_RECURSE "${SOURCE_PATH}/src/external/webrtc")
    file(COPY "${WEBRTC_SOURCE_PATH}/." DESTINATION "${SOURCE_PATH}/src/external/webrtc")

    # GNS's steamwebrtc wrapper otherwise vendors its own pinned Abseil
    # submodule for this, but this port already depends on protobuf, which
    # pulls in vcpkg's own "abseil" -- vendoring a second copy installs
    # identically-named files (absl_strings.lib etc.) that collide with it.
    # Use the one already in the tree instead of building a duplicate.
    set(_steamwebrtc_cmakelists "${SOURCE_PATH}/src/external/steamwebrtc/CMakeLists.txt")
    file(READ "${_steamwebrtc_cmakelists}" _steamwebrtc_cmakelists_contents)
    string(REPLACE
        "#find_package(absl QUIET)\nset(absl_FOUND OFF)"
        "find_package(absl CONFIG REQUIRED)\nset(absl_FOUND ON)"
        _steamwebrtc_cmakelists_contents
        "${_steamwebrtc_cmakelists_contents}")
    file(WRITE "${_steamwebrtc_cmakelists}" "${_steamwebrtc_cmakelists_contents}")
endif()

# Select static vs dynamic based on the triplet.
if("${VCPKG_LIBRARY_LINKAGE}" STREQUAL "dynamic")
    set(BUILD_SHARED_LIB ON)
    set(BUILD_STATIC_LIB OFF)
else()
    set(BUILD_SHARED_LIB OFF)
    set(BUILD_STATIC_LIB ON)
endif()

# Link the MSVC CRT statically when the CRT linkage is static.
# Not used on non-MSVC platforms; listed in MAYBE_UNUSED_VARIABLES accordingly.
if("${VCPKG_CRT_LINKAGE}" STREQUAL "static")
    set(MSVC_CRT_STATIC ON)
else()
    set(MSVC_CRT_STATIC OFF)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DUSE_CRYPTO=OpenSSL
        -DBUILD_STATIC_LIB=${BUILD_STATIC_LIB}
        -DBUILD_SHARED_LIB=${BUILD_SHARED_LIB}
        -DMSVC_CRT_STATIC=${MSVC_CRT_STATIC}
        -DBUILD_TESTS=OFF
        -DBUILD_EXAMPLES=OFF
        -DBUILD_TOOLS=OFF
        ${FEATURE_OPTIONS}
    MAYBE_UNUSED_VARIABLES
        MSVC_CRT_STATIC
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/GameNetworkingSockets")
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_copy_pdbs()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
