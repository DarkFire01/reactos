
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
            add_cd_file(FILE ${QCOM_SDM845_FIRMWARE}/${_file} DESTINATION reactos/system32/firmware/sdm845 NO_CAB FOR bootcd livecd)
        else()
            message(WARNING "SDM845 firmware ${_file} is missing from ${QCOM_SDM845_FIRMWARE}")
        endif()
    endforeach()
endif()
