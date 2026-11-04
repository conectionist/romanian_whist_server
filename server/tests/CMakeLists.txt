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
    core/support/Client.h
    core/support/Client.cpp
    core/support/Fixtures.h
    core/support/Fixtures.cpp
    core/support/Games.h
    core/support/Games.cpp
    core/support/Oracle.h
    core/support/Oracle.cpp
    core/support/Recorder.h
    core/support/Recorder.cpp
    core/support/ScriptedHuman.h
    core/support/ScriptedHuman.cpp
    core/support/ServerMessages.h
    core/support/ServerMessages.cpp
    core/CardsTests.cpp
    core/CatalogueTests.cpp
    core/CodecTests.cpp
    core/EngineTests.cpp
    core/FixtureTests.cpp
    core/GameRunTests.cpp
    core/HoldTests.cpp
    core/LeakScanTests.cpp
    core/OnlineTests.cpp
    core/NamesTests.cpp
    core/OutboxTests.cpp
    core/SeatDriverTests.cpp
    core/SeatingTests.cpp
    core/SessionTests.cpp
    core/SettingsTests.cpp
    core/SoloTests.cpp
    core/TableTests.cpp
)
target_include_directories(core_tests PRIVATE core)
target_compile_options(core_tests PRIVATE ${WHIST_SERVER_WARNINGS})
target_compile_definitions(core_tests PRIVATE WHIST_PROTOCOL_DIR="${PROJECT_SOURCE_DIR}/protocol")
target_link_libraries(core_tests PRIVATE whist_session Catch2::Catch2WithMain)
catch_discover_tests(core_tests)

# Hands reach the server only through the engine's viewer-filtered snapshot and the move contexts,
# so no server code reads one directly.
add_test(NAME no_direct_hand_reads
         COMMAND sh -c "! grep -rn 'getHand[(]' '${PROJECT_SOURCE_DIR}/server'")

add_executable(app_tests
    app/TestSupport.h
    app/TestSupport.cpp
    app/SpikeController.h
    app/SpikeController.cpp
    app/GameTests.cpp
    app/OnlineTests.cpp
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
