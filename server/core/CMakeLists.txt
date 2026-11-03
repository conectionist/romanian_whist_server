# The game logic of the server. It never links Drogon, so that everything that decides anything
# can be tested without a socket.
add_library(whist_session STATIC
    BuildInfo.h
    BuildInfo.cpp
)

target_include_directories(whist_session PUBLIC ${PROJECT_SOURCE_DIR}/server)
target_compile_features(whist_session PUBLIC cxx_std_20)
target_compile_options(whist_session PRIVATE ${WHIST_SERVER_WARNINGS})
target_compile_definitions(whist_session PRIVATE WHIST_SERVER_VERSION="${PROJECT_VERSION}")
target_link_libraries(whist_session PUBLIC RomanianWhist::engine)
