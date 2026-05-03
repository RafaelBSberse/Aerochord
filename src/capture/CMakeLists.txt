add_library(aerochord_capture STATIC
    CaptureModule.cpp
)

target_link_libraries(aerochord_capture
    PUBLIC aerochord_common
)
