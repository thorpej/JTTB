cmake_minimum_required(VERSION 3.17)

project(tbasm)

add_executable(tbasm
	tbasm.c
	)

add_compile_options(-Wall -Wformat -Werror)
