# Link State Fabric test suite.
#
# Every test is a plain executable registered with CTest. No test uses a
# timeout: a hanging test is a runtime defect, not something to mask.

function(lsf_add_test name)
  add_executable(${name} test_main.cpp ${ARGN})
  target_link_libraries(${name} PRIVATE SummonSoftwareLabs::LinkStateFabric)
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  lsf_apply_warnings(${name})
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

lsf_add_test(lsf_test_identity_state test_identity_state.cpp)
lsf_add_test(lsf_test_engine test_engine.cpp)
lsf_add_test(lsf_test_resolution test_resolution.cpp)
lsf_add_test(lsf_test_persistence test_persistence.cpp journal_builder.cpp)
lsf_add_test(lsf_test_snapshot_scale test_snapshot_scale.cpp)
lsf_add_test(lsf_test_concurrency test_concurrency.cpp)
lsf_add_test(lsf_test_adversarial test_adversarial.cpp)
lsf_add_test(lsf_test_distributed test_distributed.cpp)

if(LSF_BUILD_TOOLS)
  lsf_add_test(lsf_test_multiprocess test_multiprocess.cpp)
  target_compile_definitions(lsf_test_multiprocess PRIVATE
    LSF_COORDINATOR_EXE="$<TARGET_FILE:lsf_coordinator>"
    LSF_WORKER_EXE="$<TARGET_FILE:lsf_worker>")
  add_dependencies(lsf_test_multiprocess lsf_coordinator lsf_worker)
endif()
