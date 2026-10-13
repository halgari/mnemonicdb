cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME mnemonicdb)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# RocksDB, vendored and statically linked into both extension targets
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_ALL_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_CORE_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_TRACE_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
set(WITH_LIBURING OFF CACHE BOOL "" FORCE)
set(WITH_JEMALLOC OFF CACHE BOOL "" FORCE)
set(WITH_SNAPPY OFF CACHE BOOL "" FORCE)
set(WITH_LZ4 OFF CACHE BOOL "" FORCE)
set(WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(WITH_ZSTD OFF CACHE BOOL "" FORCE)
set(FAIL_ON_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_RTTI ON CACHE STRING "" FORCE)
set(PORTABLE 1 CACHE STRING "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/rocksdb ${CMAKE_CURRENT_BINARY_DIR}/rocksdb
                 EXCLUDE_FROM_ALL)

include_directories(src/include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rocksdb/include)

set(EXTENSION_SOURCES
    src/mnemonicdb_extension.cpp
    src/store/store.cpp
    src/catalog/mnemonic_catalog.cpp
    src/catalog/mnemonic_schema_entry.cpp
    src/txn/mnemonic_transaction.cpp
    src/txn/mnemonic_transaction_manager.cpp
    src/mnemonicdb_info.cpp
    src/codec_functions.cpp
    src/codec/keys.cpp
    src/codec/value_codec.cpp
    src/codec/tempid.cpp
    src/schema/schema.cpp
    src/write/tx_writer.cpp
    src/catalog/mnemonic_table_entry.cpp
    src/exec/mnemonic_scan.cpp
    src/exec/mnemonic_insert.cpp
    src/exec/mnemonic_modify.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# RocksDB headers require C++20
set_target_properties(rocksdb ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_link_libraries(${EXTENSION_NAME} rocksdb)
target_link_libraries(${LOADABLE_EXTENSION_NAME} rocksdb)

install(
  TARGETS ${EXTENSION_NAME} rocksdb
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
