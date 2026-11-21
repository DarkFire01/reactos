# OnePlus 6 (enchilada) on the SDM845
add_subdirectory(amp)
add_subdirectory(touch)

# Battery manager profile for the OnePlus 6 cell, over the SDM850 MTP tables in the DSDT
add_registry_inf(qcbattery_reg.inf)

# Quaternary MI2S lines and the AFE ports a voice call uses, read by qcomq6
add_registry_inf(qcomq6_board_reg.inf)
