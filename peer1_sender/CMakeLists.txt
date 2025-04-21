cmake_minimum_required(VERSION 3.20)
project(webrtc_gstreamer_demo)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
find_package(Boost REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0)

find_package(LibDataChannel REQUIRED)
if(NOT LibDataChannel_FOUND)
    message(FATAL_ERROR "Could not find LibDataChannel. Please check installation.")
endif()

include_directories(third_party/websocketpp)
include_directories(third_party/json/single_include)


add_executable(peer1 main.cpp)

target_include_directories(peer1 PRIVATE
    ${GST_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(peer1
    ${GST_LIBRARIES}
    LibDataChannel::LibDataChannel
    ${Boost_LIBRARIES}
    pthread
)

target_compile_options(peer1 PRIVATE ${GST_CFLAGS_OTHER})