cmake_minimum_required(VERSION 3.7)
project(mtallocator)

set(CMAKE_CXX_STANDARD 11)

set(SOURCE_FILES mtallocator.cpp)
add_executable(mtallocator ${SOURCE_FILES})
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wextra")