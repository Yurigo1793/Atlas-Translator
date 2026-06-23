#ifndef DATASETSCANNER_H
#define DATASETSCANNER_H

#include <QList>
#include <QSet>
#include <QtGlobal>
#include <QString>
#include <QStringList>

struct DatasetInfo
{
    enum class SourceType {
        ParallelText,
        OpusPreprocessed,
        FreeDictTei,
        MediaWikiXml,
        MediaWikiPreprocessed
    };

    SourceType sourceType = SourceType::ParallelText;
    QString corpusName;
    QString sourceLanguage;
    QString targetLanguage;
    QString sourceFile;
    QString targetFile;
    QStringList groupedSourceFiles;
    QStringList groupedTargetFiles;
    QStringList groupedSourceLanguages;
    QStringList groupedTargetLanguages;
    QStringList groupedCorpusNames;
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

    struct MediaWikiEvidence {
        QString corpusName;
        QSet<QString> sourceLanguages;
        QSet<QString> targetLanguagesBySourceKey;
    };

    QString resolveDatasetsPath(const QString &datasetsPath) const;
    void recordUnsupportedLanguageItem(const QString &description) const;
    QString unsupportedLanguageSummary() const;
    bool parseCandidate(const QFileInfo &fileInfo, CandidateFile &candidate) const;
    bool hasFreeDictTranslationEvidence(const QString &filePath) const;
    bool inspectMediaWikiEvidence(const QString &filePath, MediaWikiEvidence &evidence) const;
    bool parseLanguagePair(const QString &langPair, QString &sourceLanguage, QString &targetLanguage) const;
    bool isKnownLanguageToken(const QString &language) const;
    QString normalizedLanguageToken(const QString &language) const;
    bool isCoherentPair(const CandidateFile &sourceCandidate,
                        const CandidateFile &targetCandidate) const;
    bool hasReadableContent(const QString &filePath) const;

    QString m_datasetsPath;
    QString m_lastError;
    mutable qsizetype m_unsupportedLanguageItems = 0;
    mutable QStringList m_unsupportedLanguageSamples;
};

#endif // DATASETSCANNER_H
