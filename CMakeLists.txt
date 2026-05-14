cmake_minimum_required(VERSION 3.16)

project(AtlasTranslator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ATLAS_RUNTIME_DIRECTORIES
    database
    datasets
)

foreach(runtime_directory IN LISTS ATLAS_RUNTIME_DIRECTORIES)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/${runtime_directory}")
endforeach()

file(GLOB ATLAS_DATASET_CANDIDATES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/datasets/*"
)

set(ATLAS_RUNTIME_DATASETS)
foreach(dataset_candidate IN LISTS ATLAS_DATASET_CANDIDATES)
    if(IS_DIRECTORY "${dataset_candidate}")
        continue()
    endif()

    get_filename_component(dataset_name "${dataset_candidate}" NAME)
    if(dataset_name MATCHES "^.+\\.([A-Za-z][A-Za-z][A-Za-z]?(_[A-Za-z0-9]+)?)-([A-Za-z][A-Za-z][A-Za-z]?(_[A-Za-z0-9]+)?)\\.([A-Za-z][A-Za-z][A-Za-z]?(_[A-Za-z0-9]+)?)$")
        if(CMAKE_MATCH_5 STREQUAL CMAKE_MATCH_1 OR CMAKE_MATCH_5 STREQUAL CMAKE_MATCH_3)
            list(APPEND ATLAS_RUNTIME_DATASETS "${dataset_candidate}")
        endif()
    endif()
endforeach()

set(ATLAS_COPY_DATASET_COMMANDS)
foreach(runtime_dataset IN LISTS ATLAS_RUNTIME_DATASETS)
    list(APPEND ATLAS_COPY_DATASET_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${runtime_dataset}"
                "$<TARGET_FILE_DIR:AtlasTranslator>/datasets"
    )
endforeach()

find_package(Qt6 REQUIRED COMPONENTS Core Sql)

qt_standard_project_setup()

add_executable(AtlasTranslator
    main.cpp
    core/AppPaths.cpp
    core/AppPaths.h
    core/AtlasImporter.cpp
    core/AtlasImporter.h
    core/DatabaseManager.cpp
    core/DatabaseManager.h
    core/LanguageNormalizer.cpp
    core/LanguageNormalizer.h
    core/DatasetScanner.cpp
    core/DatasetScanner.h
    core/Tokenizer.cpp
    core/Tokenizer.h
    core/QueryCache.cpp
    core/QueryCache.h
    core/TextNormalizer.cpp
    core/TextNormalizer.h
    core/TranslatorEngine.cpp
    core/TranslatorEngine.h
    core/Utf8Streams.h
)

target_link_libraries(AtlasTranslator
    PRIVATE
        Qt6::Core
        Qt6::Sql
)

add_custom_command(TARGET AtlasTranslator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:AtlasTranslator>/database"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:AtlasTranslator>/datasets"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:AtlasTranslator>/datasets"
    ${ATLAS_COPY_DATASET_COMMANDS}
)
