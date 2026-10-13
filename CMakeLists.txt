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
set(WITH_LZ4 ON CACHE BOOL "" FORCE)
set(WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(WITH_ZSTD ON CACHE BOOL "" FORCE)
set(FAIL_ON_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_RTTI ON CACHE STRING "" FORCE)
set(PORTABLE 1 CACHE STRING "" FORCE)
# On MSVC RocksDB would take compression libraries from hardcoded NuGet paths; they are wired in below instead
set(ROCKSDB_SKIP_THIRDPARTY ON CACHE BOOL "" FORCE)
# Compression libraries, vendored and built static so the extension is self-contained. RocksDB's find modules
# are pointed at them through their cache variables and the imported target names they would create.
set(MNEMONICDB_LZ4_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4/lib)
add_library(mnemonicdb_lz4 STATIC ${MNEMONICDB_LZ4_DIR}/lz4.c ${MNEMONICDB_LZ4_DIR}/lz4hc.c
                                  ${MNEMONICDB_LZ4_DIR}/lz4frame.c ${MNEMONICDB_LZ4_DIR}/xxhash.c)
target_include_directories(mnemonicdb_lz4 PUBLIC $<BUILD_INTERFACE:${MNEMONICDB_LZ4_DIR}>)
# lz4's xxhash must not clash with other copies linked into DuckDB
target_compile_definitions(mnemonicdb_lz4 PUBLIC XXH_NAMESPACE=MNEMONICDB_LZ4_)
set(MNEMONICDB_ZSTD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib)
file(GLOB MNEMONICDB_ZSTD_SOURCES ${MNEMONICDB_ZSTD_DIR}/common/*.c ${MNEMONICDB_ZSTD_DIR}/compress/*.c
     ${MNEMONICDB_ZSTD_DIR}/decompress/*.c ${MNEMONICDB_ZSTD_DIR}/dictBuilder/*.c)
add_library(mnemonicdb_zstd STATIC ${MNEMONICDB_ZSTD_SOURCES})
target_include_directories(mnemonicdb_zstd PUBLIC $<BUILD_INTERFACE:${MNEMONICDB_ZSTD_DIR}>)
target_compile_definitions(mnemonicdb_zstd PRIVATE ZSTD_DISABLE_ASM)
set_target_properties(mnemonicdb_lz4 mnemonicdb_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET
                                                                hidden)
set(lz4_INCLUDE_DIRS ${MNEMONICDB_LZ4_DIR} CACHE PATH "" FORCE)
set(lz4_LIBRARIES mnemonicdb_lz4 CACHE STRING "" FORCE)
set(ZSTD_INCLUDE_DIRS ${MNEMONICDB_ZSTD_DIR} CACHE PATH "" FORCE)
set(ZSTD_LIBRARIES mnemonicdb_zstd CACHE STRING "" FORCE)
add_library(lz4::lz4 ALIAS mnemonicdb_lz4)
add_library(zstd::zstd ALIAS mnemonicdb_zstd)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/rocksdb ${CMAKE_CURRENT_BINARY_DIR}/rocksdb
                 EXCLUDE_FROM_ALL)
# DuckDB's own (namespaced) zstd headers are on the global include path; RocksDB must see the real ones first
target_include_directories(rocksdb BEFORE PRIVATE ${MNEMONICDB_ZSTD_DIR} ${MNEMONICDB_LZ4_DIR})
if(MSVC)
  target_compile_definitions(rocksdb PRIVATE LZ4 ZSTD)
  cmake_policy(SET CMP0079 NEW)
  target_link_libraries(rocksdb PRIVATE mnemonicdb_lz4 mnemonicdb_zstd)
endif()

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
    src/catalog/mnemonic_index.cpp
    src/exec/mnemonic_scan.cpp
    src/exec/mnemonic_insert.cpp
    src/exec/mnemonic_modify.cpp
    src/exec/mnemonic_datom_functions.cpp
    src/optimizer/mnemonic_fusion.cpp
    src/exec/mnemonic_history.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# RocksDB headers require C++20
set_target_properties(rocksdb ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
                                                                                CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(${EXTENSION_NAME} rocksdb)
target_link_libraries(${LOADABLE_EXTENSION_NAME} rocksdb)

install(
  TARGETS ${EXTENSION_NAME} rocksdb mnemonicdb_lz4 mnemonicdb_zstd
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
