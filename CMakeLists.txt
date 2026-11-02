# 3.25 for add_subdirectory(SYSTEM), which keeps Drogon's headers out of our warnings.
cmake_minimum_required(VERSION 3.25)

project(romanian_whist_web
    VERSION 0.1.0
    DESCRIPTION "Romanian Whist in the browser: the game server and its page"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Submodules
# ---------------------------------------------------------------------------
foreach(submodule RomanianWhistEngine drogon)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/${submodule}/CMakeLists.txt")
        message(FATAL_ERROR
            "The ${submodule} submodule is missing.\n"
            "Run: git submodule update --init --recursive")
    endif()
endforeach()

# The engine's own tests are off here, because it is not the top-level project.
add_subdirectory(libs/RomanianWhistEngine)

# Drogon with only the parts the server uses, set as plain variables. Trantor's older
# cmake_minimum_required would have option() clear them, hence the policy default.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_CTL OFF)
set(BUILD_EXAMPLES OFF)
set(BUILD_ORM OFF)
set(BUILD_BROTLI OFF)
set(BUILD_YAML_CONFIG OFF)
set(BUILD_DOC OFF)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
set(BUILD_C-ARES OFF)
set(USE_SUBMODULE ON)
# A cache entry, because trantor declares this one as such and would drop a plain variable.
set(TRANTOR_USE_TLS openssl CACHE STRING "The TLS library trantor uses")
add_subdirectory(libs/drogon SYSTEM)

# ---------------------------------------------------------------------------
# Warnings, on our targets only
# ---------------------------------------------------------------------------
option(WHIST_WEB_WARNINGS_AS_ERRORS "Treat warnings in our own code as errors" OFF)

set(WHIST_WEB_WARNINGS -Wall -Wextra -Wpedantic -Wshadow)
if(WHIST_WEB_WARNINGS_AS_ERRORS)
    list(APPEND WHIST_WEB_WARNINGS -Werror)
endif()

# ---------------------------------------------------------------------------
# The server
# ---------------------------------------------------------------------------
add_subdirectory(server/core)
add_subdirectory(server/app)

option(WHIST_WEB_BUILD_TESTS "Build the server's tests" ${PROJECT_IS_TOP_LEVEL})

if(WHIST_WEB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(server/tests)
endif()
