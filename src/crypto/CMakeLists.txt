add_library(yap_crypto STATIC
  sodium.cpp
  hash.cpp
  ed25519.cpp
  bech32m.cpp
  address.cpp)
add_library(yap::crypto ALIAS yap_crypto)
target_include_directories(yap_crypto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(yap_crypto PUBLIC yap::common PRIVATE yap::sodium)
yap_target_warnings(yap_crypto)
