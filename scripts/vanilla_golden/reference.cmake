add_executable(vanilla-golden-reference EXCLUDE_FROM_ALL
  "${VANILLA_GOLDEN_OVERLAY}/reference_runner.cpp"
  ${GAME_SERVER}
  ${GAME_GENERATED_SERVER}
  $<TARGET_OBJECTS:engine-shared>
  $<TARGET_OBJECTS:game-shared>
  ${DEPS}
)
target_link_libraries(vanilla-golden-reference ${LIBS_SERVER})
target_include_directories(vanilla-golden-reference PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/src"
)
