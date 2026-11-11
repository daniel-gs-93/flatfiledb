cmake_minimum_required(VERSION 3.22)
project(flatfiledb
        VERSION 0.1.0
        DESCRIPTION "FlatFileDB: a SQL terminal over swappable storage engines"
        LANGUAGES CXX
        )

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS "-Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "-g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

find_package(Threads REQUIRED)

# Domain, Application, Infrastructure, and the SQL parser (Adapter), minus
# main(), as one library so the test executable links it without recompiling
# or depending on cli.cpp.
add_library(flatfiledb_core STATIC
        src/application.cpp
        src/binder.cpp
        src/bootstrap.cpp
        src/csv_codec.cpp
        src/directory_lock.cpp
        src/expression.cpp
        src/flat_file_engine.cpp
        src/flat_file_executor.cpp
        src/infrastructure.cpp
        src/sql_parser.cpp
        src/value.cpp
        )
target_include_directories(flatfiledb_core PUBLIC include)
target_compile_features(flatfiledb_core PUBLIC cxx_std_20)
target_link_libraries(flatfiledb_core PUBLIC Threads::Threads)

add_executable(flatfiledb_cli src/cli.cpp)
target_link_libraries(flatfiledb_cli PRIVATE flatfiledb_core)

enable_testing()
add_executable(flatfiledb_tests
        tests/test_main.cpp
        tests/test_support.cpp
        tests/test_binder.cpp
        tests/test_bootstrap.cpp
        tests/test_cli.cpp
        tests/test_csv_codec.cpp
        tests/test_csv_repository.cpp
        tests/test_directory_lock.cpp
        tests/test_expression.cpp
        tests/test_flat_file_engine.cpp
        tests/test_layering.cpp
        tests/test_query_service.cpp
        tests/test_sql_parser.cpp
        )
target_include_directories(flatfiledb_tests PRIVATE tests)
target_link_libraries(flatfiledb_tests PRIVATE flatfiledb_core)
target_compile_definitions(flatfiledb_tests PRIVATE
        FLATFILEDB_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        FLATFILEDB_CLI_PATH="$<TARGET_FILE:flatfiledb_cli>")
add_dependencies(flatfiledb_tests flatfiledb_cli)
add_test(NAME flatfiledb_tests COMMAND flatfiledb_tests)
