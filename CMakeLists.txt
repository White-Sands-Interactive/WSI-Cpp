cmake_minimum_required(VERSION 3.16)

project(wsi-agent-player VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE AND EXISTS "/opt/homebrew/opt/openssl")
    set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl")
endif()
if(APPLE AND EXISTS "/opt/homebrew/opt/sqlite")
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/sqlite")
endif()

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 UNKNOWN IMPORTED)
    set_target_properties(SQLite3::SQLite3 PROPERTIES
        IMPORTED_LOCATION "${SQLITE3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SQLITE3_INCLUDE_DIR}"
    )
endif()
find_package(OpenSSL REQUIRED)

add_library(wsi_agent_player
    src/agent_server.cpp
    src/mcp.cpp
    src/memory.cpp
    src/platform_connection.cpp
    src/player.cpp
    src/schema.cpp
    src/session_loop.cpp
    src/testing.cpp
    src/turn_gate.cpp
    src/websocket.cpp
)

target_include_directories(wsi_agent_player
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(wsi_agent_player
    PUBLIC
        nlohmann_json::nlohmann_json
        Threads::Threads
        SQLite3::SQLite3
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_compile_options(wsi_agent_player PRIVATE -Wall -Wextra -Wpedantic)

if(NOT WSI_EMBEDDED)
add_executable(wsi_agent_player_tests tests/agent_player_test.cpp)
target_link_libraries(wsi_agent_player_tests PRIVATE wsi_agent_player)
target_include_directories(wsi_agent_player_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(wsi_agent_player_tests PRIVATE -Wall -Wextra -Wpedantic)

add_executable(wsi_forest examples/forest.cpp)
target_link_libraries(wsi_forest PRIVATE wsi_agent_player)

add_executable(wsi_courier examples/courier.cpp)
target_link_libraries(wsi_courier PRIVATE wsi_agent_player)

add_executable(wsi_ferry examples/ferry.cpp)
target_link_libraries(wsi_ferry PRIVATE wsi_agent_player)

enable_testing()
add_test(NAME agent_player COMMAND wsi_agent_player_tests)
add_test(NAME forest COMMAND wsi_forest)
add_test(NAME courier COMMAND wsi_courier)
add_test(NAME ferry COMMAND wsi_ferry)
endif()

install(TARGETS wsi_agent_player ARCHIVE)
install(DIRECTORY include/wsi DESTINATION include)
