# The same release as vcpkg's zstd port; only the decoder sources are installed.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO facebook/zstd
    REF "v${VERSION}"
    SHA512 26e441267305f6e58080460f96ab98645219a90d290a533410b1b0b1d2f870721c95f8384e342ee647c5e968385a5b7e30c2d04340c37f59b3e6d86762c3260c
    HEAD_REF dev
)
file(INSTALL "${SOURCE_PATH}/lib/zstd.h" "${SOURCE_PATH}/lib/zstd_errors.h" "${SOURCE_PATH}/lib/common"
     "${SOURCE_PATH}/lib/decompress" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE" "${SOURCE_PATH}/COPYING")
