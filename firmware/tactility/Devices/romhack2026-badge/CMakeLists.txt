file(GLOB_RECURSE SOURCE_FILES Source/*.cpp)

idf_component_register(
    SRCS ${SOURCE_FILES}
    INCLUDE_DIRS "Source" "include"
    REQUIRES TactilityKernel ili9341-module aw9523b-module lvgl-module app-module es8156-module mfrc522-module rgb-display-module
)
