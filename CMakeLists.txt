cmake_minimum_required(VERSION 3.16)

project(AtlasTranslator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/database")

find_package(Qt6 REQUIRED COMPONENTS Core Sql)

qt_standard_project_setup()

add_executable(AtlasTranslator
    main.cpp
    core/AppPaths.cpp
    core/AppPaths.h
    core/AtlasReport.cpp
    core/AtlasReport.h
    core/AtlasImporter.cpp
    core/AtlasImporter.h
    core/DatabaseManager.cpp
    core/DatabaseManager.h
    core/LanguageNormalizer.cpp
    core/LanguageNormalizer.h
    core/NeuralTranslator.cpp
    core/NeuralTranslator.h
    core/DatasetScanner.cpp
    core/DatasetScanner.h
    core/Tokenizer.cpp
    core/Tokenizer.h
    core/QueryCache.cpp
    core/QueryCache.h
    core/TextNormalizer.cpp
    core/TextNormalizer.h
    core/TranslationOrganizer.cpp
    core/TranslationOrganizer.h
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
)
