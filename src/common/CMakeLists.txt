add_library(aerochord_common STATIC
    Logger.cpp
)

target_include_directories(aerochord_common
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..   # expõe src/ para includes do tipo <common/Logger.h>
)

target_compile_features(aerochord_common PUBLIC cxx_std_17)
