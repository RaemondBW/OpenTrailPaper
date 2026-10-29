cmake_minimum_required(VERSION 3.22.1)
project("h3shim" C)

# The vendored H3 sources are the SAME files the iOS app compiles
# (companion-ios/Sources/H3). Building from the identical C — rather than
# reimplementing the indexing in Kotlin — is what guarantees a hexagon gets the
# same id on both phones, which is the whole basis of the tile dedup with the
# device.
file(GLOB H3_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/h3/lib/*.c")

add_library(h3shim SHARED
    ${H3_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/h3shim.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/h3jni.c"
)

target_include_directories(h3shim PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/h3/include"
)

target_compile_options(h3shim PRIVATE -w)

find_library(log-lib log)
target_link_libraries(h3shim ${log-lib} m)

# Play requires 16 KB page-size support (Android 15+ devices): NDK 26 still
# links with 4 KB LOAD alignment unless told otherwise. NDK 28 makes this the
# default; drop the flag when ndkVersion moves past 27.
target_link_options(h3shim PRIVATE "-Wl,-z,max-page-size=16384")
