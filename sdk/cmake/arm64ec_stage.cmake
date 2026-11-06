
# Picks the ARM64X build of a module when there is one, the ARM64 one otherwise
if(EXISTS "${HYBRID}")
    set(_source "${HYBRID}")
else()
    set(_source "${NATIVE}")
endif()

get_filename_component(_out_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")
file(COPY_FILE "${_source}" "${OUT}")
