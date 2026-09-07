cmake_minimum_required(VERSION 3.13)
project(gstreamer_multithread_pipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED
  gstreamer-1.0>=1.16
  gstreamer-app-1.0
  gstreamer-rtsp-server-1.0
  glib-2.0
  gobject-2.0
)

add_executable(gstpipe
  src/pipeline.cpp
  src/rtsp_server.cpp
  src/main.cpp
)
target_include_directories(gstpipe PRIVATE include ${GST_INCLUDE_DIRS})
target_compile_options(gstpipe PRIVATE ${GST_CFLAGS_OTHER})
target_link_libraries(gstpipe PRIVATE ${GST_LIBRARIES} pthread)
target_link_directories(gstpipe PRIVATE ${GST_LIBRARY_DIRS})

install(TARGETS gstpipe RUNTIME DESTINATION bin)
