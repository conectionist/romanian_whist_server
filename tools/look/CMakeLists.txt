# The look fixtures: the states the browser page draws its tables from, written to protocol/look/.
# They are built from the core alone, so this needs no Drogon.
add_library(whist_look_fixtures STATIC
    DesktopRules.h
    DesktopRules.cpp
)
target_include_directories(whist_look_fixtures PUBLIC ${PROJECT_SOURCE_DIR}/tools)
target_compile_options(whist_look_fixtures PRIVATE ${WHIST_SERVER_WARNINGS})
target_link_libraries(whist_look_fixtures PUBLIC whist_session)
