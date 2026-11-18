add_executable(birdie_parity
  fixture.cpp
  png.cpp
  parity_main.cpp
)
target_link_libraries(birdie_parity PRIVATE birdie_core birdie_render nlohmann_json)
target_compile_definitions(birdie_parity PRIVATE
  BIRDIE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
  BIRDIE_PARITY_OUT="${CMAKE_CURRENT_BINARY_DIR}/out")

add_test(NAME parity COMMAND birdie_parity)

# birdie_shot: any settings / animation frame of a fixture as a PNG (tools/hud_shots.py)
add_executable(birdie_shot
  fixture.cpp
  png.cpp
  shot_main.cpp
)
target_link_libraries(birdie_shot PRIVATE birdie_core birdie_render nlohmann_json)
