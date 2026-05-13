#include "AtlasImporter.h"

#include "TextNormalizer.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringConverter>
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
        m_databasePath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("database/atlas.db"));
    }
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
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    sourceStream.setEncoding(QStringConverter::Utf8);
    targetStream.setEncoding(QStringConverter::Utf8);
#endif

    QTextStream console(stdout);
    console << "Importando..." << Qt::endl;
    console << "Idiomas: " << normalizedSourceLang << " -> " << normalizedTargetLang << Qt::endl;

    qint64 resumeFromLine = 0;
    bool completed = false;
    if (!loadProgress(currentImportKey, resumeFromLine, completed)) {
        return false;
    }

    if (completed) {
        console << "Dataset já importado anteriormente. Nada a fazer." << Qt::endl;
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

        if (!shouldImportLine(sourceText, targetText)) {
            ++m_stats.ignoredLines;
        } else {
            insertQuery.bindValue(QStringLiteral(":source_text"), normalizedSourceText(sourceText));
            insertQuery.bindValue(QStringLiteral(":translated_text"), targetText);
            insertQuery.bindValue(QStringLiteral(":source_lang"), normalizedSourceLang);
            insertQuery.bindValue(QStringLiteral(":target_lang"), normalizedTargetLang);

            if (!insertQuery.exec()) {
                m_lastError = insertQuery.lastError().text();
                success = false;
                break;
            }

            if (insertQuery.numRowsAffected() > 0) {
                ++m_stats.insertedLines;
            } else {
                ++m_stats.duplicateLines;
                ++m_stats.ignoredLines;
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

    printProgress();
    console << "Resumo final:" << Qt::endl;
    console << "Linhas retomadas: " << m_stats.resumedLines << Qt::endl;
    console << "Linhas processadas: " << m_stats.processedLines << Qt::endl;
    console << "Inseridas: " << m_stats.insertedLines << Qt::endl;
    console << "Ignoradas: " << m_stats.ignoredLines << Qt::endl;
    console << "Duplicadas: " << m_stats.duplicateLines << Qt::endl;

    return success;
}

AtlasImporter::ImportStats AtlasImporter::stats() const
{
    return m_stats;
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
            UNIQUE(source_text, source_lang, target_lang)
        )
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    return ensureIndexes() && ensureProgressTable();
}

bool AtlasImporter::ensureIndexes()
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
        INSERT OR IGNORE INTO translations (source_text, translated_text, source_lang, target_lang)
        VALUES (:source_text, :translated_text, :source_lang, :target_lang)
    )"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    return true;
}

bool AtlasImporter::shouldImportLine(const QString &sourceText, const QString &targetText) const
{
    if (sourceText.isEmpty() || targetText.isEmpty()) {
        return false;
    }

    if (sourceText.size() > MaximumLineLength || targetText.size() > MaximumLineLength) {
        return false;
    }

    return sourceText.size() >= MinimumLineLength && targetText.size() >= MinimumLineLength;
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
    console << "Linhas retomadas: " << m_stats.resumedLines << Qt::endl;
    console << "Linhas processadas: " << m_stats.processedLines << Qt::endl;
    console << "Inseridas: " << m_stats.insertedLines << Qt::endl;
    console << "Ignoradas: " << m_stats.ignoredLines << Qt::endl;
}
