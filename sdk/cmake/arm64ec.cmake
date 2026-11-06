
include(ExternalProject)

# User mode DLLs are linked as ARM64X, executables stay ARM64
list(APPEND ARM64EC_MODULE_TYPES win32dll win32ocx cpl)

function(setup_arm64ec)
    if(CMAKE_TOOLCHAIN_FILE)
        set(ARM64EC_TOOLCHAIN_FILE ${CMAKE_TOOLCHAIN_FILE})
    else()
        set(ARM64EC_TOOLCHAIN_FILE ${REACTOS_SOURCE_DIR}/toolchain-msvc.cmake)
    endif()

    # Same compiler and shell as the ARM64 build, cl switches to ARM64EC with /arm64EC.
    # It links the ARM64X images, so the ARM64 halves are built first, see finish_arm64ec.
    ExternalProject_Add(arm64ec-binaries
        SOURCE_DIR ${REACTOS_SOURCE_DIR}
        PREFIX ${REACTOS_BINARY_DIR}/arm64ec
        BINARY_DIR ${REACTOS_BINARY_DIR}/arm64ec
        CMAKE_ARGS
            -DCMAKE_TOOLCHAIN_FILE:FILEPATH=${ARM64EC_TOOLCHAIN_FILE}
            -UCMAKE_GENERATOR_PLATFORM
            -DARCH:STRING=arm64ec
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DENABLE_ROSTESTS=${ENABLE_ROSTESTS}
            -DENABLE_ROSAPPS=${ENABLE_ROSAPPS}
            -DENABLE_WALLPAPERS=${ENABLE_WALLPAPERS}
            -DARM64X_NATIVE_BINARY_DIR=${REACTOS_BINARY_DIR}
        BUILD_COMMAND ${CMAKE_COMMAND} -DBINARY_DIR=${REACTOS_BINARY_DIR}/arm64ec -P ${REACTOS_SOURCE_DIR}/sdk/cmake/arm64ec_build.cmake
        BUILD_ALWAYS TRUE
        INSTALL_COMMAND ${CMAKE_COMMAND} -E true
        USES_TERMINAL_BUILD TRUE
        USES_TERMINAL_CONFIGURE TRUE
    )
endfunction()

# The ARM64EC build needs the ARM64 objects of every module it links as ARM64X
function(finish_arm64ec)
    get_property(_modules GLOBAL PROPERTY ARM64EC_NATIVE_MODULES)
    if(_modules)
        add_dependencies(arm64ec-binaries ${_modules})
    endif()

    # The ARM64X links add the native load config from this library
    add_dependencies(arm64ec-binaries hybrid)
endfunction()

#
# Returns the file name a module target links to, known at configure time
# so the staged copy can be a real build output.
#
function(get_module_file_name _target _out)
    get_target_property(_name ${_target} OUTPUT_NAME)
    if(NOT _name)
        set(_name ${_target})
    endif()

    get_target_property(_suffix ${_target} SUFFIX)
    if(NOT _suffix)
        get_target_property(_type ${_target} TYPE)
        if(_type STREQUAL "EXECUTABLE")
            set(_suffix ".exe")
        else()
            set(_suffix ".dll")
        endif()
    endif()

    set(${_out} "${_name}${_suffix}" PARENT_SCOPE)
endfunction()

#
# Puts a staged copy of a module on the media, the ARM64EC build when
# it produced one and the ARM64 build otherwise.
#
function(stage_arm64ec_module _target _file_out _target_out)
    # Targets defined after their add_cd_file are boot and media files, never user mode modules
    if(NOT TARGET ${_target})
        return()
    endif()

    get_target_property(_type ${_target} REACTOS_MODULE_TYPE)
    if(NOT _type IN_LIST ARM64EC_MODULE_TYPES)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY ARM64EC_NATIVE_MODULES ${_target})

    get_module_file_name(${_target} _file_name)
    get_target_property(_binary_dir ${_target} BINARY_DIR)
    file(RELATIVE_PATH _relative_dir ${REACTOS_BINARY_DIR} ${_binary_dir})
    set(_staged ${REACTOS_BINARY_DIR}/arm64ec-stage/${_relative_dir}/${_file_name})

    if(NOT TARGET ${_target}_arm64ec)
        add_custom_command(
            OUTPUT ${_staged}
            COMMAND ${CMAKE_COMMAND}
                -DNATIVE=$<TARGET_FILE:${_target}>
                -DHYBRID=${REACTOS_BINARY_DIR}/arm64ec/${_relative_dir}/${_file_name}
                -DOUT=${_staged}
                -P ${REACTOS_SOURCE_DIR}/sdk/cmake/arm64ec_stage.cmake
            DEPENDS ${_target} arm64ec-binaries ${REACTOS_SOURCE_DIR}/sdk/cmake/arm64ec_stage.cmake)
        add_custom_target(${_target}_arm64ec DEPENDS ${_staged})
    endif()

    set(${_file_out} ${_staged} PARENT_SCOPE)
    set(${_target_out} ${_target}_arm64ec PARENT_SCOPE)
endfunction()
