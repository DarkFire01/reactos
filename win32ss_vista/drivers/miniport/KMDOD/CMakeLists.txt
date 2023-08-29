

remove_definitions(-D_WIN32_WINNT=0x502)
remove_definitions(-D_WIN32_WINNT=0x601)
add_definitions(-D_WIN32_WINNT=0x602)

list(APPEND SOURCE
     bdd_ddi.cxx
     bdd_dmm.cxx
     bdd_util.cxx
     entry.c
     bdd.cxx
     bltfuncs.cxx
     blthw.cxx
     memory.cxx)

add_library(SampleDisplay MODULE
    ${SOURCE}
    sampledisplay.rc
    )

if(USE_CLANG_CL)
    target_compile_options(SampleDisplay PRIVATE "-Wno-missing-braces")
endif()

set_module_type(SampleDisplay kernelmodedriver ENTRYPOINT DriverEntry)
target_link_libraries(SampleDisplay displib)
add_importlibs(SampleDisplay ntoskrnl hal dxgkrnl)
add_driver_inf(SampleDisplay sampledisplay.inf)
add_cd_file(TARGET SampleDisplay DESTINATION reactos/system32/drivers NO_CAB FOR all)
