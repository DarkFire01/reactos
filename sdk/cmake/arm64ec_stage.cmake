
# Picks the ARM64X build of a module when there is one, the ARM64 one otherwise
# A failed link can leave an empty file behind
set(_hybrid_size 0)
if(EXISTS "${HYBRID}")
    file(SIZE "${HYBRID}" _hybrid_size)
endif()

if(_hybrid_size GREATER 0)
    set(_source "${HYBRID}")
else()
    set(_source "${NATIVE}")
endif()

get_filename_component(_out_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")
file(COPY_FILE "${_source}" "${OUT}")
