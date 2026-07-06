# M4 adds MainWindow, resources.qrc to the `indexed` executable below, and
# ResultModel, ResultView, SearchLineEdit, FirstRunDialog, SettingsDialog to
# the indexed_ui_widgets library below.
find_package(Qt6 REQUIRED COMPONENTS Widgets DBus Test)

qt_standard_project_setup()

# Qt-free presentation logic (DisplayEntry/DisplayFormat and friends), split
# out so it's unit-testable with plain gtest -- no QApplication required --
# and so tests/ can link it without pulling in Qt widgets.
add_library(indexed_ui_core STATIC
    DisplayEntry.h
    DisplayEntry.cpp
    DisplayFormat.h
    DisplayFormat.cpp
)

target_include_directories(indexed_ui_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(indexed_ui_core
    PUBLIC indexed_core
)

indexed_enable_warnings(indexed_ui_core)

# Qt widget classes (ResultModel, ResultView, SearchLineEdit, FirstRunDialog,
# SettingsDialog -- everything except MainWindow/main.cpp). Split into its own
# library, same rationale as indexed_ui_core, so each widget's QTest binary
# under tests/ links this instead of duplicating the widget .cpp into every
# test executable.
add_library(indexed_ui_widgets STATIC
)

target_include_directories(indexed_ui_widgets
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(indexed_ui_widgets
    PUBLIC
        indexed_core
        indexed_ui_core
        Qt6::Widgets
        Qt6::DBus
)

indexed_enable_warnings(indexed_ui_widgets)

add_executable(indexed
    main.cpp
)

target_link_libraries(indexed
    PRIVATE
        indexed_core
        indexed_ui_core
        indexed_ui_widgets
        Qt6::Widgets
        Qt6::DBus
)

indexed_enable_warnings(indexed)

install(TARGETS indexed DESTINATION bin)
