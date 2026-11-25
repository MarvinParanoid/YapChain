# SPDX-License-Identifier: Apache-2.0
# CometBFT RPC client over the system libcurl (ADR-0029).
add_library(yap_rpc STATIC http.cpp comet.cpp)
add_library(yap::rpc ALIAS yap_rpc)
target_include_directories(yap_rpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(yap_rpc PUBLIC yap::common PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)
yap_target_warnings(yap_rpc)
