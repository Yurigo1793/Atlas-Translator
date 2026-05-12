#include "DatabaseManager.h"

#include "TextNormalizer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
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

    return createTables() && createIndexes() && seedInitialTranslations();
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

    QStringList placeholders;
    placeholders.reserve(normalizedSources.size());
    for (qsizetype index = 0; index < normalizedSources.size(); ++index) {
        placeholders.append(QStringLiteral(":source_%1").arg(index));
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(R"(
        SELECT source_text, translated_text
        FROM translations
        WHERE source_lang = :source_lang
          AND target_lang = :target_lang
          AND source_text IN (%1)
    )").arg(placeholders.join(QStringLiteral(", "))));

    for (qsizetype index = 0; index < normalizedSources.size(); ++index) {
        query.bindValue(placeholders.at(index), normalizedSources.at(index));
    }
    query.bindValue(QStringLiteral(":source_lang"), sourceLang.toLower());
    query.bindValue(QStringLiteral(":target_lang"), targetLang.toLower());

    if (!query.exec()) {
        return results;
    }

    while (query.next()) {
        results.insert(query.value(0).toString(), query.value(1).toString());
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
    return insertTranslation(QStringLiteral("hello"), QStringLiteral("olá"), QStringLiteral("en"), QStringLiteral("pt"))
        && insertTranslation(QStringLiteral("world"), QStringLiteral("mundo"), QStringLiteral("en"), QStringLiteral("pt"))
        && insertTranslation(QStringLiteral("good morning"), QStringLiteral("bom dia"), QStringLiteral("en"), QStringLiteral("pt"))
        && insertTranslation(QStringLiteral("how are you"), QStringLiteral("como você está"), QStringLiteral("en"), QStringLiteral("pt"))
        && insertTranslation(QStringLiteral("open"), QStringLiteral("abrir"), QStringLiteral("en"), QStringLiteral("pt"))
        && insertTranslation(QStringLiteral("open file"), QStringLiteral("abrir arquivo"), QStringLiteral("en"), QStringLiteral("pt"))
        && insertTranslation(QStringLiteral("open the file"), QStringLiteral("abrir o arquivo"), QStringLiteral("en"), QStringLiteral("pt"));
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
    query.bindValue(QStringLiteral(":source_lang"), sourceLang.toLower());
    query.bindValue(QStringLiteral(":target_lang"), targetLang.toLower());

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }

    return true;
}

QString DatabaseManager::normalizedText(const QString &text) const
{
    static const TextNormalizer normalizer;
    return normalizer.normalizeForLookup(text);
}
