# M5 adds the real indexer/fanotify-monitor orchestration, signal handling
# (SIGHUP/SIGUSR1/SIGTERM), and PKEXEC_UID-based root-write hardening here.
add_executable(indexed-helper
    main_helper.cpp
)

target_link_libraries(indexed-helper
    PRIVATE
        indexed_core
)

indexed_enable_warnings(indexed-helper)

install(TARGETS indexed-helper DESTINATION bin)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/polkit/org.indexed.helper.policy
       DESTINATION share/polkit-1/actions)
