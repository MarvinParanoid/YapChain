add_executable(yap main.cpp context.cpp keys_cmd.cpp local_chain.cpp tx_cmd.cpp genesis_cmd.cpp dev_cmd.cpp query_cmd.cpp)
target_link_libraries(yap PRIVATE yap::chain yap::dev yap::wallet yap::cli11 yap::sodium)
if(YAP_WITH_NETWORK)
  # Network mode (M2.6): yap network ... over the CometBFT RPC client.
  target_sources(yap PRIVATE network_config.cpp network_cmd.cpp)
  target_link_libraries(yap PRIVATE yap::rpc nlohmann_json::nlohmann_json)
  target_compile_definitions(yap PRIVATE YAP_WITH_NETWORK=1)
endif()
yap_target_warnings(yap)
