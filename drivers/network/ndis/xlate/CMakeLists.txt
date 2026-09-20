#
# The NDIS 5 to 6 translation, built as a static library so it can be linked
# into both ndis.sys and the kmtest that proves it. The alternative would be
# exporting internal routines just to test them.
#
add_definitions(
    -DNDIS_WRAPPER
    -DNDIS51
    -DNDIS51_MINIPORT
    -DNDIS_MINIPORT_DRIVER
    -DNDIS_LEGACY_DRIVER=1
    -DNDIS_LEGACY_MINIPORT=1
    -DNDIS_LEGACY_PROTOCOL=1)

add_library(ndisxlate xlate.c)
add_dependencies(ndisxlate psdk)
