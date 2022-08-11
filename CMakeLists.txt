cmake_minimum_required(VERSION 3.24)

if(NOT WIN32)
	message(FATAL_ERROR "Error. Your OS is not windows.")
endif()

project(ComPortExample)

set(SOURCE_EXE Main.cpp)
set(SOURCE_LIB ComPort.cpp ComPort.h)

add_library(ComPort STATIC ${SOURCE_LIB})

add_executable(Main ${SOURCE_EXE})

target_link_libraries(Main ComPort)