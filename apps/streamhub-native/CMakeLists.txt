cmake_minimum_required(VERSION 3.16)
project(streamhub_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Widgets Network Svg)

qt_add_executable(streamhub-native main.cpp)

target_link_libraries(streamhub-native PRIVATE
    Qt6::Quick Qt6::QuickControls2 Qt6::Widgets Qt6::Network Qt6::Svg)

install(TARGETS streamhub-native RUNTIME DESTINATION bin)
install(DIRECTORY qml DESTINATION .)
install(FILES icon.svg DESTINATION .)
