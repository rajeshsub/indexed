# M2+ adds test_SearchEngine, test_TokenMatcher, test_WalkScanner, test_Indexer,
# test_Settings, test_MountEnumerator, test_PathUtils, test_Elevation, and tests/mocks/ here.
add_executable(indexed_tests
    test_main.cpp
    test_Scaffold.cpp
    test_IndexPool.cpp
    test_IndexStore.cpp
    test_IndexSerializer.cpp
)

target_link_libraries(indexed_tests
    PRIVATE
        indexed_core
        GTest::gtest
        GTest::gmock
)

indexed_enable_warnings(indexed_tests)

include(GoogleTest)
gtest_discover_tests(indexed_tests DISCOVERY_TIMEOUT 30)
