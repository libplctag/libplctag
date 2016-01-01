cmake_minimum_required(VERSION 2.8)

project(libplctag)

find_package(Threads REQUIRED)

set(my_flags "-std=c99 -fno-strict-aliasing -fms-extensions -g -Wall -O2 -D__USE_POSIX=1 -D_POSIX_C_SOURCE=200809L")
set(my_lib_flags "${my_flags} -fPIC")
set(my_example_flags "${my_flags} -pthread")

include_directories(lib lib/ab lib/linux lib/util)
add_library(plctag SHARED lib/libplctag_tag.c lib/ab/ab_common.c lib/ab/cip.c lib/ab/connection.c lib/ab/eip.c lib/ab/eip_cip.c lib/ab/eip_dhp_pccc.c lib/ab/eip_pccc.c lib/ab/pccc.c lib/ab/request.c lib/ab/session.c lib/linux/platform.c lib/util/attr.c)
set_target_properties(plctag PROPERTIES COMPILE_FLAGS ${my_lib_flags})

#file(GLOB_RECURSE examples examples/*.c)
set(examples examples/tag_rw.c)
foreach(example_filename ${examples})
	get_filename_component(example ${example_filename} NAME_WE)
	add_executable(${example} ${example_filename})
	set_target_properties(${example} PROPERTIES COMPILE_FLAGS ${my_example_flags})
	target_link_libraries(${example} plctag ${CMAKE_THREAD_LIBS_INIT})
endforeach(example_filename ${examples})
