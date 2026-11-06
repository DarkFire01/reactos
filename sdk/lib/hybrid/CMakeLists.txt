
add_asm_files(hybrid_asm chpe.s helpers.s)
add_library(hybrid ${hybrid_asm})
set_target_properties(hybrid PROPERTIES LINKER_LANGUAGE "C")
add_dependencies(hybrid asm)
