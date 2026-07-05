# M2+ adds search/, settings/, platform/ subdirectory sources here.
add_library(indexed_core STATIC
    Version.h
    Version.cpp
    indexer/IFileSystemScanner.h
    storage/EntryMeta.h
    storage/IndexPool.h
    storage/IndexPool.cpp
)

target_include_directories(indexed_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(indexed_core
    PUBLIC
        re2::re2
        utf8proc
)

indexed_enable_warnings(indexed_core)
