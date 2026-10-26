add_executable(irdiff-tests
  unit/Main.cpp
  unit/CoreTest.cpp
  unit/DiffTest.cpp
  unit/InputTest.cpp
  unit/AnalysisTest.cpp
  unit/PipelineTest.cpp
  unit/RealTest.cpp
  unit/CliTest.cpp)
target_link_libraries(irdiff-tests PRIVATE irdiff_core)
target_compile_definitions(irdiff-tests PRIVATE
  IRDIFF_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  IRDIFF_BINARY="$<TARGET_FILE:irdiff>")
add_dependencies(irdiff-tests irdiff)

foreach(suite core diff input analysis pipeline real cli)
  add_test(NAME ${suite} COMMAND irdiff-tests ${suite})
  set_tests_properties(${suite} PROPERTIES ENVIRONMENT "NO_COLOR=1")
endforeach()
