cmake_minimum_required(VERSION 3.22)

project(JTTB VERSION 0.5)

add_compile_options(-Wall -Wformat -Werror)

add_subdirectory(tbvm)
add_subdirectory(jttb)
