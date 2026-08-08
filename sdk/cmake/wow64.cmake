include(ExternalProject)

function(setup_wow64)
    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" lowercase_CMAKE_HOST_SYSTEM_PROCESSOR)
    if(lowercase_CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL x86 OR lowercase_CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^i[3456]86$")
        set(HOST_ARCH i386)
        set(VCVARSALL_ARCH x86)
    elseif(lowercase_CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL x86_64 OR lowercase_CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL amd64)
        set(HOST_ARCH amd64)
        set(VCVARSALL_ARCH amd64_x86)
    elseif(lowercase_CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL arm)
        set(HOST_ARCH arm)
        set(VCVARSALL_ARCH x86)
    # 'aarch64' is used in GNU tools instead of 'arm64'
    elseif(lowercase_CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL arm64 OR lowercase_CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL aarch64)
        set(HOST_ARCH arm64)
        set(VCVARSALL_ARCH x86)
    else()
        message(FATAL_ERROR "Unknown host architecture: ${lowercase_CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()

    if(MSVC)
        message("Compiling on ${HOST_ARCH} for ${ARCH} (MSVC)")
        set(WOW64_CMAKE_COMMAND "${REACTOS_BINARY_DIR}/wow64/cmake_shim.cmd")
        if(MSVC_VERSION EQUAL 1900)
            file(WRITE ${WOW64_CMAKE_COMMAND}
                "set VSCMD_SKIP_SENDTELEMETRY=1\n"
                "@call \"$ENV{VCINSTALLDIR}\\vcvarsall.bat\" ${VCVARSALL_ARCH}\n"
                "\"${CMAKE_COMMAND}\" %*"
            )
        elseif(MSVC_VERSION GREATER_EQUAL 1910)
            # 2017 and 2019 use the same folder structure
            file(WRITE ${WOW64_CMAKE_COMMAND}
                "@set VSCMD_ARG_no_logo=1\n"
                "@call \"$ENV{VCINSTALLDIR}\\Auxiliary\\Build\\vcvarsall.bat\" /clean_env\n"
                "@call \"$ENV{VCINSTALLDIR}\\Auxiliary\\Build\\vcvarsall.bat\" ${VCVARSALL_ARCH}\n"
                "\"${CMAKE_COMMAND}\" %*"
            )
        else()
            message(FATAL "Unable to figure out vcvarsall path")
        endif()
    else()
        if(WIN32)
            set(WOW64_CMAKE_COMMAND "${REACTOS_BINARY_DIR}/wow64/cmake_shim.cmd")
            file(WRITE ${WOW64_CMAKE_COMMAND}
                "@call rosbe i386\n" 
                "\"${CMAKE_COMMAND}\" %*"
            )
        else()
            set(WOW64_CMAKE_COMMAND "${REACTOS_BINARY_DIR}/wow64/cmake_shim.sh")
            message(FATAL_ERROR "Building WOW64 under RosBE-Unix is not supported yet.")
        endif()
    endif()

    # CMake might choose clang if it finds it in the PATH. Always prefer cl for wow64
    if (MSVC)
        list(APPEND CMAKE_WOW64_TOOLS_EXTRA_ARGS
            -DCMAKE_C_COMPILER=cl
            -DCMAKE_CXX_COMPILER=cl)
    endif()

    if (MSVC_IDE)
        # Required for Bison/Flex wrappers created by /CMakeLists.txt.
        list(APPEND CMAKE_WOW64_TOOLS_EXTRA_ARGS
            -DROS_SAVED_BISON_PKGDATADIR=${ROS_SAVED_BISON_PKGDATADIR}
            -DROS_SAVED_M4=${ROS_SAVED_M4}
            )
    endif()

    if (CMAKE_TOOLCHAIN_FILE)
        set(WOW64_TOOLCHAIN_FILE ${CMAKE_TOOLCHAIN_FILE})
    elseif(MSVC)
        set(WOW64_TOOLCHAIN_FILE ${REACTOS_SOURCE_DIR}/toolchain-msvc.cmake)
    else()
        set(WOW64_TOOLCHAIN_FILE ${REACTOS_SOURCE_DIR}/toolchain-gcc.cmake)
    endif()

    ExternalProject_Add(wow64-binaries
        SOURCE_DIR ${REACTOS_SOURCE_DIR}
        PREFIX ${REACTOS_BINARY_DIR}/wow64
        BINARY_DIR ${REACTOS_BINARY_DIR}/wow64
        CMAKE_COMMAND ${WOW64_CMAKE_COMMAND}
        CMAKE_ARGS
            -DCMAKE_TOOLCHAIN_FILE:FILEPATH=${WOW64_TOOLCHAIN_FILE}
            -UCMAKE_GENERATOR_PLATFORM
            -UMINGW_TOOLCHAIN_PREFIX
            -UMINGW_TOOLCHAIN_SUFFIX
            -DNO_ROSSYM=1
            -DARCH:STRING=i386
            -DCMAKE_INSTALL_PREFIX=${REACTOS_BINARY_DIR}/wow64
            -DTOOLS_FOLDER=${REACTOS_BINARY_DIR}/host-tools/bin
            -DTARGET_COMPILER_ID=${CMAKE_C_COMPILER_ID}
            -DTARGET_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DCMAKE_BUILD_TYPE=${WOW64_BUILD_TYPE}
            -DSYSWOW64_BUILD=1
            ${CMAKE_WOW64_TOOLS_EXTRA_ARGS}
        BUILD_ALWAYS TRUE
        BUILD_BYPRODUCTS ${REACTOS_BINARY_DIR}/wow64/kernel32.dll
        DEPENDS host-tools
        USES_TERMINAL_BUILD TRUE
        USES_TERMINAL_CONFIGURE TRUE
    )
endfunction()

function(add_wow64_executable _target)
    add_executable(wow64-${_target} IMPORTED)
    set_target_properties(wow64-${_target} PROPERTIES IMPORTED_LOCATION ${REACTOS_BINARY_DIR}/wow64/${_target}.exe)
    add_dependencies(wow64-${_target} wow64-binaries ${REACTOS_BINARY_DIR}/wow64/${_target}.exe)
endfunction()

function(add_wow64_library _target)
    add_library(wow64-${_target} SHARED IMPORTED)
    set_target_properties(wow64-${_target} PROPERTIES IMPORTED_LOCATION ${REACTOS_BINARY_DIR}/wow64/${_target}.dll)
    add_dependencies(wow64-${_target} wow64-binaries ${REACTOS_BINARY_DIR}/wow64/${_target}.dll)
endfunction()