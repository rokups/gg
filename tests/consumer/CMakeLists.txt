cmake_minimum_required(VERSION 3.24)
project(gg_consumer LANGUAGES C)

find_package(gg CONFIG REQUIRED)

add_executable(gg_consumer main.c)
target_link_libraries(gg_consumer PRIVATE gg::gg)
