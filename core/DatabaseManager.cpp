#include "DatabaseManager.h"

#include "TextNormalizer.h"
#include "AppPaths.h"
#include "Utf8Streams.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QVariant>
#include <QUuid>

namespace {
QString cleanedTableSql(const QVariant &value)
{
    QString tableSql = value.toString().toLower();
    tableSql.remove(QChar::Space);
    tableSql.remove(QChar::Tabulation);
    tableSql.remove(QChar::LineFeed);
    tableSql.remove(QChar::CarriageReturn);
    return tableSql;
}
}

DatabaseManager::DatabaseManager(const QString &databasePath)
    : m_databasePath(databasePath),
      m_connectionName(QStringLiteral("atlas_connection_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
    if (m_databasePath.isEmpty()) {
        m_databasePath = AppPaths::databaseFile();
    }

    m_databasePath = QDir::cleanPath(QFileInfo(m_databasePath).absoluteFilePath());
}

DatabaseManager::~DatabaseManager()
{
    if (m_database.isValid()) {
        m_database.close();
    }

    m_database = QSqlDatabase();

    if (!m_connectionName.isEmpty()) {
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool DatabaseManager::open()
{
    if (m_database.isOpen()) {
        return true;
    }

    if (m_database.isValid()) {
        m_database.close();
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
    }

    const QFileInfo databaseInfo(m_databasePath);
    QDir directory = databaseInfo.dir();
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        m_lastError = QStringLiteral("Não foi possível criar o diretório do banco de dados: %1").arg(directory.absolutePath());
        return false;
    }

    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(m_databasePath);

    if (!m_database.open()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    QSqlQuery pragmas(m_database);
    pragmas.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    pragmas.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    pragmas.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));

    return true;
}

bool DatabaseManager::initialize()
{
    if (!open()) {
        return false;
    }

    if (!createTables() || !ensureFrequencySchema() || !normalizeStoredLanguages() || !createIndexes()) {
        return false;
    }

    const DatabaseSummary summary = databaseSummary();
    if (!summary.tableExists) {
        m_lastError = QStringLiteral("Tabela translations não foi criada.");
        return false;
    }

    if (!summary.indexesOk) {
        m_lastError = QStringLiteral("Índices da tabela translations não foram carregados corretamente.");
        return false;
    }

    return true;
}

std::optional<QString> DatabaseManager::findTranslation(const QString &sourceText,
                                                        const QString &sourceLang,
                                                        const QString &targetLang) const
{
    const QHash<QString, QString> translations = findTranslations(QStringList{sourceText}, sourceLang, targetLang);
    const QString normalizedSourceText = normalizedText(sourceText);
    if (translations.contains(normalizedSourceText)) {
        return translations.value(normalizedSourceText);
    }

    return std::nullopt;
}

QHash<QString, QString> DatabaseManager::findTranslations(const QStringList &sourceTexts,
                                                          const QString &sourceLang,
                                                          const QString &targetLang) const
{
    QHash<QString, QString> results;
    if (!m_database.isOpen()) {
        m_lastError = QStringLiteral("SQLite não está aberto antes da consulta.");
        return results;
    }

    QStringList normalizedSources;
    normalizedSources.reserve(sourceTexts.size());

    for (const QString &sourceText : sourceTexts) {
        const QString normalizedSource = normalizedText(sourceText);
        if (!normalizedSource.isEmpty() && !normalizedSources.contains(normalizedSource)) {
            normalizedSources.append(normalizedSource);
        }
    }

    if (normalizedSources.isEmpty()) {
        return results;
    }

    const QStringList sourceLanguageVariants = m_languageNormalizer.lookupVariants(sourceLang);
    const QStringList targetLanguageVariants = m_languageNormalizer.lookupVariants(targetLang);
    if (sourceLanguageVariants.isEmpty() || targetLanguageVariants.isEmpty()) {
        return results;
    }

    QStringList sourceTextPlaceholders;
    sourceTextPlaceholders.reserve(normalizedSources.size());
    for (qsizetype index = 0; index < normalizedSources.size(); ++index) {
        sourceTextPlaceholders.append(QStringLiteral(":source_text_%1").arg(index));
    }

    QStringList sourceLangPlaceholders;
    sourceLangPlaceholders.reserve(sourceLanguageVariants.size());
    for (qsizetype index = 0; index < sourceLanguageVariants.size(); ++index) {
        sourceLangPlaceholders.append(QStringLiteral(":source_lang_%1").arg(index));
    }

    QStringList targetLangPlaceholders;
    targetLangPlaceholders.reserve(targetLanguageVariants.size());
    for (qsizetype index = 0; index < targetLanguageVariants.size(); ++index) {
        targetLangPlaceholders.append(QStringLiteral(":target_lang_%1").arg(index));
    }

    QSqlQuery query(m_database);
    if (!query.prepare(QStringLiteral(R"(
        SELECT source_text, translated_text, frequency
        FROM translations INDEXED BY idx_translations_lookup
        WHERE source_lang IN (%2)
          AND target_lang IN (%3)
          AND source_text IN (%1)
        ORDER BY source_text ASC, frequency DESC, LENGTH(translated_text) ASC, id ASC
    )").arg(sourceTextPlaceholders.join(QStringLiteral(", ")),
             sourceLangPlaceholders.join(QStringLiteral(", ")),
             targetLangPlaceholders.join(QStringLiteral(", "))))) {
        m_lastError = query.lastError().text();
        return results;
    }

    for (qsizetype index = 0; index < normalizedSources.size(); ++index) {
        query.bindValue(sourceTextPlaceholders.at(index), normalizedSources.at(index));
    }
    for (qsizetype index = 0; index < sourceLanguageVariants.size(); ++index) {
        query.bindValue(sourceLangPlaceholders.at(index), sourceLanguageVariants.at(index));
    }
    for (qsizetype index = 0; index < targetLanguageVariants.size(); ++index) {
        query.bindValue(targetLangPlaceholders.at(index), targetLanguageVariants.at(index));
    }

    QElapsedTimer timer;
    timer.start();
    if (!query.exec()) {
        recordSqlQueryTime(timer.nsecsElapsed());
        m_lastError = query.lastError().text();
        return results;
    }

    while (query.next()) {
        const QString sourceTextKey = query.value(0).toString();
        if (!results.contains(sourceTextKey)) {
            results.insert(sourceTextKey, query.value(1).toString());
        }
    }
    recordSqlQueryTime(timer.nsecsElapsed());

    return results;
}

