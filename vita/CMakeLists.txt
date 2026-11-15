cmake_minimum_required(VERSION 3.15)
project(daytona_vita C)

# The game code is generated from the PC executable (recomp/gen), the
# runtime emulates Win32/DirectX on top of vitaGL / sceAudio / sceCtrl.
file(GLOB GEN_SOURCES ../recomp/gen/*.c ../recomp/gen_ddse/*.c ../recomp/overrides/*.c ../recomp/lifted/*.c)
set(RUNTIME_SOURCES
    ../runtime/main.c ../runtime/image.c ../runtime/rt_core.c ../runtime/host.c ../runtime/crt.c
    ../runtime/w32_kernel.c ../runtime/w32_user.c ../runtime/w32_reg.c ../runtime/w32_mm.c
    ../runtime/w32_stubs.c ../runtime/res.c ../runtime/ddse_glue.c ../runtime/lifted_rt.c
    ../runtime/dx_ddraw.c ../runtime/dx_d3d.c ../runtime/dx_sound_input.c
    ../runtime/render.c ../runtime/audio.c ../runtime/audio_dump.c ../runtime/prof.c ../runtime/menu.c
    plat_vita.c
)

add_executable(daytona_vita ${GEN_SOURCES} ${RUNTIME_SOURCES})
target_include_directories(daytona_vita PRIVATE ../recomp/include ../recomp/gen ../runtime)

# -fsigned-char: the decompiled code assumes x86 signed char.
# -fno-strict-aliasing: the code type-puns memory freely.
# The decompiled code converts between pointers and integers everywhere.
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-O2;-w;-Wno-int-conversion;-Wno-incompatible-pointer-types")
target_compile_definitions(daytona_vita PRIVATE RECOMP_LASTFN)
target_compile_options(daytona_vita PRIVATE -fsigned-char -fno-strict-aliasing -fno-tree-vectorize
    -Wno-int-conversion -Wno-incompatible-pointer-types -O2 -g)

target_link_libraries(daytona_vita
    vitaGL vitashark mathneon SceShaccCgExt taihen_stub
    SceDisplay_stub SceGxm_stub SceKernelThreadMgr_stub SceKernelDmacMgr_stub
    SceAppMgr_stub SceAppUtil_stub SceCommonDialog_stub SceCtrl_stub SceAudio_stub
    ScePower_stub SceShaccCg_stub SceSysmodule_stub SceIofilemgr_stub
    stdc++ m c
)
