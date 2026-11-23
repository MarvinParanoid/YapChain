add_executable(yap main.cpp context.cpp keys_cmd.cpp)
target_link_libraries(yap PRIVATE yap::chain yap::wallet yap::cli11)
yap_target_warnings(yap)
