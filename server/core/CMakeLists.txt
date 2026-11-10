# The game logic of the server. It never links Drogon, so that everything that decides anything
# can be tested without a socket.
add_library(whist_session STATIC
    BuildInfo.h
    BuildInfo.cpp
    Cards.h
    Cards.cpp
    Catalogue.h
    Catalogue.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/CatalogueData.cpp
    Codec.h
    Codec.cpp
    ComputerPlayer.h
    ComputerPlayer.cpp
    Connection.h
    GameAbandoned.h
    GameRun.h
    GameRun.cpp
    Joiner.h
    Joiner.cpp
    Log.h
    Log.cpp
    Names.h
    Names.cpp
    Outbox.h
    Outbox.cpp
    Pacing.h
    Pacing.cpp
    Protocol.h
    Publisher.h
    Publisher.cpp
    Registry.h
    Registry.cpp
    SeatDriver.h
    SeatDriver.cpp
    SeatProvider.h
    Seating.h
    Seating.cpp
    Service.h
    Service.cpp
    Session.h
    Session.cpp
    Settings.h
    Settings.cpp
    Table.h
    Table.cpp
    TableLobby.cpp
    Wait.h
)

# The catalogues are compiled in, so the server needs no path to them. Editing one reconfigures.
set(WHIST_CATALOGUES ${PROJECT_SOURCE_DIR}/protocol/catalogues)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${WHIST_CATALOGUES}/player_names.json ${WHIST_CATALOGUES}/avatars.json)
file(READ ${WHIST_CATALOGUES}/player_names.json WHIST_PLAYER_NAMES)
file(READ ${WHIST_CATALOGUES}/avatars.json WHIST_AVATARS)
configure_file(CatalogueData.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/CatalogueData.cpp @ONLY)

# The same jsoncpp target Drogon links, so the server never carries two copies.
find_package(jsoncpp CONFIG REQUIRED)

target_include_directories(whist_session PUBLIC ${PROJECT_SOURCE_DIR}/server)
target_compile_features(whist_session PUBLIC cxx_std_20)
target_compile_options(whist_session PRIVATE ${WHIST_SERVER_WARNINGS})

# Recorded when CMake configures, so it can lag behind a later commit until the next configure.
# A build with no git history to ask, such as the Docker image's, is told the commit instead.
set(WHIST_SERVER_COMMIT "" CACHE STRING "The commit to report, instead of asking git")
# Compared as a string: a bare if() reads a hash made only of zeros as false.
if(WHIST_SERVER_COMMIT STREQUAL "")
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE WHIST_SERVER_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

target_compile_definitions(whist_session PRIVATE WHIST_SERVER_VERSION="${PROJECT_VERSION}"
                                                 WHIST_SERVER_COMMIT="${WHIST_SERVER_COMMIT}")
target_link_libraries(whist_session PUBLIC RomanianWhist::engine jsoncpp_lib PRIVATE utf8proc)
