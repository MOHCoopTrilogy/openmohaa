# HZM coop - standalone build for su44's md5_2_skX (MD5 <-> MOHAA skd/skc/tik converter).
# No 3ds Max needed. Links the engine's q_math.c + q_shared.c (parser) + small stubs.
cmake_minimum_required(VERSION 3.10)
project(md5_2_skX C)
set(CMAKE_C_STANDARD 99)
if(MSVC)
    add_compile_options(/D_CRT_SECURE_NO_WARNINGS /wd4996 /wd4244 /wd4267)
endif()
set(QC ${CMAKE_CURRENT_SOURCE_DIR}/../../qcommon)
add_executable(md5_2_skX
    md5_2_skX.c
    doom3md5model.c
    loadtiki.c
    misc_utils.c
    writeobj.c
    writeskl.c
    writetiki.c
    tool_stubs.c
    ${QC}/q_math.c
    ${QC}/q_shared.c
)
target_include_directories(md5_2_skX PRIVATE ${QC} ${CMAKE_CURRENT_SOURCE_DIR}/../../tiki)
