add_library(aerochord_gesture_analysis STATIC
    GestureAnalysisModule.cpp
)

target_link_libraries(aerochord_gesture_analysis
    PUBLIC aerochord_common
)
