find_package(pugixml REQUIRED)

add_executable(dragon-scanner main.cpp)
target_link_libraries(dragon-scanner PRIVATE pugixml::pugixml dragon::options)
