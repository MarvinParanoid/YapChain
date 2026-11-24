# SPDX-License-Identifier: Apache-2.0
add_executable(yapd main.cpp)
target_link_libraries(yapd PRIVATE yap::abci_app yap::storage yap::cli11)
target_compile_definitions(yapd PRIVATE YAP_VERSION="${PROJECT_VERSION}")
yap_target_warnings(yapd)
