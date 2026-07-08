# popyd tests
#
# Tiny zero-dependency assertion harness: each test_* binary returns 0 on
# success, prints "FAIL: ..." and returns 1 on failure. ctest reports them.
#
# We deliberately avoid GoogleTest/Catch2 — adds dependencies, build time, and
# binary size for what is at most a few hundred assertions.

add_library(popy_test_main STATIC test_main.cpp)
target_include_directories(popy_test_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(popy_test_main PUBLIC popy_warnings)

function(popy_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE popy_core popy_test_main)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

popy_add_test(test_naming)
popy_add_test(test_sidecar)
popy_add_test(test_hash)
popy_add_test(test_hmac)
popy_add_test(test_keyring)
popy_add_test(test_attest)
popy_add_test(test_quarantine_file)
popy_add_test(test_repair)
popy_add_test(test_textsafe)
popy_add_test(test_verified_read)
popy_add_test(test_release)
popy_add_test(test_config)
popy_add_test(test_safe_fs)
popy_add_test(test_e2e_fetch)

# test_watcher needs the platform watcher implementation linked in directly
# (the watcher .cpp lives in popyd's executable, not popy_core).
set(POPY_TEST_WATCHER_SRC)
if(APPLE)
  list(APPEND POPY_TEST_WATCHER_SRC ../src/watch/watcher_fsevents.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND POPY_TEST_WATCHER_SRC ../src/watch/watcher_inotify.cpp)
endif()
add_executable(test_watcher test_watcher.cpp ${POPY_TEST_WATCHER_SRC})
target_link_libraries(test_watcher PRIVATE popy_core popy_test_main)
if(APPLE)
  target_link_libraries(test_watcher PRIVATE
    "-framework CoreServices" "-framework CoreFoundation")
endif()
add_test(NAME test_watcher COMMAND test_watcher)
set_tests_properties(test_watcher PROPERTIES TIMEOUT 30)