QString DatabaseManager::lastError() const
{
    return m_lastError;
}

QString DatabaseManager::databasePath() const
{
    return m_databasePath;
}

DatabaseManager::SqlStatistics DatabaseManager::sqlStatistics() const
{
    return m_sqlStatistics;
}

double DatabaseManager::averageSqlQueryTimeMs() const
{
    if (m_sqlStatistics.queryCount == 0) {
        return 0.0;
    }

    return static_cast<double>(m_sqlStatistics.totalQueryTimeNs)
        / static_cast<double>(m_sqlStatistics.queryCount)
        / 1000000.0;
}

DatabaseManager::DatabaseSummary DatabaseManager::databaseSummary() const
{
    DatabaseSummary summary;
    summary.sqliteOpened = m_database.isOpen();
    summary.databasePath = m_databasePath;
    summary.tableExists = tableExists(QStringLiteral("translations"));
    if (!summary.tableExists) {
        return summary;
    }

    QSqlQuery countQuery(m_database);
    if (countQuery.exec(QStringLiteral("SELECT COUNT(*) FROM translations")) && countQuery.next()) {
        summary.translationCount = countQuery.value(0).toLongLong();
    }

    QSqlQuery totalFrequencyQuery(m_database);
    if (totalFrequencyQuery.exec(QStringLiteral("SELECT COALESCE(SUM(frequency), 0) FROM translations"))
        && totalFrequencyQuery.next()) {
        summary.totalFrequency = totalFrequencyQuery.value(0).toLongLong();
    }

    QSqlQuery indexQuery(m_database);
    if (indexQuery.exec(QStringLiteral("PRAGMA index_list(translations)"))) {
        while (indexQuery.next()) {
            summary.indexes.append(indexQuery.value(1).toString());
        }
    }
    summary.indexesOk = summary.indexes.contains(QStringLiteral("idx_translations_source_text"))
        && summary.indexes.contains(QStringLiteral("idx_translations_source_lang"))
        && summary.indexes.contains(QStringLiteral("idx_translations_target_lang"))
        && summary.indexes.contains(QStringLiteral("idx_translations_lookup"))
        && summary.indexes.contains(QStringLiteral("idx_translations_pair_unique"));

    QSqlQuery languageQuery(m_database);
    if (languageQuery.exec(QStringLiteral(R"(
        SELECT source_lang, target_lang, COUNT(*), COALESCE(SUM(frequency), 0)
        FROM translations
        GROUP BY source_lang, target_lang
        ORDER BY source_lang, target_lang
        LIMIT 20
    )"))) {
        while (languageQuery.next()) {
            summary.languagePairs.append(QStringLiteral("%1 -> %2 (%3 pares, frequência %4)")
                                             .arg(languageQuery.value(0).toString(),
                                                  languageQuery.value(1).toString(),
                                                  languageQuery.value(2).toString(),
                                                  languageQuery.value(3).toString()));
        }
    }

    return summary;
}


QStringList DatabaseManager::availableLanguages() const
{
    QStringList languages;
    if (!m_database.isOpen() || !tableExists(QStringLiteral("translations"))) {
        return languages;
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(R"(
        SELECT DISTINCT language FROM (
            SELECT source_lang AS language FROM translations
            UNION
            SELECT target_lang AS language FROM translations
        )
        WHERE language IS NOT NULL AND language != ''
        ORDER BY language
    )"))) {
        m_lastError = query.lastError().text();
        return languages;
    }

    while (query.next()) {
        languages.append(query.value(0).toString());
    }

    return languages;
}

QList<DatabaseManager::LanguagePair> DatabaseManager::availableLanguagePairs() const
{
    QList<LanguagePair> pairs;
    if (!m_database.isOpen() || !tableExists(QStringLiteral("translations"))) {
        return pairs;
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(R"(
        SELECT source_lang, target_lang, COUNT(*)
        FROM translations
        GROUP BY source_lang, target_lang
        ORDER BY source_lang, target_lang
    )"))) {
        m_lastError = query.lastError().text();
        return pairs;
    }

    while (query.next()) {
        pairs.append(LanguagePair{query.value(0).toString(),
                                  query.value(1).toString(),
                                  query.value(2).toLongLong()});
    }

    return pairs;
}

bool DatabaseManager::hasLanguagePair(const QString &sourceLang, const QString &targetLang) const
{
    if (!m_database.isOpen() || !tableExists(QStringLiteral("translations"))) {
        return false;
    }

    const QStringList sourceLanguageVariants = m_languageNormalizer.lookupVariants(sourceLang);
    const QStringList targetLanguageVariants = m_languageNormalizer.lookupVariants(targetLang);
    if (sourceLanguageVariants.isEmpty() || targetLanguageVariants.isEmpty()) {
        return false;
    }

    QStringList sourceLangPlaceholders;
    sourceLangPlaceholders.reserve(sourceLanguageVariants.size());
    for (qsizetype index = 0; index < sourceLanguageVariants.size(); ++index) {
        sourceLangPlaceholders.append(QStringLiteral(":source_lang_%1").arg(index));
    }

    QStringList targetLangPlaceholders;
    targetLangPlaceholders.reserve(targetLanguageVariants.size());
    for (qsizetype index = 0; index < targetLanguageVariants.size(); ++index) {
        targetLangPlaceholders.append(QStringLiteral(":target_lang_%1").arg(index));
    }

    QSqlQuery query(m_database);
    if (!query.prepare(QStringLiteral(R"(
        SELECT 1
        FROM translations
        WHERE source_lang IN (%1)
          AND target_lang IN (%2)
        LIMIT 1
    )").arg(sourceLangPlaceholders.join(QStringLiteral(", ")),
             targetLangPlaceholders.join(QStringLiteral(", "))))) {
        m_lastError = query.lastError().text();
        return false;
    }

    for (qsizetype index = 0; index < sourceLanguageVariants.size(); ++index) {
        query.bindValue(sourceLangPlaceholders.at(index), sourceLanguageVariants.at(index));
    }
    for (qsizetype index = 0; index < targetLanguageVariants.size(); ++index) {
        query.bindValue(targetLangPlaceholders.at(index), targetLanguageVariants.at(index));
    }

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }

    return query.next();
}

