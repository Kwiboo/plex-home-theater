set(SOURCES ProcessInfoGBM.cpp
            VideoBufferDRMPRIME.cpp
            VideoBufferDumb.cpp
            VideoBufferMemfd.cpp)

set(HEADERS ProcessInfoGBM.h
            VideoBufferDRMPRIME.h
            VideoBufferDumb.h
            VideoBufferMemfd.h)

core_add_library(processGBM)
