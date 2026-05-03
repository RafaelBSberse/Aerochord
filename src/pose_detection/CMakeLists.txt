add_library(aerochord_pose_detection STATIC
    PoseDetectionModule.cpp
)

target_link_libraries(aerochord_pose_detection
    PUBLIC  aerochord_common
    PRIVATE mediapipe
)
