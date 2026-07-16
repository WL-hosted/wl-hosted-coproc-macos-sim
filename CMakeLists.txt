cmake_minimum_required(VERSION 3.20)
project(wlh_coproc_macos_sim VERSION 0.1.0 LANGUAGES C)
include(CTest)
find_package(Threads REQUIRED)

set(WLH_SIM_BUILD_TESTING "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
add_subdirectory(coproc-core coproc-core-build EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${WLH_SIM_BUILD_TESTING}")
target_compile_definitions(wlh_coproc_core PUBLIC WLH_ENABLE_TEST_HOOKS)

add_library(wlh_sim_ipc STATIC src/ipc.c)
target_include_directories(wlh_sim_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(wlh_sim_ipc PUBLIC c_std_99)
set_target_properties(wlh_sim_ipc PROPERTIES C_EXTENSIONS OFF)

add_executable(wlh-coproc-macos-sim src/main.c)
target_link_libraries(wlh-coproc-macos-sim PRIVATE
  wlh_coproc_core wlh_sim_nanopb wlh_sim_ipc Threads::Threads)
target_compile_features(wlh-coproc-macos-sim PRIVATE c_std_99)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(wlh_sim_ipc PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(wlh-coproc-macos-sim PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING)
  add_executable(wlh_sim_ipc_tests tests/test_ipc.c)
  target_link_libraries(wlh_sim_ipc_tests PRIVATE wlh_sim_ipc Threads::Threads)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(wlh_sim_ipc_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME wlh_sim_ipc COMMAND wlh_sim_ipc_tests)
endif()
