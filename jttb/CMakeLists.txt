cmake_minimum_required(VERSION 3.12)

add_executable(jttb
	jttb.c
	)

install(TARGETS jttb)

target_include_directories(tbvm INTERFACE
	${CMAKE_CURRENT_SOURCE_DIR}
	)

target_link_libraries(jttb PUBLIC
	tbvm
	m
	)
