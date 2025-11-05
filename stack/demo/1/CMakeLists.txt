# CMakeList.txt : CMake project for 1, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.20)
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  #set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("1")

# Add source to this project's executable.
set(exe_name ${PROJECT_NAME})
add_executable (${PROJECT_NAME} "main.cpp")

if (MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE
		"/utf-8"
		"/fsanitize=address"
	)
else()
	target_compile_options(${PROJECT_NAME} PRIVATE
		"-Wall"
		"-Wextra"
	)
endif()

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 23)

# TODO: Add tests and install targets if needed.
