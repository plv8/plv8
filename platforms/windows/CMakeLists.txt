cmake_minimum_required(VERSION 3.1.0)

set(PROJECT_NAME "plv8")
set(PROJECT_ID "plv8")

set(VENDOR "")

set(VERSION_MAJOR "3")
set(VERSION_MINOR "1")
set(VERSION_PATCH "0")

set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

project("${PROJECT_ID}")

set(POSTGRESQL_DIR "${CMAKE_INSTALL_PREFIX}"
  CACHE PATH "PostgreSQL binary directory")
set(POSTGRESQL_VERSION "unknown"
  CACHE STRING "PostgreSQL version")

set(LIBRARY_NAME "lib${PROJECT_ID}")

set(EXTENSION_DIR "lib")
set(EXTENSION_DATA_DIR "share/extension")
set(DOCUMENT_DIR "share/${PROJECT_ID}")

set(SOURCES
  "coffee-script.cc"
  "livescript.cc"
  "../plv8.cc"
  "../plv8_func.cc"
  "../plv8_param.cc"
  "../plv8_type.cc")


include_directories(
  "${POSTGRESQL_DIR}/include/server/port/win32_msvc"
  "${POSTGRESQL_DIR}/include/server/port/win32"
  "${POSTGRESQL_DIR}/include/server"
  "${POSTGRESQL_DIR}/include"
  "vendor/v8/include"
  ".")

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
  "vendor/v8/out.gn/x64.release/v8.dll"
  "vendor/v8/out.gn/x64.release/v8_libbase.dll"
  "vendor/v8/out.gn/x64.release/v8_libplatform.dll")

install(TARGETS "${LIBRARY_NAME}"
  DESTINATION "${EXTENSION_DIR}")

install(FILES
  "${PROJECT_ID}.control"
  DESTINATION "${EXTENSION_DATA_DIR}")

install(FILES
  "plcoffee.control"
  DESTINATION "${EXTENSION_DATA_DIR}")

install(FILES
  "plls.control"
  DESTINATION "${EXTENSION_DATA_DIR}")

install(FILES
  "${PROJECT_ID}--${VERSION_FULL}.sql"
  DESTINATION "${EXTENSION_DATA_DIR}")

install(FILES
  "plcoffee--${VERSION_FULL}.sql"
  DESTINATION "${EXTENSION_DATA_DIR}")

install(FILES
  "plls--${VERSION_FULL}.sql"
  DESTINATION "${EXTENSION_DATA_DIR}")

install(FILES
  "./vendor/v8/out.gn/x64.release/v8.dll"
  DESTINATION "bin")
install(FILES
  "./vendor/v8/out.gn/x64.release/v8_libbase.dll"
  DESTINATION "bin")
install(FILES
  "./vendor/v8/out.gn/x64.release/v8_libplatform.dll"
  DESTINATION "bin")

set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_PACKAGE_VERSION_MAJOR "${VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${VERSION_MICRO}")
set(CPACK_PACKAGE_VENDOR "${VENDOR}")
if(CMAKE_CL_64)
  set(PLV8_SYSTEM_NAME "x64")
else()
  set(PLV8_SYSTEM_NAME "x86")
endif()
set(CPACK_PACKAGE_FILE_NAME
  "${PROJECT_ID}-${VERSION_FULL}-postgresql-${POSTGRESQL_VERSION}-${PLV8_SYSTEM_NAME}")

include(CPack)
