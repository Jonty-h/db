cmake_minimum_required(VERSION 3.17)
project(db)

set(CMAKE_CXX_STANDARD 98)

add_executable(db main.c clog.c)