project(console_bidi)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FRIBIDI REQUIRED fribidi)

add_library(console_bidi STATIC ConsoleBidi.cpp)

target_include_directories(console_bidi PUBLIC ${FRIBIDI_INCLUDE_DIRS})
target_include_directories(console_bidi PRIVATE .)
target_include_directories(console_bidi PRIVATE ..)
target_include_directories(console_bidi PRIVATE ../..)
target_include_directories(console_bidi PRIVATE ../../..)
target_include_directories(console_bidi PRIVATE ../../../utils/include)

target_link_libraries(console_bidi PUBLIC ${FRIBIDI_LIBRARIES})
target_compile_options(console_bidi PUBLIC ${FRIBIDI_CFLAGS_OTHER})

find_package(Threads REQUIRED)
target_link_libraries(console_bidi PUBLIC utils)
