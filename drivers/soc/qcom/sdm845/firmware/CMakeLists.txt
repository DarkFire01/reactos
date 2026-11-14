
# Qualcomm subsystem firmware is signed per device and cannot ship in the tree.
# Point QCOM_SDM845_FIRMWARE at a local copy (for example the postmarketOS
# firmware-oneplus-sdm845 lib/firmware/qcom/sdm845/OnePlus/enchilada folder)
# to put it on the boot media under system32\firmware\sdm845.
if(QCOM_SDM845_FIRMWARE)
    list(APPEND QCOM_SDM845_FIRMWARE_FILES
        mba.mbn
        modem.mbn
        modemr.jsn
        modemuw.jsn
        adsp.mbn
        adspr.jsn
        adspua.jsn
        cdsp.mbn
        cdspr.jsn
        ipa_fws.mbn)

    foreach(_file ${QCOM_SDM845_FIRMWARE_FILES})
        if(EXISTS ${QCOM_SDM845_FIRMWARE}/${_file})
            add_cd_file(FILE ${QCOM_SDM845_FIRMWARE}/${_file} DESTINATION reactos/system32/firmware/sdm845 NO_CAB FOR bootcd livecd regtest)
        else()
            message(WARNING "SDM845 firmware ${_file} is missing from ${QCOM_SDM845_FIRMWARE}")
        endif()
    endforeach()
endif()

# Slipstream overlays. Each directory mirrors the boot media layout and every file
# in it is copied onto every bootable image at the same path, so signed firmware,
# per device modem NV and prebuilt drivers can be dropped in without touching the build.
# QCOM_SLIPSTREAM is a list; it defaults to the modules folder beside the source tree.
if(NOT QCOM_SLIPSTREAM)
    set(QCOM_SLIPSTREAM "${CMAKE_SOURCE_DIR}/../modules")
endif()

foreach(_overlay ${QCOM_SLIPSTREAM})
    if(NOT EXISTS ${_overlay})
        continue()
    endif()

    file(GLOB_RECURSE _files CONFIGURE_DEPENDS RELATIVE ${_overlay} ${_overlay}/*)
    foreach(_rel ${_files})
        if(_rel MATCHES "(^|/)(README\\.md|\\.gitignore|\\.gitkeep)$")
            continue()
        endif()

        get_filename_component(_dir ${_rel} DIRECTORY)
        if(_dir STREQUAL "")
            add_cd_file(FILE ${_overlay}/${_rel} DESTINATION reactos NO_CAB FOR bootcd livecd regtest)
        else()
            add_cd_file(FILE ${_overlay}/${_rel} DESTINATION reactos/${_dir} NO_CAB FOR bootcd livecd regtest)
        endif()
    endforeach()
endforeach()
