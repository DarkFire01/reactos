# Picks the ARM64X build of a module when there is one, the ARM64 one otherwise
# A failed link can leave an empty file behind
set(_hybrid_size 0)
if(EXISTS "${HYBRID}")
    file(SIZE "${HYBRID}" _hybrid_size)
endif()

# A link without ARM64 objects gives an ARM64EC only image, which ARM64 processes cannot load
set(_hybrid_machine "")
if(_hybrid_size GREATER 64)
    file(READ "${HYBRID}" _lfanew LIMIT 4 OFFSET 60 HEX)
    string(REGEX REPLACE "^(..)(..)(..)(..)$" "\\4\\3\\2\\1" _lfanew "${_lfanew}")
    math(EXPR _machine_offset "0x${_lfanew} + 4")
    file(READ "${HYBRID}" _hybrid_machine LIMIT 2 OFFSET ${_machine_offset} HEX)
endif()

if(_hybrid_machine STREQUAL "64aa")
    set(_source "${HYBRID}")
else()
    set(_source "${NATIVE}")
endif()

get_filename_component(_out_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")
file(COPY_FILE "${_source}" "${OUT}")
