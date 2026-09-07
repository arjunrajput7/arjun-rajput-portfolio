cmake_minimum_required(VERSION 3.13)
project(zero_copy_v4l2_capture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(zcv4l2
  src/v4l2_capture.cpp
  src/main.cpp
)
target_include_directories(zcv4l2 PRIVATE include)

# V4L2 lives in the kernel UAPI headers -- no external library to link.
include(CheckIncludeFileCXX)
check_include_file_cxx("linux/videodev2.h" HAVE_VIDEODEV2)
if(NOT HAVE_VIDEODEV2)
  message(FATAL_ERROR "linux/videodev2.h not found -- this project is Linux-only")
endif()

install(TARGETS zcv4l2 RUNTIME DESTINATION bin)
