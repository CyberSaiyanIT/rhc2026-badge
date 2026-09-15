# The two ESP-ADF stream elements the MusicPlayer app uses, as their own component. ADF's own
# audio_stream also builds the HTTP/I2S/tone/AEC elements, dragging in audio_board and esp-sr;
# fatfs_stream.c and raw_stream.c need neither.

set(ADF_AUDIO_STREAM_DIR "${CMAKE_CURRENT_LIST_DIR}/../esp-adf/components/audio_stream")

idf_component_register(
    SRCS
        "${ADF_AUDIO_STREAM_DIR}/fatfs_stream.c"
        "${ADF_AUDIO_STREAM_DIR}/raw_stream.c"
    INCLUDE_DIRS "${ADF_AUDIO_STREAM_DIR}/include"
    REQUIRES audio_pipeline audio_sal esp-adf-libs
)
