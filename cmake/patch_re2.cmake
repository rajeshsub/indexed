file(READ "CMakeLists.txt" _content)
string(REGEX REPLACE "install\\(EXPORT re2Targets[^)]*\\)" "" _content "${_content}")
file(WRITE "CMakeLists.txt" "${_content}")
