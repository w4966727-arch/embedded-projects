cmake_minimum_required(VERSION 3.16)
project(pc_protocol_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 打开警告（工程习惯：代码应当零警告；确认干净后可加 -Werror 强制）
add_compile_options(-Wall -Wextra)

# ① 协议库：直接复用仓库里那份三端共用代码，不复制！
add_library(frame_protocol STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame_protocol.c
)
target_include_directories(frame_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

# ② 测试程序
add_executable(test_frame_protocol test_frame_protocol.c)
target_link_libraries(test_frame_protocol PRIVATE frame_protocol)

# ③ 注册到 CTest，可用 ctest 批量跑
enable_testing()
add_test(NAME frame_protocol COMMAND test_frame_protocol)