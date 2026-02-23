# NVMe storage miniport driver (nvme2k) - SCSI miniport for NVMe controllers
# Port of nvme2k for ReactOS; boot device like usbstor/uniata

add_library(nvme2k MODULE
    nvme2k.c
    nvme2k_cpl.c
    nvme2k_nvme.c
    nvme2k_scsi.c
    utils.c
    nvme2k.rc
)
target_link_libraries(nvme2k libcntpr)
set_module_type(nvme2k kernelmodedriver)
add_importlibs(nvme2k scsiport ntoskrnl hal)
add_cd_file(TARGET nvme2k DESTINATION reactos/system32/drivers NO_CAB FOR all)
#add_registry_inf(nvme2k_reg.inf)
