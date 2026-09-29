# The image is one whole segment: the processor comes out of reset near its
# top, so the entry point is only where it belongs if nothing is trimmed.
CreateBootSectorTarget(rtvmbios
    ${CMAKE_CURRENT_SOURCE_DIR}/rtvmbios.S
    ${CMAKE_CURRENT_BINARY_DIR}/rtvmbios.bin
    0)

add_cd_file(TARGET rtvmbios
    FILE ${CMAKE_CURRENT_BINARY_DIR}/rtvmbios.bin
    DESTINATION reactos/system32
    NO_CAB
    FOR all)
