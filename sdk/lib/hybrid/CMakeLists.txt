
# The ARM64 half of an ARM64X image only needs its own load config
if(ARCH STREQUAL "arm64ec")
    # Large ARM64EC frames probe the stack through __chkstk_arm64ec in every module
    add_asm_files(hybrid_asm chpe.s helpers.s ${REACTOS_SOURCE_DIR}/sdk/lib/crt/except/arm64/chkstk_asm.s)
else()
    add_asm_files(hybrid_asm loadcfg.s)
endif()
add_library(hybrid ${hybrid_asm})
set_target_properties(hybrid PROPERTIES LINKER_LANGUAGE "C")
add_dependencies(hybrid asm)
