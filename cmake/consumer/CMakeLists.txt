# A project that found this the way a user would.
#
# It is not part of the build. CI installs zu-cpp into a prefix, points
# this at it, and builds it, so the claim that find_package(zu-cpp)
# works is checked by doing it rather than by asserting it in a README.
cmake_minimum_required(VERSION 3.20)
project(zu-cpp-consumer LANGUAGES CXX)

find_package(zu-cpp REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE zu::cpp)
