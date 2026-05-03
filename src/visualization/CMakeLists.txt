find_package(OpenCV REQUIRED COMPONENTS core highgui imgproc)

add_library(aerochord_visualization STATIC
    VisualizationModule.cpp
)

target_link_libraries(aerochord_visualization
    PUBLIC  aerochord_common
    PRIVATE ${OpenCV_LIBS}
            aerochord_midi_generation
            aerochord_mapping
)
