# SPDX-License-Identifier: Apache-2.0
# Transport (framing + Unix socket server): depends only on the generated protobuf bindings.
add_library(yap_abci STATIC framing.cpp server.cpp)
add_library(yap::abci ALIAS yap_abci)
target_include_directories(yap_abci PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(yap_abci PUBLIC yap::abci_proto Threads::Threads)
yap_target_warnings(yap_abci)

# ABCI++ application over the deterministic engine.
add_library(yap_abci_app STATIC convert.cpp application.cpp)
add_library(yap::abci_app ALIAS yap_abci_app)
target_link_libraries(yap_abci_app PUBLIC yap::abci yap::chain PRIVATE yap::dev nlohmann_json::nlohmann_json)
yap_target_warnings(yap_abci_app)
