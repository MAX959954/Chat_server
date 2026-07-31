cmake_minimum_required(VERSION 3.10)
project(chat_server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(chat_server CHAT_server.c)
target_compile_options(chat_server PRIVATE -Wall -Wextra)