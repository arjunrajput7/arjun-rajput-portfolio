cmake_minimum_required(VERSION 3.16)
project(edge_ai_detector LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(vision_msgs REQUIRED)
find_package(OpenCV REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0 glib-2.0 gobject-2.0)

# ---- CUDA + TensorRT (no CMake package on most Jetson images) -------------
find_path(CUDA_INCLUDE_DIR cuda_runtime_api.h
  HINTS /usr/local/cuda/include /usr/local/cuda/targets/aarch64-linux/include)
find_library(CUDART_LIB cudart
  HINTS /usr/local/cuda/lib64 /usr/local/cuda/targets/aarch64-linux/lib)

find_path(TRT_INCLUDE_DIR NvInfer.h
  HINTS /usr/include /usr/include/aarch64-linux-gnu /usr/include/x86_64-linux-gnu)
find_library(NVINFER_LIB nvinfer     HINTS /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu)
find_library(NVONNXPARSER_LIB nvonnxparser
  HINTS /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu)

if(NOT CUDA_INCLUDE_DIR OR NOT TRT_INCLUDE_DIR OR NOT NVINFER_LIB)
  message(FATAL_ERROR "CUDA / TensorRT not found. Set CUDA_INCLUDE_DIR / TRT_INCLUDE_DIR.")
endif()

set(EDGE_AI_INCLUDES
  include ${CUDA_INCLUDE_DIR} ${TRT_INCLUDE_DIR} ${GST_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})

# ---- core library -------------------------------------------------------
add_library(edge_ai_core
  src/trt_engine.cpp
  src/yolo_postprocess.cpp
  src/gst_source.cpp
  src/socketcan_publisher.cpp
)
target_include_directories(edge_ai_core PUBLIC ${EDGE_AI_INCLUDES})
target_link_libraries(edge_ai_core
  ${NVINFER_LIB} ${CUDART_LIB} ${GST_LIBRARIES})
target_link_directories(edge_ai_core PUBLIC ${GST_LIBRARY_DIRS})

# ---- ROS 2 node -------------------------------------------------------
add_executable(detector_node src/detector_node.cpp)
target_include_directories(detector_node PRIVATE ${EDGE_AI_INCLUDES})
target_link_libraries(detector_node edge_ai_core)
ament_target_dependencies(detector_node rclcpp std_msgs sensor_msgs vision_msgs)

# vision_msgs API varies by distro. Humble+: ObjectHypothesisWithPose has a
# nested `hypothesis` (class_id string). Jazzy/Rolling also moved BoundingBox2D
# `center` to geometry_msgs/Pose2D (.position.x). Toggle these for your distro.
target_compile_definitions(detector_node PRIVATE EDGE_AI_VISION_MSGS_HAS_HYPOTHESIS)
# target_compile_definitions(detector_node PRIVATE EDGE_AI_VISION_MSGS_HAS_POSE_CENTER)

# ---- engine builder (offline tool, needs OpenCV + onnxparser) --------
add_executable(build_engine tools/build_engine.cpp src/int8_calibrator.cpp)
target_include_directories(build_engine PRIVATE ${EDGE_AI_INCLUDES})
target_link_libraries(build_engine
  edge_ai_core ${NVONNXPARSER_LIB} ${OpenCV_LIBS})

install(TARGETS detector_node build_engine DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY launch DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
