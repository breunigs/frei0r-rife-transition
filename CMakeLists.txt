cmake_minimum_required(VERSION 3.15)
project(rife_transition)

# we don't need it, but maybe we don't have to build it ourselves
set(USE_SYSTEM_WEBP ON)
include(CMakeLists.txt)

add_library (rife_transition MODULE rife_transition.cpp rife.cpp warp.cpp)
target_link_libraries(rife_transition ${RIFE_LINK_LIBRARIES})
