# M4 adds MainWindow, ResultModel, ResultView, SearchLineEdit, FirstRunDialog,
# SettingsDialog, resources.qrc here.
find_package(Qt6 REQUIRED COMPONENTS Widgets DBus)

qt_standard_project_setup()

add_executable(indexed
    main.cpp
)

target_link_libraries(indexed
    PRIVATE
        indexed_core
        Qt6::Widgets
        Qt6::DBus
)

indexed_enable_warnings(indexed)

install(TARGETS indexed DESTINATION bin)
