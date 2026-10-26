# pot_amiga: an Amiga 500 emulator as a Potluck app component (see include/amiga/amiga.hpp and NOTICE).
# Potluck's build picks it up because potluck-modules.json sits beside this file, and calls app_rows_pot_amiga().
set(A500 third_party/a500)
set(M68K third_party/musashi)
set(EMU_SRCS
    ${A500}/blitter.cpp ${A500}/cia.cpp ${A500}/custom.cpp ${A500}/disk.cpp ${A500}/drive.cpp ${A500}/input.cpp
    ${A500}/memory.cpp ${A500}/paula.cpp ${A500}/video.cpp
    ${M68K}/m68kcpu.c ${M68K}/m68kops.c ${M68K}/softfloat/softfloat.c)
idf_component_register(
    SRCS "src/amiga.cpp" "src/emu.cpp" "src/shim.cpp" "src/app_rows.cpp" ${EMU_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS ${A500} ${M68K} "shim"
    REQUIRES pot_actors pot_link pot_ns pot_deploy
    PRIV_REQUIRES esp_timer esp_netif lwip heap
    LDFRAGMENTS "linker.lf"
)
# The emulator's sources are built as their authors wrote them: their board path (ARDUINO, through shim/Arduino.h),
# the 68000's tables in PSRAM, and their warnings not ours to fix.
set_source_files_properties(${EMU_SRCS} PROPERTIES
    COMPILE_DEFINITIONS "ARDUINO=1;M68K_TABLE_ALLOC_FN=pot_amiga_table_alloc"
    COMPILE_OPTIONS "-w")
set_source_files_properties(src/emu.cpp src/shim.cpp PROPERTIES COMPILE_DEFINITIONS "ARDUINO=1")
