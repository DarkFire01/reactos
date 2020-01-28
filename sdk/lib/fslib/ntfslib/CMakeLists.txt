add_library(ntfslib ntfslib.c bootsect.c attrib.c files.c ntfslib.h data.h)
add_dependencies(ntfslib psdk)
