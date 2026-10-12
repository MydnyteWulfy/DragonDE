# A tiny wl_shm client for poking at the compositor by hand.
add_executable(dragon-demo-client main.cpp)
target_link_libraries(dragon-demo-client PRIVATE dragon_client_protocols dragon::options)
