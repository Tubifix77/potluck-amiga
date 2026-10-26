# potluck-amiga's PC harness (see main.cpp): the board's emulator sources, built for the PC.
cmake_minimum_required(VERSION 3.20)
project(a500_pc C CXX)

set(EMU ${CMAKE_CURRENT_LIST_DIR}/../firmware/components/pot_amiga/third_party)
file(GLOB CHIPSET ${EMU}/a500/*.cpp)
add_executable(a500_pc main.cpp ${CHIPSET}
    ${EMU}/musashi/m68kcpu.c ${EMU}/musashi/m68kops.c ${EMU}/musashi/softfloat/softfloat.c)
target_include_directories(a500_pc PRIVATE ${EMU}/a500 ${EMU}/musashi)
set_target_properties(a500_pc PROPERTIES CXX_STANDARD 17)
if(MSVC)
  target_compile_definitions(a500_pc PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(a500_pc PRIVATE /W1 /FI${CMAKE_CURRENT_LIST_DIR}/pc_compat.h)
else()
  target_compile_options(a500_pc PRIVATE -include ${CMAKE_CURRENT_LIST_DIR}/pc_compat.h)
endif()
