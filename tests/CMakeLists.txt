# M3+ adds test_SearchEngine, test_WalkScanner, test_Indexer, test_Settings,
# test_MountEnumerator, test_PathUtils, test_Elevation, and tests/mocks/ here.
add_executable(indexed_tests
    test_main.cpp
    test_Scaffold.cpp
    test_IndexPool.cpp
    test_IndexStore.cpp
    test_IndexSerializer.cpp
    test_TokenMatcher.cpp
    test_SimdSearch.cpp
    test_SearchEngine.cpp
    test_Indexer.cpp
    test_WalkScanner.cpp
    test_MountEnumerator.cpp
    test_IniFile.cpp
    test_Settings.cpp
    test_PathUtils.cpp
    test_DisplayFormat.cpp
    test_DisplayEntry.cpp
    test_StatusText.cpp
)

target_link_libraries(indexed_tests
    PRIVATE
        indexed_core
        indexed_ui_core
        GTest::gtest
        GTest::gmock
)

indexed_enable_warnings(indexed_tests)

include(GoogleTest)
gtest_discover_tests(indexed_tests DISCOVERY_TIMEOUT 30)

# ---------- QTest widget tests (headless via QT_QPA_PLATFORM=offscreen) ----------
# Qt6 imported targets found by src/ui/CMakeLists.txt aren't visible across
# sibling add_subdirectory() scopes -- re-find here. Each test class uses
# Q_OBJECT/slots, so AUTOMOC is enabled per target.
find_package(Qt6 REQUIRED COMPONENTS Test)

function(indexed_add_qtest name)
    add_executable(${name} ${name}.cpp)
    set_target_properties(${name} PROPERTIES AUTOMOC ON)
    target_link_libraries(${name} PRIVATE indexed_ui_widgets Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    indexed_enable_warnings(${name})
endfunction()

indexed_add_qtest(test_ResultModel)
indexed_add_qtest(test_ResultView)
indexed_add_qtest(test_SearchCoordinator)
indexed_add_qtest(test_SearchLineEdit)
indexed_add_qtest(test_FirstRunDialog)
indexed_add_qtest(test_SettingsDialog)
