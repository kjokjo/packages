cmake_minimum_required(VERSION 2.8.8)

project(AUTOUPDATER C)

set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS _GNU_SOURCE)

add_executable(autoupdater
  autoupdater.c
)
set_property(TARGET autoupdater PROPERTY COMPILE_FLAGS "-std=c99 -Wall")
#set_property(TARGET autoupdater PROPERTY LINK_FLAGS "")
target_link_libraries(autoupdater m)

install(TARGETS autoupdater RUNTIME DESTINATION sbin)
