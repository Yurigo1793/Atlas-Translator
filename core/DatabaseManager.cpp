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

bool isSupportedPair(const LanguageNormalizer &normalizer, const QString &sourceLang, const QString &targetLang)
{
    return normalizer.isSupported(sourceLang) && normalizer.isSupported(targetLang);
}
}

DatabaseManager::DatabaseManager(const QString &databasePath)
    : m_databasePath(databasePath),
      m_connectionName(QStringLiteral("atlas_connection_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
    if (m_databasePath.isEmpty()) {
        m_databasePath = QStringLiteral(":memory:");
    }

    if (m_databasePath != QStringLiteral(":memory:")) {
        m_databasePath = QDir::cleanPath(QFileInfo(m_databasePath).absoluteFilePath());
    }
}

DatabaseManager::~DatabaseManager()
{
    close();
}

void DatabaseManager::close()
{
    if (m_database.isValid()) {
        m_database.close();
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool DatabaseManager::open()
{
    if (m_database.isOpen()) {
        return true;
    }

    if (m_database.isValid()) {
        close();
    }

    if (m_databasePath != QStringLiteral(":memory:")) {
        const QFileInfo databaseInfo(m_databasePath);
        QDir directory = databaseInfo.dir();
        if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
            m_lastError = QStringLiteral("Nao foi possivel criar diretorio do banco: %1").arg(directory.absolutePath());
            return false;
        }
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

    discoverPeerDatabases();

    return true;
}

bool DatabaseManager::discoverPeerDatabases()
{
    m_peerDatabasePaths.clear();
    const QDir databaseDirectory(AppPaths::databasePath());
    const QFileInfo currentInfo(m_databasePath);
    const QFileInfoList databases = databaseDirectory.entryInfoList(QStringList{QStringLiteral("Atlas_*.db")},
                                                                    QDir::Files | QDir::Readable,
                                                                    QDir::Name);
    for (const QFileInfo &databaseInfo : databases) {
        if (databaseInfo.absoluteFilePath() == currentInfo.absoluteFilePath()) {
            continue;
        }
        m_peerDatabasePaths.append(databaseInfo.absoluteFilePath());
    }

    return true;
}

bool DatabaseManager::initialize()
{
    if (!open()) {
        return false;
    }

    if (!createTables()
        || !removeLegacyUniqueConstraint()
        || !ensureFrequencySchema()
        || !ensureTranslationMetadataSchema()
        || !normalizeStoredLanguages()
        || !createIndexes()) {
        return false;
    }

    const DatabaseSummary summary = databaseSummary();
    if (!summary.tableExists) {
        m_lastError = QStringLiteral("A tabela de traducoes nao foi criada.");
        return false;
    }

    if (!summary.indexesOk) {
        m_lastError = QStringLiteral("Os indices da tabela de traducoes nao foram carregados corretamente.");
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
        m_lastError = QStringLiteral("SQLite nao esta aberto antes da consulta.");
        return results;
    }

    if (!m_languageNormalizer.isSupported(sourceLang)
        || !m_languageNormalizer.isSupported(targetLang)) {
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

    auto runQuery = [&](QSqlDatabase database) {
        QSqlQuery query(database);
        if (!query.prepare(QStringLiteral(R"(
            SELECT source_text, translated_text, frequency
            FROM translations
            WHERE source_lang IN (%2)
              AND target_lang IN (%3)
              AND source_text IN (%1)
            ORDER BY source_text ASC, frequency DESC, LENGTH(translated_text) ASC, id ASC
        )").arg(sourceTextPlaceholders.join(QStringLiteral(", ")),
                 sourceLangPlaceholders.join(QStringLiteral(", ")),
                 targetLangPlaceholders.join(QStringLiteral(", "))))) {
            return;
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
            return;
        }

        while (query.next()) {
            const QString sourceTextKey = query.value(0).toString();
            if (!results.contains(sourceTextKey)) {
                results.insert(sourceTextKey, query.value(1).toString());
            }
        }
        recordSqlQueryTime(timer.nsecsElapsed());
    };

    runQuery(m_database);
    for (const QString &databasePath : m_peerDatabasePaths) {
        const QString connectionName = QStringLiteral("atlas_lookup_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSqlDatabase peerDatabase = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        peerDatabase.setDatabaseName(databasePath);
        if (peerDatabase.open()) {
            runQuery(peerDatabase);
        }
        peerDatabase.close();
        peerDatabase = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
        if (results.size() >= normalizedSources.size()) {
            break;
        }
    }

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

    auto collectSupportedTotals = [&](QSqlDatabase database) {
        QSqlQuery totalsQuery(database);
        if (totalsQuery.exec(QStringLiteral(R"(
            SELECT source_lang, target_lang, COUNT(*), COALESCE(SUM(frequency), 0)
            FROM translations
            GROUP BY source_lang, target_lang
        )"))) {
            while (totalsQuery.next()) {
                const QString sourceLang = totalsQuery.value(0).toString();
                const QString targetLang = totalsQuery.value(1).toString();
                if (!isSupportedPair(m_languageNormalizer, sourceLang, targetLang)) {
                    continue;
                }
                summary.translationCount += totalsQuery.value(2).toLongLong();
                summary.totalFrequency += totalsQuery.value(3).toLongLong();
            }
        }
    };

    collectSupportedTotals(m_database);

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
    )"))) {
        while (languageQuery.next()) {
            const QString sourceLang = languageQuery.value(0).toString();
            const QString targetLang = languageQuery.value(1).toString();
            if (!isSupportedPair(m_languageNormalizer, sourceLang, targetLang)) {
                continue;
            }
            summary.languagePairs.append(QStringLiteral("%1 -> %2 (%3 pairs, frequency %4)")
                                             .arg(sourceLang,
                                                  targetLang,
                                                  languageQuery.value(2).toString(),
                                                  languageQuery.value(3).toString()));
        }
    }

    for (const QString &databasePath : m_peerDatabasePaths) {
        const QString connectionName = QStringLiteral("atlas_summary_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSqlDatabase peerDatabase = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        peerDatabase.setDatabaseName(databasePath);
        if (peerDatabase.open()) {
            collectSupportedTotals(peerDatabase);
            QSqlQuery peerLanguages(peerDatabase);
            if (peerLanguages.exec(QStringLiteral(R"(
                SELECT source_lang, target_lang, COUNT(*), COALESCE(SUM(frequency), 0)
                FROM translations
                GROUP BY source_lang, target_lang
                ORDER BY source_lang, target_lang
            )"))) {
                while (peerLanguages.next()) {
                    const QString sourceLang = peerLanguages.value(0).toString();
                    const QString targetLang = peerLanguages.value(1).toString();
                    if (!isSupportedPair(m_languageNormalizer, sourceLang, targetLang)) {
                        continue;
                    }
                    summary.languagePairs.append(QStringLiteral("%1 -> %2 (%3 pairs, frequency %4)")
                                                     .arg(sourceLang,
                                                          targetLang,
                                                          peerLanguages.value(2).toString(),
                                                          peerLanguages.value(3).toString()));
                }
            }
        }
        peerDatabase.close();
        peerDatabase = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }
    summary.languagePairs.removeDuplicates();

    return summary;
}


QStringList DatabaseManager::availableLanguages() const
{
    QStringList languages;
    if (!m_database.isOpen() || !tableExists(QStringLiteral("translations"))) {
        return languages;
    }

    auto collectLanguages = [&](QSqlDatabase database) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral(R"(
            SELECT DISTINCT language FROM (
                SELECT source_lang AS language FROM translations
                UNION
                SELECT target_lang AS language FROM translations
            )
            WHERE language IS NOT NULL AND language != ''
            ORDER BY language
        )"))) {
            return;
        }
        while (query.next()) {
            const QString language = m_languageNormalizer.normalize(query.value(0).toString());
            if (m_languageNormalizer.isSupported(language) && !languages.contains(language)) {
                languages.append(language);
            }
        }
    };

    collectLanguages(m_database);
    for (const QString &databasePath : m_peerDatabasePaths) {
        const QString connectionName = QStringLiteral("atlas_languages_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSqlDatabase peerDatabase = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        peerDatabase.setDatabaseName(databasePath);
        if (peerDatabase.open()) {
            collectLanguages(peerDatabase);
        }
        peerDatabase.close();
        peerDatabase = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }
    std::sort(languages.begin(), languages.end());

    return languages;
}

QList<DatabaseManager::LanguagePair> DatabaseManager::availableLanguagePairs() const
{
    QList<LanguagePair> pairs;
    if (!m_database.isOpen() || !tableExists(QStringLiteral("translations"))) {
        return pairs;
    }

    QHash<QString, qint64> countsByPair;
    auto collectPairs = [&](QSqlDatabase database) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral(R"(
            SELECT source_lang, target_lang, COUNT(*)
            FROM translations
            GROUP BY source_lang, target_lang
            ORDER BY source_lang, target_lang
        )"))) {
            return;
        }
        while (query.next()) {
            const QString source = m_languageNormalizer.normalize(query.value(0).toString());
            const QString target = m_languageNormalizer.normalize(query.value(1).toString());
            if (!isSupportedPair(m_languageNormalizer, source, target)) {
                continue;
            }
            countsByPair[QStringLiteral("%1\n%2").arg(source, target)] += query.value(2).toLongLong();
        }
    };

    collectPairs(m_database);
    for (const QString &databasePath : m_peerDatabasePaths) {
        const QString connectionName = QStringLiteral("atlas_pairs_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSqlDatabase peerDatabase = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        peerDatabase.setDatabaseName(databasePath);
        if (peerDatabase.open()) {
            collectPairs(peerDatabase);
        }
        peerDatabase.close();
        peerDatabase = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }

    QStringList keys = countsByPair.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString &key : keys) {
        const QStringList parts = key.split(QLatin1Char('\n'));
        if (parts.size() == 2) {
            pairs.append(LanguagePair{parts.at(0), parts.at(1), countsByPair.value(key)});
        }
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
    if (!m_languageNormalizer.isSupported(sourceLang)
        || !m_languageNormalizer.isSupported(targetLang)
        || sourceLanguageVariants.isEmpty()
        || targetLanguageVariants.isEmpty()) {
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

    auto checkPair = [&](QSqlDatabase database) {
        QSqlQuery query(database);
        if (!query.prepare(QStringLiteral(R"(
            SELECT 1
            FROM translations
            WHERE source_lang IN (%1)
              AND target_lang IN (%2)
            LIMIT 1
        )").arg(sourceLangPlaceholders.join(QStringLiteral(", ")),
                 targetLangPlaceholders.join(QStringLiteral(", "))))) {
            return false;
        }

        for (qsizetype index = 0; index < sourceLanguageVariants.size(); ++index) {
            query.bindValue(sourceLangPlaceholders.at(index), sourceLanguageVariants.at(index));
        }
        for (qsizetype index = 0; index < targetLanguageVariants.size(); ++index) {
            query.bindValue(targetLangPlaceholders.at(index), targetLanguageVariants.at(index));
        }

        return query.exec() && query.next();
    };

    if (checkPair(m_database)) {
        return true;
    }
    for (const QString &databasePath : m_peerDatabasePaths) {
        const QString connectionName = QStringLiteral("atlas_has_pair_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSqlDatabase peerDatabase = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        peerDatabase.setDatabaseName(databasePath);
        const bool found = peerDatabase.open() && checkPair(peerDatabase);
        peerDatabase.close();
        peerDatabase = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
        if (found) {
            return true;
        }
    }
    return false;
}

void DatabaseManager::printDatabaseSummary(QTextStream &output) const
{
    configureUtf8Stream(output);
    const DatabaseSummary summary = databaseSummary();

    output << "Banco carregado" << Qt::endl;
    output << "Caminho do banco: " << summary.databasePath << Qt::endl;
    output << "SQLite aberto: " << (summary.sqliteOpened ? QStringLiteral("sim") : QStringLiteral("nao")) << Qt::endl;
    output << "Total de traducoes: " << summary.translationCount << Qt::endl;
    output << "Frequencia total: " << summary.totalFrequency << Qt::endl;
    output << "Indices carregados: " << (summary.indexesOk ? QStringLiteral("sim") : QStringLiteral("nao")) << Qt::endl;
    if (summary.translationCount == 0) {
        output << "Aviso: nenhum banco Atlas de traducao pronto foi encontrado na pasta database." << Qt::endl;
    }
    const QStringList languages = availableLanguages();
    output << "Idiomas disponiveis:" << Qt::endl;
    if (languages.isEmpty()) {
        output << "- nenhum" << Qt::endl;
    } else {
        for (qsizetype index = 0; index < languages.size(); ++index) {
            output << "[" << index << "] " << languages.at(index) << Qt::endl;
        }
    }
    output << "Pares encontrados:" << Qt::endl;
    if (summary.languagePairs.isEmpty()) {
        output << "- nenhum" << Qt::endl;
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
            sense_gloss TEXT NOT NULL DEFAULT '',
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
    bool hasSenseGloss = false;
    QSqlQuery tableInfo(m_database);
    if (!tableInfo.exec(QStringLiteral("PRAGMA table_info(translations)"))) {
        m_lastError = tableInfo.lastError().text();
        return false;
    }
    while (tableInfo.next()) {
        const QString columnName = tableInfo.value(1).toString();
        if (columnName == QStringLiteral("frequency")) {
            hasFrequency = true;
        } else if (columnName == QStringLiteral("sense_gloss")) {
            hasSenseGloss = true;
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

    qint64 duplicateRows = 0;
    QSqlQuery duplicateGroupsQuery(m_database);
    if (duplicateGroupsQuery.exec(QStringLiteral(R"(
        SELECT COALESCE(SUM(group_count - 1), 0)
        FROM (
            SELECT COUNT(*) AS group_count
            FROM translations
            GROUP BY source_text, translated_text, source_lang, target_lang%1
            HAVING COUNT(*) > 1
        )
    )").arg(hasSenseGloss ? QStringLiteral(", sense_gloss") : QString())) && duplicateGroupsQuery.next()) {
        duplicateRows = duplicateGroupsQuery.value(0).toLongLong();
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
    const QString senseGlossColumnDefinition = hasSenseGloss
        ? QStringLiteral("sense_gloss TEXT NOT NULL DEFAULT '',")
        : QString();
    const QString senseGlossInsertColumn = hasSenseGloss ? QStringLiteral(", sense_gloss") : QString();
    const QString senseGlossSelectColumn = hasSenseGloss ? QStringLiteral(", sense_gloss") : QString();
    const QString senseGlossGroupColumn = hasSenseGloss ? QStringLiteral(", sense_gloss") : QString();
    const QStringList migrationStatements = {
        QStringLiteral("DROP TABLE IF EXISTS translations_frequency_migration"),
        QStringLiteral(R"(
            CREATE TABLE translations_frequency_migration (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                source_text TEXT NOT NULL,
                translated_text TEXT NOT NULL,
                source_lang TEXT NOT NULL,
                target_lang TEXT NOT NULL,
                %1
                frequency INTEGER NOT NULL DEFAULT 1
            )
        )").arg(senseGlossColumnDefinition),
        QStringLiteral(R"(
            INSERT INTO translations_frequency_migration (source_text, translated_text, source_lang, target_lang%1, frequency)
            SELECT source_text, translated_text, source_lang, target_lang%2, SUM(%3)
            FROM translations
            GROUP BY source_text, translated_text, source_lang, target_lang%4
        )").arg(senseGlossInsertColumn,
                senseGlossSelectColumn,
                frequencyExpression,
                senseGlossGroupColumn),
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

bool DatabaseManager::ensureTranslationMetadataSchema()
{
    if (!tableExists(QStringLiteral("translations"))) {
        return createTables();
    }

    bool hasSenseGloss = false;
    QSqlQuery tableInfo(m_database);
    if (!tableInfo.exec(QStringLiteral("PRAGMA table_info(translations)"))) {
        m_lastError = tableInfo.lastError().text();
        return false;
    }
    while (tableInfo.next()) {
        if (tableInfo.value(1).toString() == QStringLiteral("sense_gloss")) {
            hasSenseGloss = true;
            break;
        }
    }
    tableInfo.finish();

    if (!hasSenseGloss) {
        QSqlQuery alterQuery(m_database);
        if (!alterQuery.exec(QStringLiteral("ALTER TABLE translations ADD COLUMN sense_gloss TEXT NOT NULL DEFAULT ''"))) {
            m_lastError = alterQuery.lastError().text();
            return false;
        }
        alterQuery.finish();
    }

    QStringList indexedColumns;
    QSqlQuery indexInfo(m_database);
    if (indexInfo.exec(QStringLiteral("PRAGMA index_info(idx_translations_pair_unique)"))) {
        while (indexInfo.next()) {
            indexedColumns.append(indexInfo.value(2).toString());
        }
        indexInfo.finish();
    } else {
        m_lastError = indexInfo.lastError().text();
        return false;
    }

    const QStringList expectedColumns = {
        QStringLiteral("source_text"),
        QStringLiteral("translated_text"),
        QStringLiteral("source_lang"),
        QStringLiteral("target_lang"),
        QStringLiteral("sense_gloss")
    };

    if (!indexedColumns.isEmpty() && indexedColumns != expectedColumns) {
        QSqlQuery dropQuery(m_database);
        if (!dropQuery.exec(QStringLiteral("DROP INDEX idx_translations_pair_unique"))) {
            m_lastError = dropQuery.lastError().text();
            return false;
        }
        dropQuery.finish();
    }

    return true;
}

bool DatabaseManager::createIndexes()
{
    const QStringList statements = {
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translations_source_text ON translations(source_text)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translations_source_lang ON translations(source_lang)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translations_target_lang ON translations(target_lang)"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_translations_pair_unique ON translations(source_text, translated_text, source_lang, target_lang, sense_gloss)")
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

bool DatabaseManager::ensureLookupIndex()
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
    schemaQuery.finish();
    if (!tableSql.contains(QStringLiteral("unique(source_text,source_lang,target_lang)"))) {
        return true;
    }

    return ensureFrequencySchema();
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
    selectQuery.finish();

    bool hasLanguageUpdates = false;
    for (const QString &storedLanguage : storedLanguages) {
        const QString normalizedLanguage = m_languageNormalizer.normalize(storedLanguage);
        if (!normalizedLanguage.isEmpty() && normalizedLanguage != storedLanguage) {
            hasLanguageUpdates = true;
            break;
        }
    }

    if (!hasLanguageUpdates) {
        return true;
    }

    QSqlQuery dropPairUnique(m_database);
    if (!dropPairUnique.exec(QStringLiteral("DROP INDEX IF EXISTS idx_translations_pair_unique"))) {
        m_lastError = dropPairUnique.lastError().text();
        return false;
    }
    dropPairUnique.finish();

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
        sourceUpdate.finish();

        QSqlQuery targetUpdate(m_database);
        targetUpdate.prepare(QStringLiteral("UPDATE translations SET target_lang = :normalized WHERE target_lang = :stored"));
        targetUpdate.bindValue(QStringLiteral(":normalized"), normalizedLanguage);
        targetUpdate.bindValue(QStringLiteral(":stored"), storedLanguage);
        if (!targetUpdate.exec()) {
            m_lastError = targetUpdate.lastError().text();
            return false;
        }
        targetUpdate.finish();
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
