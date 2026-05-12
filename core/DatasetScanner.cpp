#include "DatasetScanner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <QStringList>

namespace {
constexpr qint64 MinimumDatasetFileSize = 8;
constexpr double MaximumSizeRatio = 10.0;

QString candidateKey(const QString &corpusName, const QString &langPair)
{
    return QStringLiteral("%1\n%2").arg(corpusName, langPair);
}
}

DatasetScanner::DatasetScanner(const QString &datasetsPath)
    : m_datasetsPath(resolveDatasetsPath(datasetsPath))
{
}

QList<DatasetInfo> DatasetScanner::scan()
{
    m_lastError.clear();

    QList<DatasetInfo> datasets;
    QDir datasetsDirectory(m_datasetsPath);
    if (!datasetsDirectory.exists()) {
        m_lastError = QStringLiteral("Diretório de datasets não encontrado: %1").arg(m_datasetsPath);
        return datasets;
    }

    const QFileInfoList files = datasetsDirectory.entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
    QHash<QString, QList<CandidateFile>> candidatesByPair;

    for (const QFileInfo &fileInfo : files) {
        CandidateFile candidate;
        if (!parseCandidate(fileInfo, candidate)) {
            continue;
        }

        candidatesByPair[candidateKey(candidate.corpusName, candidate.langPair)].append(candidate);
    }

    QSet<QString> emittedDatasets;
    const QList<QString> keys = candidatesByPair.keys();
    for (const QString &key : keys) {
        const QList<CandidateFile> candidates = candidatesByPair.value(key);
        if (candidates.size() < 2) {
            continue;
        }

        const QStringList keyParts = key.split(QLatin1Char('\n'));
        if (keyParts.size() != 2) {
            continue;
        }

        const QString langPair = keyParts.at(1);
        const int separatorIndex = langPair.indexOf(QLatin1Char('-'));
        if (separatorIndex <= 0 || separatorIndex >= langPair.size() - 1) {
            continue;
        }

        const QString sourceLanguage = langPair.left(separatorIndex);
        const QString targetLanguage = langPair.mid(separatorIndex + 1);
        CandidateFile sourceCandidate;
        CandidateFile targetCandidate;
        bool foundSource = false;
        bool foundTarget = false;

        for (const CandidateFile &candidate : candidates) {
            if (candidate.language == sourceLanguage) {
                sourceCandidate = candidate;
                foundSource = true;
            } else if (candidate.language == targetLanguage) {
                targetCandidate = candidate;
                foundTarget = true;
            }
        }

        if (!foundSource || !foundTarget) {
            continue;
        }

        if (!isCoherentPair(sourceCandidate, targetCandidate)) {
            continue;
        }

        const QString datasetKey = QStringLiteral("%1\n%2\n%3")
                                       .arg(sourceCandidate.corpusName,
                                            sourceLanguage,
                                            targetLanguage);
        if (emittedDatasets.contains(datasetKey)) {
            continue;
        }

        emittedDatasets.insert(datasetKey);
        datasets.append(DatasetInfo{
            sourceCandidate.corpusName,
            sourceLanguage,
            targetLanguage,
            sourceCandidate.filePath,
            targetCandidate.filePath
        });
    }

    std::sort(datasets.begin(), datasets.end(), [](const DatasetInfo &left, const DatasetInfo &right) {
        if (left.corpusName != right.corpusName) {
            return left.corpusName < right.corpusName;
        }
        if (left.sourceLanguage != right.sourceLanguage) {
            return left.sourceLanguage < right.sourceLanguage;
        }
        return left.targetLanguage < right.targetLanguage;
    });

    return datasets;
}

QString DatasetScanner::lastError() const
{
    return m_lastError;
}

QString DatasetScanner::datasetsPath() const
{
    return m_datasetsPath;
}

QString DatasetScanner::resolveDatasetsPath(const QString &datasetsPath) const
{
    if (!datasetsPath.trimmed().isEmpty()) {
        return QDir::cleanPath(datasetsPath);
    }

    const QString currentPath = QDir::current().filePath(QStringLiteral("datasets"));
    if (QFileInfo::exists(currentPath)) {
        return QDir::cleanPath(currentPath);
    }

    return QDir::cleanPath(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("datasets")));
}

bool DatasetScanner::parseCandidate(const QFileInfo &fileInfo, CandidateFile &candidate) const
{
    if (!fileInfo.isFile() || !fileInfo.isReadable()) {
        return false;
    }

    if (fileInfo.size() < MinimumDatasetFileSize) {
        return false;
    }

    const QString language = fileInfo.suffix();
    if (!isKnownLanguageToken(language)) {
        return false;
    }

    const QString baseName = fileInfo.completeBaseName();
    const int langPairSeparator = baseName.lastIndexOf(QLatin1Char('.'));
    if (langPairSeparator <= 0 || langPairSeparator >= baseName.size() - 1) {
        return false;
    }

    const QString corpusName = baseName.left(langPairSeparator);
    const QString langPair = baseName.mid(langPairSeparator + 1);
    const int pairSeparator = langPair.indexOf(QLatin1Char('-'));
    if (pairSeparator <= 0 || pairSeparator >= langPair.size() - 1) {
        return false;
    }

    const QString sourceLanguage = langPair.left(pairSeparator);
    const QString targetLanguage = langPair.mid(pairSeparator + 1);
    if (language != sourceLanguage && language != targetLanguage) {
        return false;
    }

    if (!isKnownLanguageToken(sourceLanguage) || !isKnownLanguageToken(targetLanguage)) {
        return false;
    }

    if (!hasReadableContent(fileInfo.absoluteFilePath())) {
        return false;
    }

    candidate.corpusName = corpusName;
    candidate.langPair = langPair;
    candidate.language = language;
    candidate.filePath = fileInfo.absoluteFilePath();
    candidate.size = fileInfo.size();
    return true;
}

bool DatasetScanner::isKnownLanguageToken(const QString &language) const
{
    static const QRegularExpression languageExpression(QStringLiteral("^[A-Za-z]{2,3}(?:_[A-Za-z0-9]{2,8})?$"));
    return languageExpression.match(language).hasMatch();
}

bool DatasetScanner::isCoherentPair(const CandidateFile &sourceCandidate,
                                    const CandidateFile &targetCandidate) const
{
    if (!QFileInfo::exists(sourceCandidate.filePath) || !QFileInfo::exists(targetCandidate.filePath)) {
        return false;
    }

    if (sourceCandidate.size < MinimumDatasetFileSize || targetCandidate.size < MinimumDatasetFileSize) {
        return false;
    }

    const qint64 largerSize = qMax(sourceCandidate.size, targetCandidate.size);
    const qint64 smallerSize = qMin(sourceCandidate.size, targetCandidate.size);
    if (smallerSize <= 0) {
        return false;
    }

    return (static_cast<double>(largerSize) / static_cast<double>(smallerSize)) <= MaximumSizeRatio;
}

bool DatasetScanner::hasReadableContent(const QString &filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    const QByteArray firstLine = file.readLine(1024);
    return !QString::fromUtf8(firstLine).trimmed().isEmpty();
}
