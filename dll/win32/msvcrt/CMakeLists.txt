
###############################################################################
# Global compile options
###############################################################################

# Use conforming swprintfs
remove_definitions(-D_CRT_NON_CONFORMING_SWPRINTFS)

# This requires NT6
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(-D_WIN32_WINNT=0x601 -DWINVER=0x601)

# Replace the global CRT include directory with the msvcrt include directory
get_property(INCLUDE_DIRS DIRECTORY . PROPERTY INCLUDE_DIRECTORIES)
list(REMOVE_ITEM INCLUDE_DIRS "${REACTOS_SOURCE_DIR}/sdk/include/crt")
set_property(DIRECTORY . PROPERTY INCLUDE_DIRECTORIES ${INCLUDE_DIRS})
include_directories(include)

# includes for wine code
include_directories(BEFORE ${REACTOS_SOURCE_DIR}/sdk/include/wine)

add_definitions(
    -D_CRTIMP=

    # Workarounds
    -DARRAY_SIZE=_countof
    -DI64_MIN=_I64_MIN
    -DI64_MAX=_I64_MAX
    -DUI64_MAX=_UI64_MAX
    -Dx87_asin=asin
    -Dmath_error=_invoke_matherr
    -Dasm_sqrt=sqrt
    -Dasm_sqrtf=sqrtf
)

if(ARCH STREQUAL "i386")
    add_definitions(
        -D__ASM_USE_THISCALL_WRAPPER
        -DRtlUnwind=__wine_RtlUnwind # See i386/__wine_RtlUnwind.s
    )
endif()

if(MSVC)
    add_compile_options(
        /wd4090 # 'function': different 'const' qualifiers
        /wd4146 # 'minus' operator applied to unsigned type, result still unsigned
        /wd4164 # 'function': intrinsic function not declared
        /wd4267 # 'conversion from 'size_t' to 'unsigned int', possible loss of data'
        /wd4723 # potential divide by 0
    )
    if(ARCH STREQUAL "amd64")
        add_compile_options(#msvcrt PRIVATE
            /wd4334 # 'result of 32-bit shift implicitly converted to 64 bits (was 64-bit shift intended?)'
        )
    endif()
endif()

###############################################################################
# A static library shared with ucrtbase
###############################################################################

list(APPEND SHARED_SOURCE
    cpp.c
    except.c
    handler4.c
    undname.c
)

if(ARCH STREQUAL "i386")
    list(APPEND SHARED_SOURCE
        except_i386.c
    )
    list(APPEND SHARED_ASM_SOURCE
        i386/__wine_RtlUnwind.s
        i386/exception_ptr.s
        i386/inout.s
    )
elseif(ARCH STREQUAL "amd64")
    list(APPEND SHARED_SOURCE
        except_x86_64.c
    )
elseif(ARCH STREQUAL "arm")
    list(APPEND SHARED_SOURCE
        except_arm.c
    )
elseif(ARCH STREQUAL "arm64")
    list(APPEND SHARED_SOURCE
        except_arm64.c
    )
elseif(ARCH STREQUAL "arm64ec")
    list(APPEND SHARED_SOURCE
        except_arm64ec.c
    )
endif()

# MSVC asm code
if(MSVC)
    if(ARCH STREQUAL "i386")
        list(APPEND SHARED_ASM_SOURCE
            i386/cpp_alias.s
            i386/cpp_vtables.s
            i386/except_i386_asm.s
        )
    elseif(ARCH STREQUAL "amd64")
        list(APPEND SHARED_ASM_SOURCE
            amd64/cpp_alias.s
            amd64/cpp_vtables.s
            amd64/except_x86_64.s
        )
    elseif(ARCH STREQUAL "arm64")
        list(APPEND SHARED_ASM_SOURCE
            arm64/call_exc_handler.asm
            arm64/cpp_vtables.s
        )
    endif()
endif()

add_asm_files(msvcrt_shared_asm ${SHARED_ASM_SOURCE})

add_library(msvcrt_shared
    ${SHARED_SOURCE}
    ${msvcrt_shared_asm}
    reactos/ucrt_tls_sup.c
)

target_compile_definitions(msvcrt_shared PRIVATE _MSVCR_VER=0 __UCRTSUPPORT__)
add_dependencies(msvcrt_shared psdk asm)

