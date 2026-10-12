set(exe_name stack_demo_3)
set(SRC "main.cpp" "yq_coroutine.cpp")
add_executable (${exe_name} ${SRC})

add_executable(stack_demo_3_loop "loop_example.cpp" "yq_coroutine.cpp")