void DatabaseManager::printDatabaseSummary(QTextStream &output) const
{
    configureUtf8Stream(output);
    const DatabaseSummary summary = databaseSummary();

    output << "Database loaded" << Qt::endl;
    output << "Database path: " << summary.databasePath << Qt::endl;
    output << "SQLite opened: " << (summary.sqliteOpened ? QStringLiteral("yes") : QStringLiteral("no")) << Qt::endl;
    output << "Translations count: " << summary.translationCount << Qt::endl;
    output << "Total frequency: " << summary.totalFrequency << Qt::endl;
    output << "Indexes loaded: " << (summary.indexesOk ? QStringLiteral("yes") : QStringLiteral("no")) << Qt::endl;
    if (summary.translationCount == 0) {
        output << "Aviso: banco de traduções vazio. Importe um dataset para habilitar traduções." << Qt::endl;
    }
    const QStringList languages = availableLanguages();
    output << "Idiomas disponíveis:" << Qt::endl;
    if (languages.isEmpty()) {
        output << "- none" << Qt::endl;
    } else {
        for (qsizetype index = 0; index < languages.size(); ++index) {
            output << "[" << index << "] " << languages.at(index) << Qt::endl;
        }
    }
    output << "Languages found:" << Qt::endl;
    if (summary.languagePairs.isEmpty()) {
        output << "- none" << Qt::endl;
    } else {
        for (const QString &languagePair : summary.languagePairs) {
            output << "- " << languagePair << Qt::endl;
        }
    }
}

