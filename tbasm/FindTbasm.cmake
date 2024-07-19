# Finds (really, builds) the tbasm executable.
#
# This expects the following variables to be set:
#
#	TBASM_SOURCE_DIR	Where this file is located.
#
# This will define the following variables:
#
#	Tbasm_FOUND
#
# and the following imported targets:
#
#	Tbasm
#
if (NOT Tbasm_FOUND)
	include(ExternalProject)

	set(TBASM_BINARY_DIR ${CMAKE_BINARY_DIR}/tbasm)

	set(TbasmBuild_TARGET TbasmBuild)
	set(Tbasm_TARGET Tbasm)

	if (NOT TARGET ${TbasmBuild_TARGET})
	ExternalProject_Add(${TbasmBuild_TARGET}
		PREFIX tbasm
		SOURCE_DIR ${TBASM_SOURCE_DIR}
		BINARY_DIR ${TBASM_BINARY_DIR}
		CMAKE_ARGS "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}"
		CMAKE_CACHE_ARGS "-DTBASM_EXTRA_SOURCE_FILES:STRING=${TBASM_EXTRA_SOURCE_FILES}"
		BUILD_ALWAYS 1 # force dependency checking
		INSTALL_COMMAND ""
		)
	endif()

	if (CMAKE_HOST_WIN32)
		set(Tbasm_EXECUTABLE ${TBASM_BINARY_DIR}/tbasm.exe)
	else()
		set(Tbasm_EXECUTABLE ${TBASM_BINARY_DIR}/tbasm)
	endif()

	if (NOT TARGET ${Tbasm_TARGET})
		add_executable(${Tbasm_TARGET} IMPORTED)
	endif()
	set_property(TARGET ${Tbasm_TARGET} PROPERTY IMPORTED_LOCATION
		${Tbasm_EXECUTABLE})

	add_dependencies(${Tbasm_TARGET} ${TbasmBuild_TARGET})
	set (Tbasm_FOUND 1)
endif()
