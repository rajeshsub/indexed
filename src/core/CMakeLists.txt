# M3+ adds settings/, platform/ subdirectory sources here.
add_library(indexed_core STATIC
    Version.h
    Version.cpp
    indexer/IFileSystemScanner.h
    indexer/IChangeMonitor.h
    storage/EntryMeta.h
    storage/IndexPool.h
    storage/IndexPool.cpp
    storage/IndexStore.h
    storage/IndexStore.cpp
    storage/Crc32.h
    storage/Crc32.cpp
    storage/IndexSerializer.h
    storage/IndexSerializer.cpp
    search/ISearchEngine.h
    search/TokenMatcher.h
    search/TokenMatcher.cpp
    search/SimdSearch.h
    search/SimdSearch.cpp
    search/SimdSearchSse42.cpp
    search/SimdSearchAvx2.cpp
    search/SearchEngine.h
    search/SearchEngine.cpp
)

set_source_files_properties(search/SimdSearchSse42.cpp PROPERTIES COMPILE_OPTIONS "-msse4.2")
set_source_files_properties(search/SimdSearchAvx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")

target_include_directories(indexed_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(indexed_core
    PUBLIC
        re2::re2
        utf8proc
)

indexed_enable_warnings(indexed_core)
