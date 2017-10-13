cmake_minimum_required(VERSION 3.0.0)

set(PROJECT_NAME "plv8")
set(PROJECT_ID "plv8")

set(VENDOR "")

set(VERSION_MAJOR "2") 
set(VERSION_MINOR "1") 
set(VERSION_PATCH "0") 

set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

project("${PROJECT_ID}")

# ↓はユーザーが指定した「%PostgreSQLをインストールしたフォルダー%」になる
set(POSTGRESQL_DIR "${CMAKE_INSTALL_PREFIX}"
  CACHE PATH "PostgreSQL binary directory")
# ↓はパッケージを作成するときだけ必要
set(POSTGRESQL_VERSION "unknown"
  CACHE STRING "PostgreSQL version")

set(LIBRARY_NAME "lib${PROJECT_ID}")

set(EXTENSION_DIR "lib")
set(EXTENSION_DATA_DIR "share/extension")
set(DOCUMENT_DIR "share/${PROJECT_ID}")

set(SOURCES
  "coffee-script.cc"
  "livescript.cc"
  "plv8.cc"
  "plv8_func.cc"
  "plv8_param.cc"
  "plv8_type.cc")


include_directories(
  "${POSTGRESQL_DIR}/include/server/port/win32_msvc"
  "${POSTGRESQL_DIR}/include/server/port/win32"
  "${POSTGRESQL_DIR}/include/server"
  "${POSTGRESQL_DIR}/include"
  "vendor/v8/include")

link_directories(
  "${POSTGRESQL_DIR}/lib")

add_library("${LIBRARY_NAME}" SHARED ${SOURCES})
set_target_properties("${LIBRARY_NAME}"
   PROPERTIES
   OUTPUT_NAME "${PROJECT_ID}")

set_source_files_properties(${SOURCES}
  PROPERTIES
  COMPILE_FLAGS "/EHsc")

target_link_libraries("${LIBRARY_NAME}"
  "postgres.lib"
  "vendor/v8/lib/Release/v8"
  "vendor/v8/lib/Release/v8_libbase"
  "vendor/v8/lib/Release/v8_libplatform")