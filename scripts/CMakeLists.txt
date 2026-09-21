cmake_minimum_required(VERSION 3.15)
project(mila_controller CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(mila_controller WIN32 mila_controller.cpp)
target_link_libraries(mila_controller PRIVATE winhttp gdi32 user32 shell32 ws2_32)

if(MINGW)
  target_link_options(mila_controller PRIVATE -static)
endif()