bool DatabaseManager::createTables()
{
    QSqlQuery query(m_database);
    const bool ok = query.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS translations (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source_text TEXT NOT NULL,
            translated_text TEXT NOT NULL,
            source_lang TEXT NOT NULL,
            target_lang TEXT NOT NULL,
            frequency INTEGER NOT NULL DEFAULT 1
        )
    )"));

    if (!ok) {
        m_lastError = query.lastError().text();
    }

    return ok;
}

bool DatabaseManager::ensureFrequencySchema()
{
    if (!tableExists(QStringLiteral("translations"))) {
        return createTables();
    }

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

bool DatabaseManager::createIndexes()
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
    }

    return ensureLookupIndex();
}

bool DatabaseManager::ensureLookupIndex()
{
    QStringList indexedColumns;
    QSqlQuery indexInfo(m_database);
    if (indexInfo.exec(QStringLiteral("PRAGMA index_info(idx_translations_lookup)"))) {
        while (indexInfo.next()) {
            indexedColumns.append(indexInfo.value(2).toString());
        }
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
        indexedColumns.clear();
    }

    if (indexedColumns.isEmpty()) {
        QSqlQuery createQuery(m_database);
        if (!createQuery.exec(QStringLiteral(
                "CREATE INDEX idx_translations_lookup ON translations(source_lang, target_lang, source_text, frequency DESC)"))) {
            m_lastError = createQuery.lastError().text();
            return false;
        }
    }

    return true;
}

