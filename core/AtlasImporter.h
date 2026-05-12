#ifndef ATLASIMPORTER_H
#define ATLASIMPORTER_H

#include <QSqlDatabase>
#include <QString>

class QSqlQuery;

class AtlasImporter
{
public:
    struct ImportStats {
        qint64 processedLines = 0;
        qint64 insertedLines = 0;
        qint64 ignoredLines = 0;
        qint64 duplicateLines = 0;
    };

    explicit AtlasImporter(const QString &databasePath = QString());
    ~AtlasImporter();

    bool importMosesDataset(const QString &sourceFilePath,
                            const QString &targetFilePath,
                            const QString &sourceLang = QStringLiteral("en"),
                            const QString &targetLang = QStringLiteral("pt"));

    ImportStats stats() const;
    QString lastError() const;
    QString databasePath() const;

private:
    bool openDatabase();
    bool ensureSchema();
    bool ensureIndexes();
    bool prepareInsertStatement(QSqlQuery &query);
    bool shouldImportLine(const QString &sourceText, const QString &targetText) const;
    QString cleanText(const QString &text) const;
    QString normalizedSourceText(const QString &text) const;
    void printProgress() const;

    QString m_databasePath;
    QString m_connectionName;
    QSqlDatabase m_database;
    QString m_lastError;
    ImportStats m_stats;
};

#endif // ATLASIMPORTER_H
