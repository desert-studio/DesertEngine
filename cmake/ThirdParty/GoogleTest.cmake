# GoogleTest: googletest v1.17.0, vendored, one amalgamated TU. Included on Windows
# only (macOS links Homebrew's gtest).
set(_root "${DESERT_ROOT}/ThirdParty/google-test")
if(NOT EXISTS "${_root}/src/gtest-all.cc")
    message(FATAL_ERROR "ThirdParty/google-test/src is missing. The googletest v1.17.0 sources are vendored; restore them from git.")
endif()
desert_add_library(GoogleTest FOLDER ThirdParty SOURCES "${_root}/src/gtest-all.cc")
set_target_properties(GoogleTest PROPERTIES CXX_STANDARD 17)
# gtest-all.cc includes "src/gtest-*.cc" relative to the gtest root.
target_include_directories(GoogleTest PRIVATE "${_root}/include" "${_root}")
