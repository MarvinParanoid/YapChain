# SPDX-License-Identifier: Apache-2.0
add_executable(yapd main.cpp)
target_link_libraries(yapd PRIVATE yap::abci_app yap::storage yap::cli11)
target_compile_definitions(yapd PRIVATE YAP_VERSION="${PROJECT_VERSION}")
yap_target_warnings(yapd)

# Test-only build of the same daemon with crash hooks (YAPD_TEST_CRASH), used by the live
# crash-recovery tests. Built only together with the tests; the yapd above never contains them.
if(YAP_BUILD_TESTS)
  add_executable(yapd_test_hooks main.cpp)
  set_target_properties(yapd_test_hooks PROPERTIES OUTPUT_NAME yapd-test-hooks)
  target_link_libraries(yapd_test_hooks PRIVATE yap::abci_app yap::storage yap::cli11)
  target_compile_definitions(yapd_test_hooks PRIVATE YAP_VERSION="${PROJECT_VERSION}" YAPD_TEST_HOOKS=1)
  yap_target_warnings(yapd_test_hooks)
endif()

# ABCI readiness/health probe for containers (M2.7): plain POSIX, no protobuf, so the same binary is
# also copied into the CometBFT image.
add_executable(yap_abci_probe abci_probe.cpp)
set_target_properties(yap_abci_probe PROPERTIES OUTPUT_NAME yap-abci-probe)
yap_target_warnings(yap_abci_probe)
