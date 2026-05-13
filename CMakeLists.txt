cmake_minimum_required(VERSION 3.16)

project(AtlasTranslator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Sql)

qt_standard_project_setup()

add_executable(AtlasTranslator
    main.cpp
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
)

target_link_libraries(AtlasTranslator
    PRIVATE
        Qt6::Core
        Qt6::Sql
)

add_custom_command(TARGET AtlasTranslator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:AtlasTranslator>/database"
)
