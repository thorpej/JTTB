cmake_minimum_required(VERSION 3.12)

include(${PROJECT_SOURCE_DIR}/tbasm/tbasm.cmake)

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tbvm_program.h
	COMMAND tbasm -H${CMAKE_CURRENT_BINARY_DIR}/tbvm_program.h ${CMAKE_CURRENT_SOURCE_DIR}/tbvm_program.asm
	DEPENDS tbasm
	)

add_library(tbvm STATIC
	${CMAKE_CURRENT_BINARY_DIR}/tbvm_program.h
	tbvm.c
	)

target_include_directories(tbvm INTERFACE
	${CMAKE_CURRENT_SOURCE_DIR}
	)

target_include_directories(tbvm PRIVATE
	${CMAKE_CURRENT_BINARY_DIR}
	)
