cmake_minimum_required(VERSION 3.10)
project(chat_server C)

# The server uses POSIX sockets (sys/socket.h, unistd.h, select()).
# On Windows, build inside WSL or Docker instead of with MSVC.
if(NOT UNIX)
  message(FATAL_ERROR
    "chat_server is POSIX-only. On Windows, build it in WSL or Docker (see README.md).")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(chat_server CHAT_server.c)
target_compile_options(chat_server PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wvla)
