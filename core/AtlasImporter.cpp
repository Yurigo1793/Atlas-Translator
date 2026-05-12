#include "AtlasImporter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
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

    QSqlQuery insertQuery(m_database);
    if (!prepareInsertStatement(insertQuery)) {
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

    if (!m_database.transaction()) {
        m_lastError = m_database.lastError().text();
        return false;
    }

    bool success = true;
    while (!sourceStream.atEnd() && !targetStream.atEnd()) {
        const QString sourceText = cleanText(sourceStream.readLine());
        const QString targetText = cleanText(targetStream.readLine());
        ++m_stats.processedLines;

        if (!shouldImportLine(sourceText, targetText)) {
            ++m_stats.ignoredLines;
            if ((m_stats.processedLines % ProgressInterval) == 0) {
                printProgress();
            }
            continue;
        }

        insertQuery.bindValue(QStringLiteral(":source_text"), normalizedSourceText(sourceText));
        insertQuery.bindValue(QStringLiteral(":translated_text"), targetText);
        insertQuery.bindValue(QStringLiteral(":source_lang"), sourceLang.toLower());
        insertQuery.bindValue(QStringLiteral(":target_lang"), targetLang.toLower());

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

        if ((m_stats.processedLines % ProgressInterval) == 0) {
            printProgress();
        }
    }

    if (success && (sourceStream.atEnd() != targetStream.atEnd())) {
        m_lastError = QStringLiteral("Dataset desalinhado: os arquivos têm quantidades de linhas diferentes.");
        success = false;
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

    return ensureIndexes();
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

QString AtlasImporter::cleanText(const QString &text) const
{
    QString cleaned = text.simplified();
    cleaned.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return cleaned.trimmed();
}

QString AtlasImporter::normalizedSourceText(const QString &text) const
{
    return cleanText(text).toLower();
}

void AtlasImporter::printProgress() const
{
    QTextStream console(stdout);
    console << "Linhas processadas: " << m_stats.processedLines << Qt::endl;
    console << "Inseridas: " << m_stats.insertedLines << Qt::endl;
    console << "Ignoradas: " << m_stats.ignoredLines << Qt::endl;
}
