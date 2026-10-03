cmake_minimum_required(VERSION 3.13)
project(chat_server C)

# The server uses POSIX sockets and epoll/poll.
# On Windows, build inside WSL or Docker instead of with MSVC.
if(NOT UNIX)
  message(FATAL_ERROR
    "chat_server is POSIX-only. On Windows, build it in WSL or Docker (see README.md).")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# ---- options ----------------------------------------------------------------
set(CHAT_POLLER "auto" CACHE STRING "Event backend: auto, epoll or poll")
set_property(CACHE CHAT_POLLER PROPERTY STRINGS auto epoll poll)
option(CHAT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(CHAT_POLLER STREQUAL "auto")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CHAT_POLLER_IMPL epoll)
  else()
    set(CHAT_POLLER_IMPL poll)
  endif()
else()
  set(CHAT_POLLER_IMPL ${CHAT_POLLER})
endif()
if(NOT CHAT_POLLER_IMPL MATCHES "^(epoll|poll)$")
  message(FATAL_ERROR "CHAT_POLLER must be auto, epoll or poll")
endif()
message(STATUS "chat_server event backend: ${CHAT_POLLER_IMPL}")

if(CHAT_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---- library + executable ---------------------------------------------------
add_library(chat_core STATIC
  src/buffer.c
  src/client.c
  src/log.c
  src/net.c
  src/protocol.c
  src/server.c
  src/poller_${CHAT_POLLER_IMPL}.c
)
target_include_directories(chat_core PUBLIC src)
target_compile_definitions(chat_core PUBLIC _POSIX_C_SOURCE=200809L)
target_compile_options(chat_core PUBLIC -Wall -Wextra -Wpedantic -Wshadow -Wvla)

add_executable(chat_server src/main.c)
target_link_libraries(chat_server PRIVATE chat_core)

# ---- tests (ctest) ----------------------------------------------------------
enable_testing()

foreach(t buffer protocol)
  add_executable(test_${t} tests/test_${t}.c)
  target_link_libraries(test_${t} PRIVATE chat_core)
  add_test(NAME unit_${t} COMMAND test_${t})
endforeach()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME integration
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_server.py
                   $<TARGET_FILE:chat_server>)
  set_tests_properties(integration PROPERTIES TIMEOUT 180)
endif()
