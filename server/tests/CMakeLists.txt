# Catch2 v3.16.0, the tag the engine and the desktop client pin.
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.16.0
    SYSTEM
)
FetchContent_MakeAvailable(Catch2)
include(Catch)

# The core's tests link no Drogon, as the core itself does not.
add_executable(core_tests
    core/EngineTests.cpp
)
target_compile_options(core_tests PRIVATE ${WHIST_SERVER_WARNINGS})
target_link_libraries(core_tests PRIVATE whist_session Catch2::Catch2WithMain)
catch_discover_tests(core_tests)

add_executable(app_tests
    app/TestSupport.h
    app/TestSupport.cpp
    app/SpikeController.h
    app/SpikeController.cpp
    app/OptionsTests.cpp
    app/ServerTests.cpp
    app/SpikeTests.cpp
    app/ShutdownTests.cpp
)
target_compile_options(app_tests PRIVATE ${WHIST_SERVER_WARNINGS})
target_link_libraries(app_tests PRIVATE whist_app Catch2::Catch2WithMain)
target_compile_definitions(app_tests PRIVATE WHIST_SERVER_PATH="$<TARGET_FILE:whist_server>")
add_dependencies(app_tests whist_server)
catch_discover_tests(app_tests)
