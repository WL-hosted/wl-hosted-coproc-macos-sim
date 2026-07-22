cmake_minimum_required(VERSION 3.20)
project(wlh_coproc_macos_sim VERSION 0.1.0 LANGUAGES C)
include(CTest)
find_package(Threads REQUIRED)

set(WLH_SIM_BUILD_TESTING "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
add_subdirectory(core/coproc-core core-coproc-build EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${WLH_SIM_BUILD_TESTING}")
wlh_common_enable_posix_osal(BUILD_TESTING "${WLH_SIM_BUILD_TESTING}")
target_compile_definitions(wlh_coproc_core PUBLIC WLH_ENABLE_TEST_HOOKS)

add_library(wlh_sim_ipc STATIC src/ipc.c)
target_include_directories(wlh_sim_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(wlh_sim_ipc PUBLIC c_std_99)
set_target_properties(wlh_sim_ipc PROPERTIES C_EXTENSIONS OFF)

add_executable(wlh-coproc-macos-sim src/main.c)
target_link_libraries(wlh-coproc-macos-sim PRIVATE
  wlh_coproc_core wlh_sim_nanopb wlh_sim_ipc wlh::posix_osal Threads::Threads)
target_compile_features(wlh-coproc-macos-sim PRIVATE c_std_11)

if(APPLE)
  enable_language(Swift)
  target_sources(wlh-coproc-macos-sim PRIVATE
    src/wifi_backend_real.c src/wifi_backend_real.swift)
  target_compile_definitions(wlh-coproc-macos-sim PRIVATE WLH_SIM_HAVE_REAL_BACKEND)
  # main() lives in C; do not let the Swift object emit its own _main.
  target_compile_options(wlh-coproc-macos-sim PRIVATE
    $<$<COMPILE_LANGUAGE:Swift>:-parse-as-library>)
  target_link_libraries(wlh-coproc-macos-sim PRIVATE
    "-framework Foundation" "-framework CoreWLAN")
  # Keep sanitizer builds working through the swiftc link driver: mirror the
  # C -fsanitize flags as Swift -sanitize flags (compile and link).
  foreach(sanitizer address thread undefined)
    if(CMAKE_C_FLAGS MATCHES "-fsanitize=[^ ]*${sanitizer}")
      target_compile_options(wlh-coproc-macos-sim PRIVATE
        $<$<COMPILE_LANGUAGE:Swift>:-sanitize=${sanitizer}>)
      target_link_options(wlh-coproc-macos-sim PRIVATE
        $<$<LINK_LANGUAGE:Swift>:-sanitize=${sanitizer}>)
    endif()
  endforeach()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(wlh_sim_ipc PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(wlh-coproc-macos-sim PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic -Werror>)
endif()

if(BUILD_TESTING)
  add_executable(wlh_sim_ipc_tests tests/test_ipc.c)
  target_link_libraries(wlh_sim_ipc_tests PRIVATE wlh_sim_ipc Threads::Threads)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(wlh_sim_ipc_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  elseif(MSVC)
    target_compile_options(wlh_sim_ipc_tests PRIVATE /UNDEBUG)
  endif()
  add_test(NAME wlh_sim_ipc COMMAND wlh_sim_ipc_tests)

endif()
