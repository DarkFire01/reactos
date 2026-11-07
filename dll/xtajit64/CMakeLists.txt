
# The x64 emulators of ARM64: FEX, a JIT, and the interpreter here.
# ntdll loads xtajit64.dll unless the default value of HKLM\Software\Microsoft\Wow64\amd64 names another one.
if(NOT ARCH STREQUAL "arm64")
    return()
endif()

# FEX only builds with clang targeting MinGW, so it comes from an llvm-mingw toolchain
set(LLVM_MINGW_PATH "" CACHE PATH "llvm-mingw toolchain that builds the FEX x64 emulator")

# find_package(Python3) makes imported executables, which the add_executable override here does not take
if(LLVM_MINGW_PATH)
    find_program(FEX_PYTHON_EXECUTABLE NAMES python3 python)
    if(NOT FEX_PYTHON_EXECUTABLE)
        message(WARNING "FEX generates sources with Python, it is not built")
    endif()
else()
    message(STATUS "LLVM_MINGW_PATH is not set, FEX is not built and the interpreter is xtajit64.dll")
endif()

# With FEX as xtajit64.dll the interpreter stays available as xtaint64.dll
if(LLVM_MINGW_PATH AND FEX_PYTHON_EXECUTABLE)
    set(XTA_INTERPRETER xtaint64)
else()
    set(XTA_INTERPRETER xtajit64)
endif()

spec2def(${XTA_INTERPRETER}.dll xtajit64.spec)

list(APPEND SOURCE
    cpu.c
    xtajit64.c)

add_asm_files(xtajit64_asm dispatch.s)

add_library(${XTA_INTERPRETER} MODULE
    ${SOURCE}
    ${xtajit64_asm}
    ${CMAKE_CURRENT_BINARY_DIR}/${XTA_INTERPRETER}.def)

set_module_type(${XTA_INTERPRETER} win32dll ENTRYPOINT 0)
add_importlibs(${XTA_INTERPRETER} ntdll)
add_dependencies(${XTA_INTERPRETER} psdk asm)
add_cd_file(TARGET ${XTA_INTERPRETER} DESTINATION reactos/system32 FOR all)

if(NOT XTA_INTERPRETER STREQUAL "xtaint64")
    return()
endif()

include(ExternalProject)

set(FEX_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/fex)

# FEX runs python3, dd and gzip while it builds, Windows hosts get small stand-ins for them
if(CMAKE_HOST_WIN32)
    set(FEX_HOST_TOOLS_DIR ${CMAKE_CURRENT_BINARY_DIR}/hosttools)
    configure_file(hosttools/python3.cmd.in ${FEX_HOST_TOOLS_DIR}/python3.cmd @ONLY)
    configure_file(hosttools/dd.cmd.in ${FEX_HOST_TOOLS_DIR}/dd.cmd @ONLY)
    configure_file(hosttools/gzip.cmd.in ${FEX_HOST_TOOLS_DIR}/gzip.cmd @ONLY)
    configure_file(hosttools/build.cmd.in ${CMAKE_CURRENT_BINARY_DIR}/fexbuild.cmd @ONLY)
    set(FEX_BUILD_COMMAND ${CMAKE_CURRENT_BINARY_DIR}/fexbuild.cmd)
else()
    set(FEX_BUILD_COMMAND ${CMAKE_COMMAND} --build . --target arm64ecfex)
endif()

ExternalProject_Add(fex-emulator
    SOURCE_DIR ${REACTOS_SOURCE_DIR}/sdk/lib/3rdparty/fex
    PREFIX ${FEX_BINARY_DIR}
    BINARY_DIR ${FEX_BINARY_DIR}
    CMAKE_GENERATOR Ninja
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_CURRENT_SOURCE_DIR}/llvm-mingw.cmake
        -DLLVM_MINGW_PATH:PATH=${LLVM_MINGW_PATH}
        -DMINGW_TRIPLE:STRING=arm64ec-w64-mingw32
        -DCMAKE_BUILD_TYPE:STRING=Release
        -DPython_EXECUTABLE:FILEPATH=${FEX_PYTHON_EXECUTABLE}
        -DOVERRIDE_VERSION:STRING=FEX-2610-175-geff9f459d
        -DOVERRIDE_HASH:STRING=eff9f459d8a13dc80aa9edb331c3da0e98334a38
        -DTUNE_CPU:STRING=none
        -DTUNE_ARCH:STRING=generic
        -DBUILD_TESTING:BOOL=OFF
        -DBUILD_FEXCONFIG:BOOL=OFF
        -DENABLE_JEMALLOC_GLIBC_ALLOC:BOOL=OFF
        -DENABLE_LTO:BOOL=OFF
        -DENABLE_CCACHE:BOOL=OFF
        -DENABLE_OFFLINE_TELEMETRY:BOOL=OFF
    BUILD_COMMAND ${FEX_BUILD_COMMAND}
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS ${FEX_BINARY_DIR}/Bin/libarm64ecfex.dll
    INSTALL_COMMAND ${CMAKE_COMMAND} -E true
    USES_TERMINAL_BUILD TRUE
    USES_TERMINAL_CONFIGURE TRUE)

# ntdll loads the emulator by the name Windows uses
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xtajit64.dll
    COMMAND ${CMAKE_COMMAND} -E copy ${FEX_BINARY_DIR}/Bin/libarm64ecfex.dll ${CMAKE_CURRENT_BINARY_DIR}/xtajit64.dll
    DEPENDS fex-emulator ${FEX_BINARY_DIR}/Bin/libarm64ecfex.dll)
add_custom_target(xtajit64 ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/xtajit64.dll)

add_cd_file(TARGET xtajit64 FILE ${CMAKE_CURRENT_BINARY_DIR}/xtajit64.dll DESTINATION reactos/system32 FOR all)
