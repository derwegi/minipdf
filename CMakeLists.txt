cmake_minimum_required(VERSION 3.16)
project(minipdf LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets PrintSupport)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER REQUIRED IMPORTED_TARGET poppler-qt6)

add_executable(minipdf main.cpp)
target_link_libraries(minipdf PRIVATE Qt6::Widgets Qt6::PrintSupport PkgConfig::POPPLER)

install(TARGETS minipdf RUNTIME DESTINATION bin)
