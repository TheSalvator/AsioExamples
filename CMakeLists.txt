cmake_minimum_required(VERSION 3.15)

project(AsioExamples LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${PROJECT_SOURCE_DIR}/cmake/conan.cmake)

conan_cmake_install(
    PATH_OR_REFERENCE ${PROJECT_SOURCE_DIR}
    BUILD missing
    INSTALL_FOLDER ${PROJECT_BINARY_DIR}
)

list(PREPEND CMAKE_MODULE_PATH ${PROJECT_BINARY_DIR})

find_package(asio REQUIRED)
find_package(liburing REQUIRED)
find_package(expected-lite REQUIRED)
find_package(glog REQUIRED)
find_package(yaclib REQUIRED)

add_compile_options(-Wall -Werror -Wextra -Wpedantic)

add_library(AsioWithUring INTERFACE)
target_compile_definitions(AsioWithUring INTERFACE ASIO_HAS_IO_URING ASIO_DISABLE_EPOLL)
target_link_libraries(AsioWithUring INTERFACE asio::asio liburing::liburing)

add_executable(UseYaclibFuture UseYaclibFuture.cpp)
target_link_libraries(UseYaclibFuture PRIVATE AsioWithUring yaclib::yaclib)
