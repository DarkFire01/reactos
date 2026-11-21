# Board specific drivers: the parts a device adds around its SoC (amplifiers,
# touch controllers, gauges, battery profiles). SoC drivers stay in drivers/soc.
option(QCOM_PLATFORM "Register the Qualcomm platform drivers on the boot media" ON)

if(ARCH STREQUAL "arm64" AND QCOM_PLATFORM)
    add_subdirectory(oneplus6)
endif()
