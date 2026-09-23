cmake_minimum_required(VERSION 3.25)
project(cyaneserver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "CyaneServer requires Clang, got ${CMAKE_CXX_COMPILER_ID}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerFlags)
include(Dependencies)

option(CYANE_BUILD_TESTS "Build unit tests" ON)
option(CYANE_LTO "Enable thin LTO" ON)
option(CYANE_SANITIZE "Enable ASan+UBSan" OFF)
option(CYANE_WERROR "Treat warnings as errors" OFF)

add_library(cyane_flags INTERFACE)
target_compile_options(cyane_flags INTERFACE ${CYANE_WARNINGS} ${CYANE_OPT_FLAGS})
target_compile_definitions(cyane_flags INTERFACE ${CYANE_DEFINES})

if(CYANE_LTO)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(cyane_flags INTERFACE -flto)
    target_link_options(cyane_flags INTERFACE -flto)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(cyane_flags INTERFACE -flto=auto)
    target_link_options(cyane_flags INTERFACE -flto=auto)
  endif()
endif()

if(CYANE_SANITIZE)
  target_compile_options(cyane_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(cyane_flags INTERFACE -fsanitize=address,undefined)
endif()

# 代码生成：data/registry.toml -> generated/cyane/generated/registry_meta.hpp
set(CYANE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(CYANE_REGISTRY_HEADER "${CYANE_GENERATED_DIR}/cyane/generated/registry_meta.hpp")

add_executable(gen_registry tools/gen_registry/main.cpp)
target_link_libraries(gen_registry PRIVATE cyane::core)
target_include_directories(gen_registry PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_custom_command(
  OUTPUT "${CYANE_REGISTRY_HEADER}"
  COMMAND gen_registry
          --data "${CMAKE_CURRENT_SOURCE_DIR}/data/registry.toml"
          --out "${CYANE_REGISTRY_HEADER}"
  DEPENDS gen_registry "${CMAKE_CURRENT_SOURCE_DIR}/data/registry.toml"
  COMMENT "Generating registry metadata"
  VERBATIM
)

add_library(cyane_core
  src/core/log.cpp
  src/core/config.cpp
  src/core/thread_pool.cpp
)
target_include_directories(cyane_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(cyane_core PUBLIC cyane_flags cyane::deps)
add_library(cyane::core ALIAS cyane_core)

add_library(cyane_crypto
  src/crypto/cipher.cpp
  src/crypto/digest.cpp
  src/crypto/rsa.cpp
)
target_include_directories(cyane_crypto PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(cyane_crypto PUBLIC cyane::core cyane::deps)
add_library(cyane::crypto ALIAS cyane_crypto)

add_library(cyane_proto
  src/proto/frame.cpp
)
target_include_directories(cyane_proto PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(cyane_proto PUBLIC cyane::core)
add_library(cyane::proto ALIAS cyane_proto)

add_library(cyane_net
  src/net/connection.cpp
  src/net/net_service.cpp
  src/net/session_service.cpp
)
target_include_directories(cyane_net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(cyane_net PUBLIC cyane::crypto cyane::proto)
add_library(cyane::net ALIAS cyane_net)

add_library(cyane_game
  src/game/server.cpp
  src/game/status.cpp
  src/world/region.cpp
  src/world/world.cpp
  src/entity/entity.cpp
  "${CYANE_REGISTRY_HEADER}"
)
target_include_directories(cyane_game PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CYANE_GENERATED_DIR}"
)
target_link_libraries(cyane_game PUBLIC cyane::net)
add_library(cyane::game ALIAS cyane_game)

add_executable(cyane src/main.cpp)
target_link_libraries(cyane PRIVATE cyane::game)
set_target_properties(cyane PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_custom_target(cyane_codegen DEPENDS "${CYANE_REGISTRY_HEADER}")

if(CYANE_BUILD_TESTS)
  enable_testing()
  add_executable(cyane_tests
    tests/test_main.cpp
    tests/test_mpmc_queue.cpp
    tests/test_inline_function.cpp
    tests/test_config.cpp
    tests/test_log.cpp
    tests/test_time.cpp
    tests/test_crypto.cpp
    tests/test_crypto.cpp
    tests/test_generated.cpp
  tests/test_server.cpp
  tests/test_bytes.cpp
    tests/test_frame.cpp
    tests/test_json.cpp
    tests/test_status.cpp
    tests/test_status_ping.cpp
  )
  target_include_directories(cyane_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(cyane_tests PRIVATE cyane::game)
  add_test(NAME cyane_tests COMMAND cyane_tests)
endif()

message(STATUS "")
message(STATUS "CyaneServer ${CMAKE_BUILD_TYPE} | clang ${CMAKE_CXX_COMPILER_VERSION} | C++23")
message(STATUS "  LTO: ${CYANE_LTO}  sanitize: ${CYANE_SANITIZE}  tests: ${CYANE_BUILD_TESTS}")
message(STATUS "  deps: ${CYANE_ZLIB_IMPL} | openssl=${CYANE_HAVE_OPENSSL} liburing=${CYANE_HAVE_LIBURING}")
message(STATUS "")
