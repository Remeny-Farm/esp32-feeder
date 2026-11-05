cmake_minimum_required(VERSION 3.16)

# Reported in the state message as fw_version.
set(PROJECT_VER "0.1.0")

# components/ (safety_gate, motor, cue, feeder_util) is picked up automatically.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(feeder)
