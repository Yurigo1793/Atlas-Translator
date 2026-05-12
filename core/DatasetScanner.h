#ifndef DATASETSCANNER_H
#define DATASETSCANNER_H

#include <QList>
#include <QtGlobal>
#include <QString>

struct DatasetInfo
{
    QString corpusName;
    QString sourceLanguage;
    QString targetLanguage;
    QString sourceFile;
    QString targetFile;
};

class QFileInfo;

class DatasetScanner
{
public:
    explicit DatasetScanner(const QString &datasetsPath = QString());

    QList<DatasetInfo> scan();
    QString lastError() const;
    QString datasetsPath() const;

private:
    struct CandidateFile {
        QString corpusName;
        QString langPair;
        QString language;
        QString filePath;
        qint64 size = 0;
    };

    QString resolveDatasetsPath(const QString &datasetsPath) const;
    bool parseCandidate(const QFileInfo &fileInfo, CandidateFile &candidate) const;
    bool isKnownLanguageToken(const QString &language) const;
    bool isCoherentPair(const CandidateFile &sourceCandidate,
                        const CandidateFile &targetCandidate) const;
    bool hasReadableContent(const QString &filePath) const;

    QString m_datasetsPath;
    QString m_lastError;
};

#endif // DATASETSCANNER_H
