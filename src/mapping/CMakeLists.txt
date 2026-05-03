add_library(aerochord_mapping STATIC
    MappingModule.cpp
)

target_link_libraries(aerochord_mapping
    PUBLIC aerochord_common
)
