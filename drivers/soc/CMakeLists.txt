# Off for images meant for machines without a Qualcomm SoC, such as QEMU, where
# the boot start platform drivers have no hardware to bind to
option(QCOM_PLATFORM "Register the Qualcomm platform drivers on the boot media" ON)

if(ARCH STREQUAL "arm64" AND QCOM_PLATFORM)
    add_subdirectory(qcom)
endif()