###############################################################################
# A static library shared between msvcrt.dll msvcrt20.dll and msvcrt40.dll
###############################################################################

list(APPEND MSVCRT_SOURCE
    ${SHARED_SOURCE}
    ${msvcrt_shared_asm}
    reactos/misc.c
    reactos/system.cpp
    concurrency.c
    console.c
    ctype.c
    data.c
    dir.c
    environ.c
    errno.c
    exit.c
    file.c
    heap.c
    locale.c
    lock.c
    main.c
    math.c
    mbcs.c
    misc.c
    process.c
    scanf.c
    sincos.c
    string.c
    thread.c
    time.c
    wcs.c
)

add_asm_files(msvcrt_asm ${MSVCRT_ASM_SOURCE})
list(APPEND MSVCRT_SOURCE ${msvcrt_asm})

add_library(msvcrt_static
    ${MSVCRT_SOURCE}
)

target_compile_definitions(msvcrt_static PRIVATE _MSVCR_VER=0)
add_dependencies(msvcrt_static psdk asm)

###############################################################################
# MSVCRT.DLL
###############################################################################

spec2def(msvcrt.dll msvcrt.spec ADD_IMPORTLIB)

add_library(msvcrt MODULE
    ${CMAKE_CURRENT_BINARY_DIR}/msvcrt_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/msvcrt.def
    msvcrt.rc
)

target_compile_definitions(msvcrt PRIVATE _MSVCR_VER=0)
set_module_type(msvcrt win32dll ENTRYPOINT DllMain 12)
target_link_libraries(msvcrt msvcrt_static crtmath dbgrpt vcruntime wine chkstk ${PSEH_LIB})
add_importlibs(msvcrt kernel32 kernel32_vista ntdll)
add_delay_importlibs(msvcrt user32 advapi32)

