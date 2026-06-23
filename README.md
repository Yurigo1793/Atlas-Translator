# Atlas-Translator

Atlas-Translator is a local translation workspace built around Atlas SQLite databases,
dataset importers, preprocessing tools, and a console translator.

The core translator is offline by default. It reads local Atlas databases and only
returns translations that can be assembled from stored translation pairs, unless an
optional local neural backend is configured.

## Current State

This repository is being preserved with most local project artifacts included.
The root `.gitignore` ignores the root `build/` output and the files inside
`datasets/`. Dataset folders are kept with `.gitkeep` placeholders, while the
large corpus files remain local.

Before publishing publicly, review the large and local-state folders carefully:

- `datasets/` contains very large corpora and preprocessed TSV files locally;
  only its folder structure is meant to go to Git.
- `external_tools/google_official_lab/browser_profile/` and
  `external_tools/google_official_lab/chrome_logged_profile/` contain browser
  profile state.
- `external_tools/dictionary_extractor/.venv/` contains a local Python virtual
  environment.
- Generated reports and outputs are intentionally visible now.

## What The Program Does

- Opens Atlas SQLite databases from `database/`.
- Lists language pairs found in the loaded database.
- Translates typed text with the best matching stored phrases.
- Keeps unknown words unchanged instead of inventing translations.
- Preserves basic line breaks, punctuation, capitalization, quotes, and
  parentheses.
- Uses frequency information to choose between repeated candidates.
- Uses an in-memory cache to reduce repeated SQLite lookups.
- Imports detected datasets into pair-specific Atlas databases.
- Preprocesses OPUS/Moses and MediaWiki sources for faster later imports.
- Writes import, preprocessing, translation, rejected-detail, and flux reports.
- Can call an optional local neural translator when the database has no match for
  a segment.

## What It Does Not Do By Itself

- It does not train a model.
- It does not download source corpora.
- It does not call online services during normal translation.
- Without an optional neural backend, it does not translate words or phrases that
  are not present in the database.

## Console Menu

The application starts by loading the newest non-empty `database/Atlas_*.db`
database it can find. The main menu includes:

```text
1 - Translate text
2 - Importar dataset detectado
3 - Importar todos os datasets detectados
4 - Pre-processar MediaWiki
5 - Pre-processar OPUS/Moses
0 - Exit
```

Import options scan `datasets/`, filter supported language pairs, and create or
update Atlas SQLite databases under `database/`.

## Build

Requirements:

- CMake 3.16+
- Qt 6 with `Core` and `Sql`
- A C++17 compiler

Build:

```sh
cmake -S . -B build
cmake --build build
```

The executable is generated as:

```text
build/AtlasTranslator
```

On some Qt Creator builds, the executable may be inside a configuration folder
under `build/`.

## Run

Place at least one ready Atlas database in `database/`, or import datasets from
the menu, then run:

```sh
./build/AtlasTranslator
```

On Windows, the built executable may be run from the build output directory,
depending on the selected CMake generator and Qt Creator configuration.

## Environment Variables

`ATLAS_DATASETS_PATH` can override the dataset search path. If it is not set,
Atlas searches from the current path, the application path, and then falls back
to `datasets/`.

`ATLAS_NEURAL_TRANSLATOR_CMD` enables the optional local neural backend.
`ATLAS_NEURAL_TRANSLATOR_TIMEOUT_MS` controls its timeout.

Neural backend contract:

- The command receives `sourceLang` and `targetLang` as command-line arguments,
  unless placeholders are used.
- The source text is sent through standard input.
- The translated text must be written to standard output.
- A non-zero exit code is treated as failure, and Atlas keeps the database
  fallback output.

Example:

```sh
set ATLAS_NEURAL_TRANSLATOR_CMD=C:\Atlas-Translator\neural\translate-local.cmd
set ATLAS_NEURAL_TRANSLATOR_TIMEOUT_MS=15000
```

The command may also use placeholders:

```sh
set ATLAS_NEURAL_TRANSLATOR_CMD=C:\Atlas-Translator\neural\translate-local.cmd --from {source} --to {target}
```

When no neural command is configured, Atlas behaves as a database-only
translator.

## Database Files

Ready-to-use Atlas databases live in:

```text
database/
```

Expected names are pair-specific:

```text
database/Atlas_por-en_en-por.db
database/Atlas_en-es_es-en.db
database/Atlas_fr-por_por-fr.db
```

The importer creates databases with the normalized source and target language
pair in the filename.

## Datasets

The `datasets/` folder structure is part of the preserved workspace, but the
large files inside those folders are ignored. Current dataset families include:

- `datasets/FreeDict/`: TEI dictionaries.
- `datasets/MediaWiki/`: MediaWiki/Wiktionary XML dumps.
- `datasets/OPUS/`: raw OPUS/Moses-style parallel text, XML metadata, and ID
  files.
- `datasets/OPUS_Preprocessed/`: generated preprocessed TSV files and
  preprocessing reports.

The scanner recognizes parallel text files, OPUS preprocessed TSV files,
FreeDict TEI files, MediaWiki XML dumps, and MediaWiki preprocessed TSV files.

## External Tools

`external_tools/dictionary_extractor/` contains a local PDF dictionary extractor.
It reads:

```text
external_tools/dicionario-pt.pdf
```

and writes TSV files under:

```text
external_tools/dictionary_extractor/output/
```

Main command:

```bat
external_tools\dictionary_extractor\EXTRAIR_DICIONARIO_PDF.cmd
```

`external_tools/google_official_lab/` contains a browser automation lab that
opens the official Google Translate website, reads TSV input from `input/`, and
writes importable TSV/progress/report files to `output/`.

Main commands:

```bat
external_tools\google_official_lab\GOOGLE_OFICIAL_LAB.cmd
external_tools\google_official_lab\RESETAR_GOOGLE_OFICIAL.cmd
external_tools\google_official_lab\ABRIR_PERFIL_ATLAS_PARA_LOGIN.cmd
```

The lab keeps browser state in `browser_profile/` and
`chrome_logged_profile/`. These folders are no longer ignored.

## Helper Scripts

`tools/` contains PowerShell utilities used during dataset preparation:

- `Analyze-MediaWikiTranslations.ps1`
- `Split-MediaWikiDump.ps1`

## Project Layout

```text
.agents/        local agent notes/state
.qtcreator/     Qt Creator project state, now visible to Git
build/          generated build output, still ignored
core/           translator engine, importers, database access, reports, cache
database/       Atlas SQLite databases and database placeholders
datasets/       dataset folder structure; internal files are ignored
external_tools/ dictionary extraction and Google Translate lab tools
tests/          test placeholder/local experiments
tools/          PowerShell helper scripts
main.cpp        console application entry point
CMakeLists.txt  CMake/Qt build definition
```

## Git Ignore Policy

The repository intentionally ignores:

```text
/build/
/datasets/**
!/datasets/
!/datasets/**/
!/datasets/**/.gitkeep
```

Everything else is available for `git add` so the paused workspace can be sent to
GitHub without the heavy dataset contents.