bool DatabaseManager::removeLegacyUniqueConstraint()
{
    if (!tableExists(QStringLiteral("translations"))) {
        return true;
    }

    QSqlQuery schemaQuery(m_database);
    schemaQuery.prepare(QStringLiteral("SELECT sql FROM sqlite_master WHERE type = 'table' AND name = :name"));
    schemaQuery.bindValue(QStringLiteral(":name"), QStringLiteral("translations"));
    if (!schemaQuery.exec() || !schemaQuery.next()) {
        m_lastError = schemaQuery.lastError().text();
        return false;
    }

    const QString tableSql = cleanedTableSql(schemaQuery.value(0));
    if (!tableSql.contains(QStringLiteral("unique(source_text,source_lang,target_lang)"))) {
        return true;
    }

    return ensureFrequencySchema();
}

bool DatabaseManager::normalizeStoredLanguages()
{
    QSqlQuery dropPairUnique(m_database);
    if (!dropPairUnique.exec(QStringLiteral("DROP INDEX IF EXISTS idx_translations_pair_unique"))) {
        m_lastError = dropPairUnique.lastError().text();
        return false;
    }

    QSqlQuery selectQuery(m_database);
    if (!selectQuery.exec(QStringLiteral("SELECT DISTINCT source_lang FROM translations UNION SELECT DISTINCT target_lang FROM translations"))) {
        m_lastError = selectQuery.lastError().text();
        return false;
    }

    QStringList storedLanguages;
    while (selectQuery.next()) {
        const QString language = selectQuery.value(0).toString();
        if (!language.isEmpty() && !storedLanguages.contains(language)) {
            storedLanguages.append(language);
        }
    }

    for (const QString &storedLanguage : storedLanguages) {
        const QString normalizedLanguage = m_languageNormalizer.normalize(storedLanguage);
        if (normalizedLanguage.isEmpty() || normalizedLanguage == storedLanguage) {
            continue;
        }

        QSqlQuery sourceUpdate(m_database);
        sourceUpdate.prepare(QStringLiteral("UPDATE translations SET source_lang = :normalized WHERE source_lang = :stored"));
        sourceUpdate.bindValue(QStringLiteral(":normalized"), normalizedLanguage);
        sourceUpdate.bindValue(QStringLiteral(":stored"), storedLanguage);
        if (!sourceUpdate.exec()) {
            m_lastError = sourceUpdate.lastError().text();
            return false;
        }

        QSqlQuery targetUpdate(m_database);
        targetUpdate.prepare(QStringLiteral("UPDATE translations SET target_lang = :normalized WHERE target_lang = :stored"));
        targetUpdate.bindValue(QStringLiteral(":normalized"), normalizedLanguage);
        targetUpdate.bindValue(QStringLiteral(":stored"), storedLanguage);
        if (!targetUpdate.exec()) {
            m_lastError = targetUpdate.lastError().text();
            return false;
        }
    }

    if (!ensureFrequencySchema() || !createIndexes()) {
        return false;
    }

    return true;
}

bool DatabaseManager::tableExists(const QString &tableName) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = :table_name LIMIT 1"));
    query.bindValue(QStringLiteral(":table_name"), tableName);
    return query.exec() && query.next();
}

QString DatabaseManager::normalizedText(const QString &text) const
{
    static const TextNormalizer normalizer;
    return normalizer.normalizeForLookup(text);
}

void DatabaseManager::recordSqlQueryTime(qint64 elapsedNs) const
{
    ++m_sqlStatistics.queryCount;
    m_sqlStatistics.totalQueryTimeNs += elapsedNs;
}
