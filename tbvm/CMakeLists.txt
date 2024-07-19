cmake_minimum_required(VERSION 3.17)

if (NOT Tbasm_FOUND)
	set(TBASM_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../tbasm)
	set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${TBASM_SOURCE_DIR})
	find_package(Tbasm REQUIRED)
endif()

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tbvm_program.h
	COMMAND ${Tbasm_EXECUTABLE} -H${CMAKE_CURRENT_BINARY_DIR}/tbvm_program.h ${CMAKE_CURRENT_SOURCE_DIR}/tbvm_program.asm
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
