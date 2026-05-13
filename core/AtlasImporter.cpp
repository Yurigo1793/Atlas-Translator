#include "AtlasImporter.h"

#include "TextNormalizer.h"
#include "AppPaths.h"
#include "Utf8Streams.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSqlError>
#include <QSqlQuery>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <QUuid>
#include <QVariant>

namespace {
constexpr qsizetype MaximumLineLength = 500;
constexpr qsizetype MinimumLineLength = 2;
constexpr qint64 ProgressInterval = 10000;
constexpr qint64 CommitInterval = 10000;
}

AtlasImporter::AtlasImporter(const QString &databasePath)
    : m_databasePath(databasePath),
      m_connectionName(QStringLiteral("atlas_importer_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
    if (m_databasePath.isEmpty()) {
        m_databasePath = AppPaths::databaseFile();
    }

    m_databasePath = QDir::cleanPath(QFileInfo(m_databasePath).absoluteFilePath());
}

AtlasImporter::~AtlasImporter()
{
    if (m_database.isValid()) {
        m_database.close();
    }

    m_database = QSqlDatabase();

    if (!m_connectionName.isEmpty()) {
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool AtlasImporter::importMosesDataset(const QString &sourceFilePath,
                                       const QString &targetFilePath,
                                       const QString &sourceLang,
                                       const QString &targetLang)
{
    m_stats = ImportStats();
    m_cleanupStats = CleanupStats();
    m_lastError.clear();

    const QString normalizedSourceLang = m_languageNormalizer.normalize(sourceLang);
    const QString normalizedTargetLang = m_languageNormalizer.normalize(targetLang);
    const QString currentImportKey = importKey(sourceFilePath, targetFilePath, normalizedSourceLang, normalizedTargetLang);

    QFile sourceFile(sourceFilePath);
    QFile targetFile(targetFilePath);

    if (!sourceFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Não foi possível abrir o arquivo de origem: %1").arg(sourceFilePath);
        return false;
    }

    if (!targetFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("Não foi possível abrir o arquivo de destino: %1").arg(targetFilePath);
        return false;
    }

    if (!openDatabase() || !ensureSchema()) {
        return false;
    }

    QTextStream sourceStream(&sourceFile);
    QTextStream targetStream(&targetFile);
    configureUtf8Stream(sourceStream);
    configureUtf8Stream(targetStream);

    QTextStream console(stdout);
    configureUtf8Stream(console);
    console << "Importando..." << Qt::endl;
    console << "Database path: " << m_databasePath << Qt::endl;
    console << "SQLite opened: " << (m_database.isOpen() ? QStringLiteral("yes") : QStringLiteral("no")) << Qt::endl;
    console << "Idiomas: " << normalizedSourceLang << " -> " << normalizedTargetLang << Qt::endl;

    qint64 resumeFromLine = 0;
    bool completed = false;
    if (!loadProgress(currentImportKey, resumeFromLine, completed)) {
        return false;
    }

    if (completed) {
        console << "Dataset já importado anteriormente. Executando limpeza/otimização do banco." << Qt::endl;
        if (!cleanupDatabase()) {
            return false;
        }
        console << "Database cleanup complete" << Qt::endl;
        console << "Removed invalid entries: " << m_cleanupStats.removedInvalidEntries << Qt::endl;
        console << "Removed duplicates: " << m_cleanupStats.removedDuplicates << Qt::endl;
        console << "Final translations count: " << m_cleanupStats.finalTranslationsCount << Qt::endl;
        return true;
    }

    if (resumeFromLine > 0) {
        console << "Retomando importação da linha: " << resumeFromLine << Qt::endl;
        if (!skipLines(sourceStream, targetStream, resumeFromLine)) {
            return false;
        }
        m_stats.resumedLines = resumeFromLine;
        m_stats.processedLines = resumeFromLine;
    }

    QSqlQuery insertQuery(m_database);
    if (!prepareInsertStatement(insertQuery)) {
        return false;
    }

    QSqlQuery pairExistsQuery(m_database);
    if (!preparePairExistsStatement(pairExistsQuery)) {
        return false;
    }

    if (!m_database.transaction()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    bool success = true;
    qint64 linesSinceCommit = 0;
    while (!sourceStream.atEnd() && !targetStream.atEnd()) {
        const QString sourceText = cleanText(sourceStream.readLine());
        const QString targetText = cleanText(targetStream.readLine());
        ++m_stats.processedLines;
        ++linesSinceCommit;

        if (!isValidTranslationPair(sourceText, targetText)) {
            ++m_stats.ignoredLines;
        } else {
            const QString normalizedSource = normalizedSourceText(sourceText);
            const bool existingPair = translationPairExists(pairExistsQuery,
                                                            normalizedSource,
                                                            targetText,
                                                            normalizedSourceLang,
                                                            normalizedTargetLang);
            if (!m_lastError.isEmpty()) {
                success = false;
                break;
            }

            insertQuery.bindValue(QStringLiteral(":source_text"), normalizedSource);
            insertQuery.bindValue(QStringLiteral(":translated_text"), targetText);
            insertQuery.bindValue(QStringLiteral(":source_lang"), normalizedSourceLang);
            insertQuery.bindValue(QStringLiteral(":target_lang"), normalizedTargetLang);

            if (!insertQuery.exec()) {
                m_lastError = insertQuery.lastError().text();
                success = false;
                break;
            }

            if (existingPair) {
                ++m_stats.updatedFrequencyLines;
                ++m_stats.duplicateLines;
            } else {
                ++m_stats.insertedLines;
            }
        }

        if ((m_stats.processedLines % ProgressInterval) == 0) {
            printProgress();
        }

        if (linesSinceCommit >= CommitInterval) {
            if (!saveProgress(currentImportKey,
                              sourceFilePath,
                              targetFilePath,
                              normalizedSourceLang,
                              normalizedTargetLang,
                              m_stats.processedLines,
                              false)) {
                success = false;
                break;
            }

            if (!m_database.commit()) {
                m_lastError = m_database.lastError().text();
                success = false;
                break;
            }

            if (!m_database.transaction()) {
                m_lastError = m_database.lastError().text();
                success = false;
                break;
            }
            linesSinceCommit = 0;
        }
    }

    if (success && (sourceStream.atEnd() != targetStream.atEnd())) {
        m_lastError = QStringLiteral("Dataset desalinhado: os arquivos têm quantidades de linhas diferentes.");
        success = false;
    }

    if (success) {
        success = saveProgress(currentImportKey,
                               sourceFilePath,
                               targetFilePath,
                               normalizedSourceLang,
                               normalizedTargetLang,
                               m_stats.processedLines,
                               true);
    }

    if (success && !m_database.commit()) {
        m_lastError = m_database.lastError().text();
        success = false;
    } else if (!success) {
        m_database.rollback();
    }

    if (success && !ensureIndexes()) {
        return false;
    }

    if (success && !cleanupDatabase()) {
        return false;
    }

    printProgress();
    console << "Resumo final:" << Qt::endl;
    console << "Linhas retomadas: " << m_stats.resumedLines << Qt::endl;
    console << "Linhas processadas: " << m_stats.processedLines << Qt::endl;
    console << "Novos pares: " << m_stats.insertedLines << Qt::endl;
    console << "Frequências atualizadas: " << m_stats.updatedFrequencyLines << Qt::endl;
    console << "Ignoradas: " << m_stats.ignoredLines << Qt::endl;
    console << "Duplicadas: " << m_stats.duplicateLines << Qt::endl;
    console << "Database cleanup complete" << Qt::endl;
    console << "Removed invalid entries: " << m_cleanupStats.removedInvalidEntries << Qt::endl;
    console << "Removed duplicates: " << m_cleanupStats.removedDuplicates << Qt::endl;
    console << "Final translations count: " << m_cleanupStats.finalTranslationsCount << Qt::endl;

    return success;
}

AtlasImporter::ImportStats AtlasImporter::stats() const
{
    return m_stats;
}

AtlasImporter::CleanupStats AtlasImporter::cleanupStats() const
{
    return m_cleanupStats;
}

QString AtlasImporter::lastError() const
{
    return m_lastError;
}

QString AtlasImporter::databasePath() const
{
    return m_databasePath;
}

bool AtlasImporter::openDatabase()
{
    if (m_database.isOpen()) {
        return true;
    }

    const QFileInfo databaseInfo(m_databasePath);
    QDir directory = databaseInfo.dir();
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        m_lastError = QStringLiteral("Não foi possível criar o diretório do banco de dados.");
        return false;
    }

    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(m_databasePath);

    if (!m_database.open()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    QSqlQuery pragmas(m_database);
    pragmas.exec(QStringLiteral("PRAGMA busy_timeout = 10000"));
    pragmas.finish();
    pragmas.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    pragmas.finish();
    pragmas.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    pragmas.finish();
    pragmas.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    pragmas.finish();

    return true;
}

bool AtlasImporter::ensureSchema()
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS translations (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source_text TEXT NOT NULL,
            translated_text TEXT NOT NULL,
            source_lang TEXT NOT NULL,
            target_lang TEXT NOT NULL,
            frequency INTEGER NOT NULL DEFAULT 1
        )
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    return removeLegacyUniqueConstraint() && ensureFrequencySchema() && ensureIndexes() && ensureProgressTable();
}

bool AtlasImporter::ensureIndexes()
{
    const QStringList statements = {
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translations_source_text ON translations(source_text)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translations_source_lang ON translations(source_lang)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translations_target_lang ON translations(target_lang)"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_translations_pair_unique ON translations(source_text, translated_text, source_lang, target_lang)")
    };

    for (const QString &statement : statements) {
        QSqlQuery query(m_database);
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            return false;
        }
        query.finish();
    }

    return ensureLookupIndex();
}

bool AtlasImporter::ensureLookupIndex()
{
    QStringList indexedColumns;
    QSqlQuery indexInfo(m_database);
    if (indexInfo.exec(QStringLiteral("PRAGMA index_info(idx_translations_lookup)"))) {
        while (indexInfo.next()) {
            indexedColumns.append(indexInfo.value(2).toString());
        }
        indexInfo.finish();
    } else {
        m_lastError = indexInfo.lastError().text();
        return false;
    }

    const QStringList expectedColumns = {
        QStringLiteral("source_lang"),
        QStringLiteral("target_lang"),
        QStringLiteral("source_text"),
        QStringLiteral("frequency")
    };

    if (!indexedColumns.isEmpty() && indexedColumns != expectedColumns) {
        QSqlQuery dropQuery(m_database);
        if (!dropQuery.exec(QStringLiteral("DROP INDEX idx_translations_lookup"))) {
            m_lastError = dropQuery.lastError().text();
            return false;
        }
        dropQuery.finish();
        indexedColumns.clear();
    }

    if (indexedColumns.isEmpty()) {
        QSqlQuery createQuery(m_database);
        if (!createQuery.exec(QStringLiteral(
                "CREATE INDEX idx_translations_lookup ON translations(source_lang, target_lang, source_text, frequency DESC)"))) {
            m_lastError = createQuery.lastError().text();
            return false;
        }
        createQuery.finish();
    }

    return true;
}


bool AtlasImporter::ensureFrequencySchema()
{
    bool hasFrequency = false;
    QSqlQuery tableInfo(m_database);
    if (!tableInfo.exec(QStringLiteral("PRAGMA table_info(translations)"))) {
        m_lastError = tableInfo.lastError().text();
        return false;
    }
    while (tableInfo.next()) {
        if (tableInfo.value(1).toString() == QStringLiteral("frequency")) {
            hasFrequency = true;
            break;
        }
    }
    tableInfo.finish();

    bool hasPairUnique = false;
    QSqlQuery indexList(m_database);
    if (!indexList.exec(QStringLiteral("PRAGMA index_list(translations)"))) {
        m_lastError = indexList.lastError().text();
        return false;
    }
    while (indexList.next()) {
        if (indexList.value(1).toString() == QStringLiteral("idx_translations_pair_unique")) {
            hasPairUnique = true;
            break;
        }
    }
    indexList.finish();

    if (hasFrequency && hasPairUnique) {
        return true;
    }

    QSqlQuery duplicateGroupsQuery(m_database);
    qint64 duplicateRows = 0;
    if (duplicateGroupsQuery.exec(QStringLiteral(R"(
        SELECT COALESCE(SUM(group_count - 1), 0)
        FROM (
            SELECT COUNT(*) AS group_count
            FROM translations
            GROUP BY source_text, translated_text, source_lang, target_lang
            HAVING COUNT(*) > 1
        )
    )")) && duplicateGroupsQuery.next()) {
        duplicateRows = duplicateGroupsQuery.value(0).toLongLong();
        m_cleanupStats.removedDuplicates += duplicateRows;
    }
    duplicateGroupsQuery.finish();

    if (hasFrequency && duplicateRows == 0) {
        return true;
    }

    if (!m_database.transaction()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    QSqlQuery query(m_database);
    const QString frequencyExpression = hasFrequency ? QStringLiteral("COALESCE(frequency, 1)") : QStringLiteral("1");
    const QStringList migrationStatements = {
        QStringLiteral("DROP TABLE IF EXISTS translations_frequency_migration"),
        QStringLiteral(R"(
            CREATE TABLE translations_frequency_migration (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                source_text TEXT NOT NULL,
                translated_text TEXT NOT NULL,
                source_lang TEXT NOT NULL,
                target_lang TEXT NOT NULL,
                frequency INTEGER NOT NULL DEFAULT 1
            )
        )"),
        QStringLiteral(R"(
            INSERT INTO translations_frequency_migration (source_text, translated_text, source_lang, target_lang, frequency)
            SELECT source_text, translated_text, source_lang, target_lang, SUM(%1)
            FROM translations
            GROUP BY source_text, translated_text, source_lang, target_lang
        )").arg(frequencyExpression),
        QStringLiteral("DROP TABLE translations"),
        QStringLiteral("ALTER TABLE translations_frequency_migration RENAME TO translations")
    };

    for (const QString &statement : migrationStatements) {
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            m_database.rollback();
            return false;
        }
        query.finish();
    }

    if (!m_database.commit()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    return true;
}


bool AtlasImporter::ensureFrequencySchema()
{
    bool hasFrequency = false;
    QSqlQuery tableInfo(m_database);
    if (!tableInfo.exec(QStringLiteral("PRAGMA table_info(translations)"))) {
        m_lastError = tableInfo.lastError().text();
        return false;
    }
    while (tableInfo.next()) {
        if (tableInfo.value(1).toString() == QStringLiteral("frequency")) {
            hasFrequency = true;
            break;
        }
    }

    bool hasPairUnique = false;
    QSqlQuery indexList(m_database);
    if (!indexList.exec(QStringLiteral("PRAGMA index_list(translations)"))) {
        m_lastError = indexList.lastError().text();
        return false;
    }
    while (indexList.next()) {
        if (indexList.value(1).toString() == QStringLiteral("idx_translations_pair_unique")) {
            hasPairUnique = true;
            break;
        }
    }

    if (hasFrequency && hasPairUnique) {
        return true;
    }

    QSqlQuery duplicateGroupsQuery(m_database);
    if (duplicateGroupsQuery.exec(QStringLiteral(R"(
        SELECT COALESCE(SUM(group_count - 1), 0)
        FROM (
            SELECT COUNT(*) AS group_count
            FROM translations
            GROUP BY source_text, translated_text, source_lang, target_lang
            HAVING COUNT(*) > 1
        )
    )")) && duplicateGroupsQuery.next()) {
        m_cleanupStats.removedDuplicates += duplicateGroupsQuery.value(0).toLongLong();
    }

    if (!m_database.transaction()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    QSqlQuery query(m_database);
    const QString frequencyExpression = hasFrequency ? QStringLiteral("COALESCE(frequency, 1)") : QStringLiteral("1");
    const QStringList migrationStatements = {
        QStringLiteral("DROP TABLE IF EXISTS translations_frequency_migration"),
        QStringLiteral(R"(
            CREATE TABLE translations_frequency_migration (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                source_text TEXT NOT NULL,
                translated_text TEXT NOT NULL,
                source_lang TEXT NOT NULL,
                target_lang TEXT NOT NULL,
                frequency INTEGER NOT NULL DEFAULT 1
            )
        )"),
        QStringLiteral(R"(
            INSERT INTO translations_frequency_migration (source_text, translated_text, source_lang, target_lang, frequency)
            SELECT source_text, translated_text, source_lang, target_lang, SUM(%1)
            FROM translations
            GROUP BY source_text, translated_text, source_lang, target_lang
        )").arg(frequencyExpression),
        QStringLiteral("DROP TABLE translations"),
        QStringLiteral("ALTER TABLE translations_frequency_migration RENAME TO translations")
    };

    for (const QString &statement : migrationStatements) {
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            m_database.rollback();
            return false;
        }
    }

    if (!m_database.commit()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    return true;
}

bool AtlasImporter::removeLegacyUniqueConstraint()
{
    QSqlQuery schemaQuery(m_database);
    schemaQuery.prepare(QStringLiteral("SELECT sql FROM sqlite_master WHERE type = 'table' AND name = :name"));
    schemaQuery.bindValue(QStringLiteral(":name"), QStringLiteral("translations"));
    if (!schemaQuery.exec() || !schemaQuery.next()) {
        m_lastError = schemaQuery.lastError().text();
        return false;
    }

    QString tableSql = schemaQuery.value(0).toString().toLower();
    tableSql.remove(QChar::Space);
    tableSql.remove(QChar::Tabulation);
    tableSql.remove(QChar::LineFeed);
    tableSql.remove(QChar::CarriageReturn);
    schemaQuery.finish();

    if (!tableSql.contains(QStringLiteral("unique(source_text,source_lang,target_lang)"))) {
        return true;
    }

    if (!m_database.transaction()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    QSqlQuery query(m_database);
    const QStringList migrationStatements = {
        QStringLiteral("DROP TABLE IF EXISTS translations_without_unique"),
        QStringLiteral(R"(
            CREATE TABLE translations_without_unique (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                source_text TEXT NOT NULL,
                translated_text TEXT NOT NULL,
                source_lang TEXT NOT NULL,
                target_lang TEXT NOT NULL
            )
        )"),
        QStringLiteral(R"(
            INSERT INTO translations_without_unique (id, source_text, translated_text, source_lang, target_lang)
            SELECT id, source_text, translated_text, source_lang, target_lang
            FROM translations
        )"),
        QStringLiteral("DROP TABLE translations"),
        QStringLiteral("ALTER TABLE translations_without_unique RENAME TO translations")
    };

    for (const QString &statement : migrationStatements) {
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            m_database.rollback();
            return false;
        }
    }

    if (!m_database.commit()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    return true;
}

bool AtlasImporter::ensureProgressTable()
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS import_progress (
            import_key TEXT PRIMARY KEY,
            source_file TEXT NOT NULL,
            target_file TEXT NOT NULL,
            source_lang TEXT NOT NULL,
            target_lang TEXT NOT NULL,
            processed_lines INTEGER NOT NULL DEFAULT 0,
            completed INTEGER NOT NULL DEFAULT 0,
            updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
        )
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    return true;
}

bool AtlasImporter::prepareInsertStatement(QSqlQuery &query)
{
    if (!query.prepare(QStringLiteral(R"(
        INSERT INTO translations (source_text, translated_text, source_lang, target_lang, frequency)
        VALUES (:source_text, :translated_text, :source_lang, :target_lang, 1)
        ON CONFLICT(source_text, translated_text, source_lang, target_lang)
        DO UPDATE SET frequency = frequency + 1
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    return true;
}


bool AtlasImporter::preparePairExistsStatement(QSqlQuery &query)
{
    if (!query.prepare(QStringLiteral(R"(
        SELECT 1
        FROM translations
        WHERE source_text = :source_text
          AND translated_text = :translated_text
          AND source_lang = :source_lang
          AND target_lang = :target_lang
        LIMIT 1
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    return true;
}

bool AtlasImporter::translationPairExists(QSqlQuery &query,
                                          const QString &sourceText,
                                          const QString &targetText,
                                          const QString &sourceLang,
                                          const QString &targetLang)
{
    m_lastError.clear();
    query.bindValue(QStringLiteral(":source_text"), sourceText);
    query.bindValue(QStringLiteral(":translated_text"), targetText);
    query.bindValue(QStringLiteral(":source_lang"), sourceLang);
    query.bindValue(QStringLiteral(":target_lang"), targetLang);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }

    const bool exists = query.next();
    query.finish();
    return exists;
}

bool AtlasImporter::isValidTranslationPair(const QString &sourceText, const QString &targetText) const
{
    auto hasTechnicalNoise = [](const QString &text) {
        static const QRegularExpression markupExpression(QStringLiteral(R"(<\/?\w+|&(?:[a-z]+|#\d+);|<!DOCTYPE|<\?xml)"),
                                                         QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression styleExpression(QStringLiteral(R"((?:^|[\s;])(?:margin|padding|font-size|background|color|display|width|height)\s*:|\.[A-Za-z0-9_-]+\s*\{|#(?:[A-Fa-f0-9]{3}){1,2}\b)"),
                                                        QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression urlExpression(QStringLiteral(R"((?:https?|ftp)://|www\.|\b\w+://)"),
                                                      QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression codeExpression(QStringLiteral(R"((?:\b(?:function|class|return|if|else|while|for|var|let|const|import|include)\b\s*[\(\{;])|(?:->|=>|::|==|!=|<=|>=|&&|\|\|)|[\{\}\[\];]{2,})"),
                                                       QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression variableExpression(QStringLiteral(R"((?:%\d+|%[A-Za-z_]+|\$\{?\w+\}?|\{\d+\}|@[A-Za-z_]+@|__[A-Za-z0-9_]+__|\b[A-Z_]{3,}\b))"));
        static const QRegularExpression pidExpression(QStringLiteral(R"(\bPID\b|\bprocess\s+id\b)"),
                                                      QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression authorizationExpression(QStringLiteral(R"(\bAuthorization\b|\bBearer\b|\bOAuth\b|\bAPI[-_ ]?Key\b|\btoken\b)"),
                                                               QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression tooltipExpression(QStringLiteral(R"(\btooltip\b|tool\s*tip)"),
                                                         QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression filePathExpression(QStringLiteral(R"((?:[A-Za-z]:\\|/\w+/|\.\.?/|\w+/\w+\.\w{1,6}\b))"));

        return markupExpression.match(text).hasMatch()
            || styleExpression.match(text).hasMatch()
            || urlExpression.match(text).hasMatch()
            || codeExpression.match(text).hasMatch()
            || variableExpression.match(text).hasMatch()
            || pidExpression.match(text).hasMatch()
            || authorizationExpression.match(text).hasMatch()
            || tooltipExpression.match(text).hasMatch()
            || filePathExpression.match(text).hasMatch();
    };

    auto hasExcessiveSymbols = [](const QString &text) {
        qsizetype lettersOrNumbers = 0;
        qsizetype symbols = 0;
        qsizetype repeatedPunctuationRun = 0;
        qsizetype currentPunctuationRun = 0;

        for (const QChar character : text) {
            if (character.isLetterOrNumber()) {
                ++lettersOrNumbers;
                currentPunctuationRun = 0;
            } else if (character.isSpace()) {
                currentPunctuationRun = 0;
            } else {
                ++symbols;
                ++currentPunctuationRun;
                repeatedPunctuationRun = qMax(repeatedPunctuationRun, currentPunctuationRun);
            }
        }

        if (lettersOrNumbers == 0) {
            return true;
        }

        const double symbolRatio = static_cast<double>(symbols) / static_cast<double>(text.size());
        return symbolRatio > 0.35 || repeatedPunctuationRun >= 4;
    };

    auto looksStrange = [](const QString &text) {
        static const QRegularExpression mostlyNumbersExpression(QStringLiteral(R"(^[\d\s\.,:/_\-+]+$)"));
        static const QRegularExpression repeatedTokenExpression(QStringLiteral(R"(\b(\w+)\b(?:\s+\1\b){3,})"),
                                                               QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression longTechnicalTokenExpression(QStringLiteral(R"(\b[A-Za-z0-9_./\\-]{35,}\b)"));

        const QStringList words = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        return mostlyNumbersExpression.match(text).hasMatch()
            || repeatedTokenExpression.match(text).hasMatch()
            || longTechnicalTokenExpression.match(text).hasMatch()
            || (words.size() == 1 && text.size() > 40);
    };

    if (sourceText.isEmpty() || targetText.isEmpty()) {
        return false;
    }

    if (sourceText.size() > MaximumLineLength || targetText.size() > MaximumLineLength) {
        return false;
    }

    if (sourceText.size() < MinimumLineLength || targetText.size() < MinimumLineLength) {
        return false;
    }

    return !hasTechnicalNoise(sourceText)
        && !hasTechnicalNoise(targetText)
        && !hasExcessiveSymbols(sourceText)
        && !hasExcessiveSymbols(targetText)
        && !looksStrange(sourceText)
        && !looksStrange(targetText);
}


bool AtlasImporter::isInvalidStoredTranslationPair(const QString &sourceText, const QString &targetText) const
{
    auto containsControlNoise = [](const QString &text) {
        qsizetype controlCharacters = 0;
        for (const QChar character : text) {
            if (character.isNull()) {
                return true;
            }
            if (character.category() == QChar::Other_Control && !character.isSpace()) {
                ++controlCharacters;
            }
        }
        return controlCharacters > 0;
    };

    auto hasTechnicalJunk = [](const QString &text) {
        static const QRegularExpression htmlXmlExpression(QStringLiteral(R"(<\/?\w+|<!DOCTYPE|<\?xml|&(?:[a-z]+|#\d+);)"),
                                                          QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression cssExpression(QStringLiteral(R"((?:^|[\s;])(?:margin|padding|font-size|background|display|width|height)\s*:|\.[A-Za-z0-9_-]+\s*\{|#(?:[A-Fa-f0-9]{3}){1,2}\b)"),
                                                      QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression urlExpression(QStringLiteral(R"((?:https?|ftp)://|www\.|\b\w+://)"),
                                                      QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression codeExpression(QStringLiteral(R"((?:\b(?:function|class|return|while|for|var|let|const|include)\b\s*[\(\{;])|(?:->|=>|::|==|!=|<=|>=|&&|\|\|)|[\{\}\[\];]{3,})"),
                                                       QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression authSecretExpression(QStringLiteral(R"((?:Authorization\s*:\s*(?:Bearer|Basic)|\bBearer\s+[A-Za-z0-9._\-]{16,}|\bOAuth\b|\bAPI[-_ ]?Key\s*[:=]|\btoken\s*[:=]\s*[A-Za-z0-9._\-]{12,}))"),
                                                             QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression pidValueExpression(QStringLiteral(R"(\bPID\s*[:=#]?\s*\d+\b|\bprocess\s+id\s*[:=#]?\s*\d+\b)"),
                                                           QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression tooltipMarkupExpression(QStringLiteral(R"(<[^>]*tooltip|tooltip\s*[:=]\s*['\"<{])"),
                                                                QRegularExpression::CaseInsensitiveOption);

        return htmlXmlExpression.match(text).hasMatch()
            || cssExpression.match(text).hasMatch()
            || urlExpression.match(text).hasMatch()
            || codeExpression.match(text).hasMatch()
            || authSecretExpression.match(text).hasMatch()
            || pidValueExpression.match(text).hasMatch()
            || tooltipMarkupExpression.match(text).hasMatch();
    };

    auto hasExcessiveSymbolNoise = [](const QString &text) {
        qsizetype lettersOrNumbers = 0;
        qsizetype symbols = 0;
        qsizetype replacementCharacters = 0;
        qsizetype repeatedPunctuationRun = 0;
        qsizetype currentPunctuationRun = 0;

        for (const QChar character : text) {
            if (character == QChar(0xFFFD)) {
                ++replacementCharacters;
            }

            if (character.isLetterOrNumber()) {
                ++lettersOrNumbers;
                currentPunctuationRun = 0;
            } else if (character.isSpace()) {
                currentPunctuationRun = 0;
            } else {
                ++symbols;
                ++currentPunctuationRun;
                repeatedPunctuationRun = qMax(repeatedPunctuationRun, currentPunctuationRun);
            }
        }

        if (lettersOrNumbers == 0) {
            return true;
        }

        const double symbolRatio = static_cast<double>(symbols) / static_cast<double>(qMax<qsizetype>(text.size(), 1));
        const double replacementRatio = static_cast<double>(replacementCharacters) / static_cast<double>(qMax<qsizetype>(text.size(), 1));
        return symbolRatio > 0.45 || repeatedPunctuationRun >= 6 || replacementRatio > 0.05;
    };

    auto looksBroken = [](const QString &text) {
        static const QRegularExpression repeatedTokenExpression(QStringLiteral(R"(\b(\w+)\b(?:\s+\1\b){4,})"),
                                                               QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression mostlyNumbersExpression(QStringLiteral(R"(^[\d\s\.,:/_\-+]+$)"));
        const QStringList words = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        return repeatedTokenExpression.match(text).hasMatch()
            || mostlyNumbersExpression.match(text).hasMatch()
            || (words.size() == 1 && text.size() > 80);
    };

    if (sourceText.trimmed().isEmpty() || targetText.trimmed().isEmpty()) {
        return true;
    }

    constexpr qsizetype CleanupMaximumLength = 800;
    if (sourceText.size() > CleanupMaximumLength || targetText.size() > CleanupMaximumLength) {
        return true;
    }

    return containsControlNoise(sourceText)
        || containsControlNoise(targetText)
        || hasTechnicalJunk(sourceText)
        || hasTechnicalJunk(targetText)
        || hasExcessiveSymbolNoise(sourceText)
        || hasExcessiveSymbolNoise(targetText)
        || looksBroken(sourceText)
        || looksBroken(targetText);
}

bool AtlasImporter::cleanupDatabase()
{
    const qint64 migratedDuplicates = m_cleanupStats.removedDuplicates;
    m_cleanupStats = CleanupStats();
    m_cleanupStats.removedDuplicates = migratedDuplicates;

    if (!m_database.transaction()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    QList<QVariant> invalidIds;
    {
        QSqlQuery selectQuery(m_database);
        if (!selectQuery.exec(QStringLiteral("SELECT id, source_text, translated_text FROM translations"))) {
            m_lastError = selectQuery.lastError().text();
            m_database.rollback();
            return false;
        }

        while (selectQuery.next()) {
            const QString sourceText = selectQuery.value(1).toString();
            const QString targetText = selectQuery.value(2).toString();
            if (isInvalidStoredTranslationPair(sourceText, targetText)) {
                invalidIds.append(selectQuery.value(0));
            }
        }
        selectQuery.finish();
    }

    QSqlQuery deleteQuery(m_database);
    if (!deleteQuery.prepare(QStringLiteral("DELETE FROM translations WHERE id = :id"))) {
        m_lastError = deleteQuery.lastError().text();
        m_database.rollback();
        return false;
    }

    for (const QVariant &invalidId : invalidIds) {
        deleteQuery.bindValue(QStringLiteral(":id"), invalidId);
        if (!deleteQuery.exec()) {
            m_lastError = deleteQuery.lastError().text();
            m_database.rollback();
            return false;
        }
        deleteQuery.finish();
        ++m_cleanupStats.removedInvalidEntries;
    }

    if (!m_database.commit()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    if (!ensureFrequencySchema() || !ensureIndexes() || !optimizeDatabase(m_cleanupStats.removedInvalidEntries > 0
                                                                           || m_cleanupStats.removedDuplicates > 0)) {
        return false;
    }

    m_cleanupStats.finalTranslationsCount = translationCount();
    return m_cleanupStats.finalTranslationsCount >= 0;
}

bool AtlasImporter::optimizeDatabase(bool compactDatabase)
{
    QStringList maintenanceStatements;
    if (compactDatabase) {
        maintenanceStatements.append(QStringLiteral("VACUUM"));
    }
    maintenanceStatements.append(QStringLiteral("ANALYZE"));
    if (compactDatabase) {
        maintenanceStatements.append(QStringLiteral("REINDEX"));
    }

    for (const QString &statement : maintenanceStatements) {
        QSqlQuery query(m_database);
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            return false;
        }
        query.finish();
    }

    return true;
}

qint64 AtlasImporter::translationCount() const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM translations")) || !query.next()) {
        return -1;
    }

    const qint64 count = query.value(0).toLongLong();
    query.finish();
    return count;
}


bool AtlasImporter::loadProgress(const QString &importKey, qint64 &processedLines, bool &completed)
{
    processedLines = 0;
    completed = false;

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(R"(
        SELECT processed_lines, completed
        FROM import_progress
        WHERE import_key = :import_key
        LIMIT 1
    )"));
    query.bindValue(QStringLiteral(":import_key"), importKey);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }

    if (query.next()) {
        processedLines = query.value(0).toLongLong();
        completed = query.value(1).toInt() != 0;
    }

    return true;
}

bool AtlasImporter::saveProgress(const QString &importKey,
                                 const QString &sourceFilePath,
                                 const QString &targetFilePath,
                                 const QString &sourceLang,
                                 const QString &targetLang,
                                 qint64 processedLines,
                                 bool completed)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(R"(
        INSERT OR REPLACE INTO import_progress (
            import_key,
            source_file,
            target_file,
            source_lang,
            target_lang,
            processed_lines,
            completed,
            updated_at
        ) VALUES (
            :import_key,
            :source_file,
            :target_file,
            :source_lang,
            :target_lang,
            :processed_lines,
            :completed,
            CURRENT_TIMESTAMP
        )
    )"));
    query.bindValue(QStringLiteral(":import_key"), importKey);
    query.bindValue(QStringLiteral(":source_file"), sourceFilePath);
    query.bindValue(QStringLiteral(":target_file"), targetFilePath);
    query.bindValue(QStringLiteral(":source_lang"), sourceLang);
    query.bindValue(QStringLiteral(":target_lang"), targetLang);
    query.bindValue(QStringLiteral(":processed_lines"), processedLines);
    query.bindValue(QStringLiteral(":completed"), completed ? 1 : 0);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }

    return true;
}

bool AtlasImporter::skipLines(QTextStream &sourceStream, QTextStream &targetStream, qint64 linesToSkip)
{
    for (qint64 line = 0; line < linesToSkip; ++line) {
        if (sourceStream.atEnd() || targetStream.atEnd()) {
            m_lastError = QStringLiteral("Progresso salvo aponta para além do fim do dataset.");
            return false;
        }

        sourceStream.readLine();
        targetStream.readLine();
    }

    return true;
}

QString AtlasImporter::importKey(const QString &sourceFilePath,
                                 const QString &targetFilePath,
                                 const QString &sourceLang,
                                 const QString &targetLang) const
{
    const QString rawKey = QStringLiteral("%1|%2|%3|%4")
                               .arg(QFileInfo(sourceFilePath).absoluteFilePath(),
                                    QFileInfo(targetFilePath).absoluteFilePath(),
                                    sourceLang,
                                    targetLang);
    return QString::fromLatin1(QCryptographicHash::hash(rawKey.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString AtlasImporter::cleanText(const QString &text) const
{
    static const TextNormalizer normalizer;
    return normalizer.normalizeWhitespace(text);
}

QString AtlasImporter::normalizedSourceText(const QString &text) const
{
    static const TextNormalizer normalizer;
    return normalizer.normalizeForLookup(text);
}

void AtlasImporter::printProgress() const
{
    QTextStream console(stdout);
    configureUtf8Stream(console);
    console << "Linhas retomadas: " << m_stats.resumedLines << Qt::endl;
    console << "Linhas processadas: " << m_stats.processedLines << Qt::endl;
    console << "Inseridas: " << m_stats.insertedLines << Qt::endl;
    console << "Ignoradas: " << m_stats.ignoredLines << Qt::endl;
}
