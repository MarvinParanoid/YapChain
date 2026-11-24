# SPDX-License-Identifier: Apache-2.0
add_library(yap_test_support INTERFACE)
target_include_directories(yap_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(yap_test_support INTERFACE yap::codec yap::crypto yap::tx yap::state yap::chain yap::storage yap::wallet yap::dev)

if(YAP_BUILD_TESTS)
  include(Catch)

  add_executable(yap_unit_tests
    unit/common_test.cpp
    unit/ycs_test.cpp
    unit/hash_test.cpp
    unit/ed25519_test.cpp
    unit/bech32m_test.cpp
    unit/address_test.cpp
    unit/tx_test.cpp
    unit/validation_test.cpp
    unit/ledger_test.cpp
    unit/economics_test.cpp
    unit/merkle_test.cpp
    unit/genesis_test.cpp
    unit/engine_test.cpp
    unit/storage_test.cpp
    unit/persistent_smt_test.cpp
    unit/recovery_test.cpp
    unit/wallet_test.cpp
    unit/dev_chain_test.cpp)
  target_link_libraries(yap_unit_tests PRIVATE yap_test_support nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
  yap_target_warnings(yap_unit_tests)
  catch_discover_tests(yap_unit_tests TEST_PREFIX "unit:")

  add_executable(yap_vector_tests
    vectors/vectors_test.cpp
    vectors/tx_vectors_test.cpp
    vectors/validation_vectors_test.cpp
    vectors/economics_vectors_test.cpp
    vectors/merkle_vectors_test.cpp
    vectors/genesis_vectors_test.cpp
    vectors/blocks_vectors_test.cpp
    vectors/devblock_vectors_test.cpp)
  target_link_libraries(yap_vector_tests PRIVATE yap_test_support nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
  target_compile_definitions(yap_vector_tests PRIVATE YAP_TEST_VECTORS_DIR="${PROJECT_SOURCE_DIR}/test-vectors")
  yap_target_warnings(yap_vector_tests)
  catch_discover_tests(yap_vector_tests TEST_PREFIX "vectors:")

  # Deterministic mutation smoke run of the fuzz targets; works with GCC (no libFuzzer needed).
  add_executable(yap_fuzz_smoke fuzz/fuzz_smoke_main.cpp fuzz/fuzz_ycs.cpp fuzz/fuzz_bech32m.cpp fuzz/fuzz_tx.cpp fuzz/fuzz_devblock.cpp)
  target_compile_definitions(yap_fuzz_smoke PRIVATE YAP_FUZZ_SMOKE=1)
  target_link_libraries(yap_fuzz_smoke PRIVATE yap_test_support)
  yap_target_warnings(yap_fuzz_smoke)
  add_test(NAME fuzz_smoke COMMAND yap_fuzz_smoke 200000)

  # CLI tests drive the real `yap` binary as a subprocess.
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME cli:keys COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/cli/test_keys_cli.py $<TARGET_FILE:yap>)
  add_executable(yap_test_corrupt tools/corrupt_store.cpp)
  target_link_libraries(yap_test_corrupt PRIVATE yap_test_support)
  yap_target_warnings(yap_test_corrupt)
  add_test(NAME cli:dev COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/cli/test_dev_cli.py $<TARGET_FILE:yap> $<TARGET_FILE:yap_test_corrupt>)
  add_test(NAME e2e:workflow COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_e2e_workflow.py $<TARGET_FILE:yap>)
  add_test(NAME cli:tx COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/cli/test_tx_cli.py $<TARGET_FILE:yap>)

  if(YAP_WITH_ABCI)
    add_executable(yap_abci_tests abci/transport_test.cpp abci/application_test.cpp)
    target_include_directories(yap_abci_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/abci)
    target_link_libraries(yap_abci_tests PRIVATE yap_test_support yap::abci yap::abci_app nlohmann_json::nlohmann_json
      Catch2::Catch2WithMain)
    target_compile_definitions(yap_abci_tests PRIVATE YAP_PROTO_DIR="${PROJECT_SOURCE_DIR}/third_party/cometbft-proto")
    yap_target_warnings(yap_abci_tests)
    catch_discover_tests(yap_abci_tests TEST_PREFIX "abci:")
    add_executable(yap_abci_fuzz_smoke fuzz/fuzz_abci_smoke_main.cpp fuzz/fuzz_abci.cpp)
    target_compile_definitions(yap_abci_fuzz_smoke PRIVATE YAP_FUZZ_SMOKE=1)
    target_link_libraries(yap_abci_fuzz_smoke PRIVATE yap::abci)
    yap_target_warnings(yap_abci_fuzz_smoke)
    add_test(NAME fuzz_smoke_abci COMMAND yap_abci_fuzz_smoke 100000)
    # The production yapd has no crash hooks; only yapd-test-hooks does (M2.4f).
    add_test(NAME node:no_test_hooks_in_yapd
      COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/node/test_yapd_hooks.py
              $<TARGET_FILE:yapd> $<TARGET_FILE:yapd_test_hooks>)

    # Live test with a real CometBFT v0.40.0 (built from the verified source archive by
    # scripts/localnet/localnet.py build-cometbft). Registered only when the binary is given.
    set(YAP_COMETBFT_BIN "" CACHE FILEPATH "CometBFT v0.40.0 binary for the live integration test")
    if(YAP_COMETBFT_BIN)
      add_test(NAME integration:live_cometbft
        COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_live_cometbft.py
                $<TARGET_FILE:yap> $<TARGET_FILE:yapd> ${YAP_COMETBFT_BIN} $<TARGET_FILE:yapd_test_hooks>)
      set_tests_properties(integration:live_cometbft PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)
      # Four independent validators (M2.4): consensus, distributed transactions, rewards, blocksync.
      add_test(NAME integration:live_multival
        COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_live_multival.py
                $<TARGET_FILE:yap> $<TARGET_FILE:yapd> ${YAP_COMETBFT_BIN})
      set_tests_properties(integration:live_multival PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE)
      # Fault tolerance of the four-validator network through the netfault P2P proxy (M2.5).
      set(YAP_FAULT_SCENARIOS infra quorum partition)
      foreach(scenario IN LISTS YAP_FAULT_SCENARIOS)
        add_test(NAME integration:fault_${scenario}
          COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_live_faults.py ${scenario}
                  $<TARGET_FILE:yap> $<TARGET_FILE:yapd> ${YAP_COMETBFT_BIN} $<TARGET_FILE:yapd_test_hooks>)
        set_tests_properties(integration:fault_${scenario} PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE LABELS "live;fault")
      endforeach()
    endif()
  endif()

  # Crash-recovery tests fork child processes that are killed with SIGKILL.
  add_executable(yap_crash_tests crash/crash_recovery_test.cpp)
  target_link_libraries(yap_crash_tests PRIVATE yap_test_support Catch2::Catch2WithMain)
  yap_target_warnings(yap_crash_tests)
  catch_discover_tests(yap_crash_tests TEST_PREFIX "crash:" PROPERTIES TIMEOUT 1800)

  # Benchmark (not a test): full SMT recomputation vs incremental persistent updates.
  add_executable(yap_smt_bench bench/smt_bench.cpp)
  target_link_libraries(yap_smt_bench PRIVATE yap_test_support)
  yap_target_warnings(yap_smt_bench)
endif()

if(YAP_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "YAP_FUZZ requires Clang (libFuzzer)")
  endif()
  set(_yap_fuzz_targets ycs bech32m tx devblock)
  if(YAP_WITH_ABCI)
    list(APPEND _yap_fuzz_targets abci)
  endif()
  foreach(target IN LISTS _yap_fuzz_targets)
    add_executable(fuzz_${target} fuzz/fuzz_${target}.cpp)
    target_link_libraries(fuzz_${target} PRIVATE yap_test_support)
    if(target STREQUAL "abci")
      target_link_libraries(fuzz_${target} PRIVATE yap::abci)
    endif()
    target_compile_options(fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    yap_target_warnings(fuzz_${target})
  endforeach()
endif()
