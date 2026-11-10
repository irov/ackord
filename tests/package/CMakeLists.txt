cmake_minimum_required(VERSION 3.24)

project(ackord_package_test LANGUAGES C)

enable_testing()
find_package(ackord 0.1 CONFIG REQUIRED)

add_executable(ackord_package_c ../../examples/counter.c)
target_link_libraries(ackord_package_c PRIVATE ackord::ackord)
add_test(NAME ackord_package_c COMMAND ackord_package_c)
