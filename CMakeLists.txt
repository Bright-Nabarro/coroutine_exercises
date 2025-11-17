cmake_minimum_required (VERSION 3.20)
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  #set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("coroutine_exercises")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (MSVC)
	#add_compile_definitions(${PROJECT_NAME} PRIVATE
	#	"/utf-8"
	#	"/fsanitize=address"
	#)
else()
	add_compile_definitions(${PROJECT_NAME} PRIVATE
		"-Wall"
		"-Wextra"
	)
endif()

add_subdirectory(stack)
