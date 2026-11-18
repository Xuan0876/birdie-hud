add_executable(birdie_unit unit_main.cpp)
target_link_libraries(birdie_unit PRIVATE birdie_core)
add_test(NAME unit COMMAND birdie_unit)

add_executable(birdie_panel_unit panel_main.cpp)
target_link_libraries(birdie_panel_unit PRIVATE birdie_panel)
add_test(NAME panel COMMAND birdie_panel_unit)
