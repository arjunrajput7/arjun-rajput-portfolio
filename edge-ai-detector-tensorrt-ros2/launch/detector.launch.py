# SPDX-License-Identifier: MIT
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    args = [
        DeclareLaunchArgument("engine_path", default_value="model_int8.engine"),
        DeclareLaunchArgument("camera_device", default_value="/dev/video0"),
        DeclareLaunchArgument("capture_width", default_value="1280"),
        DeclareLaunchArgument("capture_height", default_value="720"),
        DeclareLaunchArgument("capture_fps", default_value="30"),
        DeclareLaunchArgument("camera_mjpeg", default_value="true"),
        DeclareLaunchArgument("jetson_csi", default_value="false"),
        DeclareLaunchArgument("input_width", default_value="640"),
        DeclareLaunchArgument("input_height", default_value="640"),
        DeclareLaunchArgument("num_classes", default_value="80"),
        DeclareLaunchArgument("conf_threshold", default_value="0.25"),
        DeclareLaunchArgument("iou_threshold", default_value="0.45"),
        DeclareLaunchArgument("output_transposed", default_value="true"),
        DeclareLaunchArgument("publish_socketcan", default_value="false"),
        DeclareLaunchArgument("socketcan_iface", default_value="can0"),
        DeclareLaunchArgument("benchmark_csv", default_value=""),
    ]

    node = Node(
        package="edge_ai_detector",
        executable="detector_node",
        name="edge_ai_detector",
        output="screen",
        parameters=[{
            k: LaunchConfiguration(k) for k in [
                "engine_path", "camera_device", "capture_width", "capture_height",
                "capture_fps", "camera_mjpeg", "jetson_csi", "input_width", "input_height",
                "num_classes", "conf_threshold", "iou_threshold", "output_transposed",
                "publish_socketcan", "socketcan_iface", "benchmark_csv",
            ]
        }],
    )

    return LaunchDescription(args + [node])
