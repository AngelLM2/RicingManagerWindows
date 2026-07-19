cmake_minimum_required(VERSION 3.10.0 FATAL_ERROR)
project(RicingManagerWindows VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ATENÇÃO, TODA VEZ Q FOR ADICIONADO UM SOURCE CODE NOVO (.hpp)
# É RECOMENDADO REGERAR O CMAKE (apagar a pasta build e fazer de novo)

file(GLOB_RECURSE _RicingManagerWindows_Sources
    ${PROJECT_SOURCE_DIR}/src/*.cpp
)

add_executable(RicingManagerWindows
	${_RicingManagerWindows_Sources}
)

target_include_directories(RicingManagerWindows PRIVATE
	src/
)

# target_link_directories(RicingManagerWindows PRIVATE
#	  ${CMAKE_CURRENT_SOURCE_DIR}/lib/x64
# )

# add_custom_command(TARGET RicingManagerWindows POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E copy_if_different
#        ${CMAKE_CURRENT_SOURCE_DIR}/lib/x64/lib_para_linkar.dll $<TARGET_FILE_DIR:RicingManagerWindows>)