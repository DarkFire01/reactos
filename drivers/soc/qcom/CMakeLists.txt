# The Windows Qualcomm drivers run the hardware (sdm845/windows), so only the
# touch filter, the firmware overlay and the audio drivers are built from here.
# The audio drivers import the logging, pin and I2C engine libraries from gen.
add_subdirectory(gen/dbg)
add_subdirectory(gen/pnp)
add_subdirectory(gen/geni)
add_subdirectory(gen/tlmm)
add_subdirectory(sdm845)