if(MSVC)
    # Silence warning C4102: export of deleting destructor "name"
    target_link_options(msvcrt PRIVATE "/ignore:4102")
    set_property(TARGET libmsvcrt APPEND PROPERTY STATIC_LIBRARY_OPTIONS "/ignore:4102")
    if(ARCH STREQUAL "arm64")
        target_link_options(msvcrt PRIVATE
            "LINKER:/alternatename:??3@YAXPEAX@Z=operator_delete"
            "LINKER:/alternatename:??_U@YAPEAX_K@Z=operator_new"
            "LINKER:/alternatename:??_U@YAPEAX_KHPEBDH@Z=operator_new_dbg"
            "LINKER:/alternatename:??_V@YAXPEAX@Z=operator_delete"
            "LINKER:/alternatename:??2@YAPEAX_K@Z=operator_new"
            "LINKER:/alternatename:??2@YAPEAX_KHPEBDH@Z=operator_new_dbg"
            "LINKER:/alternatename:?_query_new_handler@@YAP6AHI@ZXZ=_query_new_handler"
            "LINKER:/alternatename:?_set_new_handler@@YAP6AHI@ZP6AHI@Z@Z=_set_new_handler"
            "LINKER:/alternatename:?set_new_handler@@YAP6AXXZP6AXXZ@Z=set_new_handler"
            "LINKER:/alternatename:?_query_new_mode@@YAHXZ=_query_new_mode"
            "LINKER:/alternatename:?_set_new_mode@@YAHH@Z=_set_new_mode"
            "LINKER:/alternatename:?_set_se_translator@@YAP6AXIPAU_EXCEPTION_POINTERS@@@ZP6AXI0@Z@Z=_set_se_translator"
            "LINKER:/alternatename:?set_terminate@@YAP6AXXZP6AXXZ@Z=set_terminate"
            "LINKER:/alternatename:?set_unexpected@@YAP6AXXZP6AXXZ@Z=set_unexpected"
            "LINKER:/alternatename:?terminate@@YAXXZ=terminate"
            "LINKER:/alternatename:?unexpected@@YAXXZ=unexpected"
            "LINKER:/alternatename:?what@exception@@UEBAPEBDXZ=exception_what"
            "LINKER:/alternatename:??0exception@@QEAA@AEBQEBDH@Z=exception_ctor_noalloc"
            "LINKER:/alternatename:??0exception@@QEAA@AEBV0@@Z=exception_copy_ctor"
            "LINKER:/alternatename:??0exception@@QEAA@XZ=exception_default_ctor"
            "LINKER:/alternatename:??1exception@@UEAA@XZ=exception_dtor"
            "LINKER:/alternatename:??4exception@@QEAAAEAV0@AEBV0@@Z=exception_opequals"
            "LINKER:/alternatename:??1type_info@@UEAA@XZ=type_info_dtor"
            "LINKER:/alternatename:??0__non_rtti_object@@QEAA@AEBV0@@Z=__non_rtti_object_copy_ctor"
            "LINKER:/alternatename:??0__non_rtti_object@@QEAA@PEBD@Z=__non_rtti_object_ctor"
            "LINKER:/alternatename:??0bad_cast@@AAE@PBQBD@Z=bad_cast_ctor"
            "LINKER:/alternatename:??0bad_cast@@AEAA@PEBQEBD@Z=bad_cast_ctor"
            "LINKER:/alternatename:??0bad_cast@@QAE@ABQBD@Z=bad_cast_ctor"
            "LINKER:/alternatename:??0bad_cast@@QEAA@AEBQEBD@Z=bad_cast_ctor"
            "LINKER:/alternatename:??0bad_cast@@QEAA@AEBV0@@Z=bad_cast_copy_ctor"
            "LINKER:/alternatename:??0bad_cast@@QEAA@PEBD@Z=bad_cast_ctor_charptr"
            "LINKER:/alternatename:??0bad_typeid@@QEAA@AEBV0@@Z=bad_typeid_copy_ctor"
            "LINKER:/alternatename:??0bad_typeid@@QEAA@PEBD@Z=bad_typeid_ctor"
            "LINKER:/alternatename:??0exception@@QEAA@AEBQEBD@Z=exception_ctor"
            "LINKER:/alternatename:??1__non_rtti_object@@UEAA@XZ=__non_rtti_object_dtor"
            "LINKER:/alternatename:??1bad_cast@@UEAA@XZ=bad_cast_dtor"
            "LINKER:/alternatename:??1bad_typeid@@UEAA@XZ=bad_typeid_dtor"
            "LINKER:/alternatename:??4bad_cast@@QEAAAEAV0@AEBV0@@Z=bad_cast_opequals"
            "LINKER:/alternatename:??4bad_typeid@@QEAAAEAV0@AEBV0@@Z=bad_typeid_opequals"
            "LINKER:/alternatename:??8type_info@@QEBAHAEBV0@@Z=type_info_opequals_equals"
            "LINKER:/alternatename:??9type_info@@QEBAHAEBV0@@Z=type_info_opnot_equals"
            "LINKER:/alternatename:??_Fbad_cast@@QEAAXXZ=bad_cast_default_ctor"
            "LINKER:/alternatename:??_Fbad_typeid@@QEAAXXZ=bad_typeid_default_ctor"
            "LINKER:/alternatename:?_query_new_handler@@YAP6AH_K@ZXZ=_query_new_handler"
            "LINKER:/alternatename:?_set_new_handler@@YAP6AH_K@ZP6AH0@Z@Z=_set_new_handler"
            "LINKER:/alternatename:?_set_se_translator@@YAP6AXIPEAU_EXCEPTION_POINTERS@@@ZP6AXI0@Z@Z=_set_se_translator"
            "LINKER:/alternatename:?before@type_info@@QEBAHAEBV1@@Z=type_info_before"
            "LINKER:/alternatename:?name@type_info@@QEBAPEBDXZ=type_info_name"
            "LINKER:/alternatename:?raw_name@type_info@@QEBAPEBDXZ=type_info_raw_name"
            "LINKER:/alternatename:??4__non_rtti_object@@QEAAAEAV0@AEBV0@@Z=__non_rtti_object_opequals"
        )
    endif()
endif()

add_cd_file(TARGET msvcrt DESTINATION reactos/system32 FOR all)

###############################################################################
# Import library
###############################################################################

# Let consumers of msvcrt have the right defines
target_compile_definitions(libmsvcrt INTERFACE _DLL __USE_CRTIMP)

# Link libmsvcrt to msvcrtex
target_link_libraries(libmsvcrt INTERFACE msvcrtex)

# Link GCC stack protector lib
if (STACK_PROTECTOR)
    target_link_libraries(libmsvcrt INTERFACE gcc_ssp_msvcrt)
    target_link_libraries(libmsvcrt INTERFACE libkernel32) # For OutputDebugStringA
endif()
