
list(APPEND SOURCE
    smfx.cpp)

add_library(smfx STATIC ${SOURCE})
add_dependencies(smfx bugcodes xdk)
set_target_properties(smfx PROPERTIES CXX_STANDARD 17)
