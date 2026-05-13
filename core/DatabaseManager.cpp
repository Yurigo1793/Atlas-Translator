#include "DatabaseManager.h"

#include "TextNormalizer.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QVariant>
#include <QUuid>

DatabaseManager::DatabaseManager(const QString &databasePath)
    : m_databasePath(databasePath),
      m_connectionName(QStringLiteral("atlas_connection_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
    if (m_databasePath.isEmpty()) {
        m_databasePath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("database/atlas.db"));
    }
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

    return true;
}

bool DatabaseManager::initialize()
{
    if (!m_database.isOpen() && !open()) {
        return false;
    }

    if (!createTables() || !createIndexes() || !normalizeStoredLanguages() || !seedInitialTranslations()) {
        return false;
    }

    QTextStream output(stdout);
    printDatabaseSummary(output);
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
        SELECT source_text, translated_text
        FROM translations INDEXED BY idx_translations_lookup
        WHERE source_text IN (%1)
          AND source_lang IN (%2)
          AND target_lang IN (%3)
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
    summary.tableExists = tableExists(QStringLiteral("translations"));
    if (!summary.tableExists) {
        return summary;
    }

    QSqlQuery countQuery(m_database);
    if (countQuery.exec(QStringLiteral("SELECT COUNT(*) FROM translations")) && countQuery.next()) {
        summary.translationCount = countQuery.value(0).toLongLong();
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
        && summary.indexes.contains(QStringLiteral("idx_translations_lookup"));

    QSqlQuery languageQuery(m_database);
    if (languageQuery.exec(QStringLiteral(R"(
        SELECT source_lang, target_lang, COUNT(*)
        FROM translations
        GROUP BY source_lang, target_lang
        ORDER BY source_lang, target_lang
        LIMIT 20
    )"))) {
        while (languageQuery.next()) {
            summary.languagePairs.append(QStringLiteral("%1 -> %2 (%3)")
                                             .arg(languageQuery.value(0).toString(),
                                                  languageQuery.value(1).toString(),
                                                  languageQuery.value(2).toString()));
        }
    }

    return summary;
}

void DatabaseManager::printDatabaseSummary(QTextStream &output) const
{
    const DatabaseSummary summary = databaseSummary();

    output << "Database loaded" << Qt::endl;
    output << "Path: " << m_databasePath << Qt::endl;
    output << "Translations: " << summary.translationCount << Qt::endl;
    output << "Indexes: " << (summary.indexesOk ? QStringLiteral("OK") : QStringLiteral("MISSING")) << Qt::endl;
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
            UNIQUE(source_text, source_lang, target_lang)
        )
    )"));

    if (!ok) {
        m_lastError = query.lastError().text();
    }

    return ok;
}

bool DatabaseManager::createIndexes()
{
    const QStringList statements = {
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translations_source_text ON translations(source_text)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translations_source_lang ON translations(source_lang)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translations_target_lang ON translations(target_lang)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translations_lookup ON translations(source_text, source_lang, target_lang)")
    };

    for (const QString &statement : statements) {
        QSqlQuery query(m_database);
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            return false;
        }
    }

    return true;
}

bool DatabaseManager::seedInitialTranslations()
{
    return insertTranslation(QStringLiteral("hello"), QStringLiteral("olá"), QStringLiteral("en"), QStringLiteral("pt_BR"))
        && insertTranslation(QStringLiteral("world"), QStringLiteral("mundo"), QStringLiteral("en"), QStringLiteral("pt_BR"))
        && insertTranslation(QStringLiteral("good morning"), QStringLiteral("bom dia"), QStringLiteral("en"), QStringLiteral("pt_BR"))
        && insertTranslation(QStringLiteral("how are you"), QStringLiteral("como você está"), QStringLiteral("en"), QStringLiteral("pt_BR"))
        && insertTranslation(QStringLiteral("open"), QStringLiteral("abrir"), QStringLiteral("en"), QStringLiteral("pt_BR"))
        && insertTranslation(QStringLiteral("file"), QStringLiteral("arquivo"), QStringLiteral("en"), QStringLiteral("pt_BR"))
        && insertTranslation(QStringLiteral("save"), QStringLiteral("salvar"), QStringLiteral("en"), QStringLiteral("pt_BR"))
        && insertTranslation(QStringLiteral("delete"), QStringLiteral("excluir"), QStringLiteral("en"), QStringLiteral("pt_BR"))
        && insertTranslation(QStringLiteral("network settings"), QStringLiteral("configurações de rede"), QStringLiteral("en"), QStringLiteral("pt_BR"))
        && insertTranslation(QStringLiteral("open file"), QStringLiteral("abrir arquivo"), QStringLiteral("en"), QStringLiteral("pt_BR"))
        && insertTranslation(QStringLiteral("open the file"), QStringLiteral("abrir o arquivo"), QStringLiteral("en"), QStringLiteral("pt_BR"));
}

bool DatabaseManager::insertTranslation(const QString &sourceText,
                                        const QString &translatedText,
                                        const QString &sourceLang,
                                        const QString &targetLang)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(R"(
        INSERT OR IGNORE INTO translations (source_text, translated_text, source_lang, target_lang)
        VALUES (:source_text, :translated_text, :source_lang, :target_lang)
    )"));
    query.bindValue(QStringLiteral(":source_text"), normalizedText(sourceText));
    query.bindValue(QStringLiteral(":translated_text"), translatedText.trimmed());
    query.bindValue(QStringLiteral(":source_lang"), m_languageNormalizer.normalize(sourceLang));
    query.bindValue(QStringLiteral(":target_lang"), m_languageNormalizer.normalize(targetLang));

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }

    return true;
}

bool DatabaseManager::normalizeStoredLanguages()
{
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
        sourceUpdate.prepare(QStringLiteral("UPDATE OR IGNORE translations SET source_lang = :normalized WHERE source_lang = :stored"));
        sourceUpdate.bindValue(QStringLiteral(":normalized"), normalizedLanguage);
        sourceUpdate.bindValue(QStringLiteral(":stored"), storedLanguage);
        if (!sourceUpdate.exec()) {
            m_lastError = sourceUpdate.lastError().text();
            return false;
        }

        QSqlQuery targetUpdate(m_database);
        targetUpdate.prepare(QStringLiteral("UPDATE OR IGNORE translations SET target_lang = :normalized WHERE target_lang = :stored"));
        targetUpdate.bindValue(QStringLiteral(":normalized"), normalizedLanguage);
        targetUpdate.bindValue(QStringLiteral(":stored"), storedLanguage);
        if (!targetUpdate.exec()) {
            m_lastError = targetUpdate.lastError().text();
            return false;
        }
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
