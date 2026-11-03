# Everything but main(), so the tests start the same server main() does.
add_library(whist_app STATIC
    ServerOptions.h
    ServerOptions.cpp
    Server.h
    Server.cpp
    EchoController.h
    EchoController.cpp
)

target_include_directories(whist_app PUBLIC ${PROJECT_SOURCE_DIR}/server)
target_compile_features(whist_app PUBLIC cxx_std_20)
target_compile_options(whist_app PRIVATE ${WHIST_SERVER_WARNINGS})
target_link_libraries(whist_app PUBLIC whist_session Drogon::Drogon)

add_executable(whist_server main.cpp)
target_compile_options(whist_server PRIVATE ${WHIST_SERVER_WARNINGS})
target_link_libraries(whist_server PRIVATE whist_app)
