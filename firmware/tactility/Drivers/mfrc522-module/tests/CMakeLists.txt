project(Mfrc522ModuleTests)

enable_language(C CXX)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/source/*.cpp)

# The parser source is compiled in directly rather than linked from mfrc522-module: the module
# requires platform-esp32, while ndef.cpp itself depends on nothing and builds anywhere.
add_executable(Mfrc522ModuleTests EXCLUDE_FROM_ALL
    ${TEST_SOURCES}
    ${PROJECT_SOURCE_DIR}/../source/ndef.cpp
)

target_include_directories(Mfrc522ModuleTests PRIVATE
    ${DOCTESTINC}
    ${PROJECT_SOURCE_DIR}/../include
)

add_test(NAME Mfrc522ModuleTests COMMAND Mfrc522ModuleTests)
