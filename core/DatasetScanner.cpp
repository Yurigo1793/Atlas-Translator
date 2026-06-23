#include "DatasetScanner.h"

#include "LanguageNormalizer.h"
#include "AppPaths.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <algorithm>
#include <QStringList>

namespace {
constexpr qint64 MinimumDatasetFileSize = 8;
constexpr int MaximumMediaWikiPagesToInspect = 5000;
constexpr qsizetype CompleteKnownMediaWikiPairCount = 3;

QString mediaWikiTemplateLanguageCode(const QString &languageCode);

QString candidateKey(const QString &corpusName, const QString &langPair)
{
    return QStringLiteral("%1\n%2").arg(corpusName, langPair);
}

QString directoryName(const QFileInfo &fileInfo)
{
    return fileInfo.dir().dirName().toLower();
}

bool isGeneratedOrSupportFile(const QFileInfo &fileInfo)
{
    const QString suffix = fileInfo.suffix().toLower();
    const QString fileName = fileInfo.fileName().toLower();
    const QString completeBaseName = fileInfo.completeBaseName().toLower();

    if (suffix == QStringLiteral("ids")
        || suffix == QStringLiteral("progress")
        || suffix == QStringLiteral("txt")) {
        return true;
    }

    if (fileName.endsWith(QStringLiteral("_metadata.tsv"))) {
        return true;
    }

    if (suffix == QStringLiteral("xml")
        && completeBaseName.contains(QLatin1Char('-'))) {
        return true;
    }

    return false;
}

QString mediaWikiPairKey(const QString &sourceLanguage, const QString &targetLanguage)
{
    return QStringLiteral("%1\n%2").arg(sourceLanguage, targetLanguage);
}

bool parseMediaWikiPreprocessedLanguagePair(const QString &langPair,
                                            QString &sourceLanguage,
                                            QString &targetLanguage)
{
    for (qsizetype index = 1; index < langPair.size() - 1; ++index) {
        if (langPair.at(index) != QLatin1Char('-')) {
            continue;
        }

        const QString sourceCandidate = mediaWikiTemplateLanguageCode(langPair.left(index));
        const QString targetCandidate = mediaWikiTemplateLanguageCode(langPair.mid(index + 1));
        if (!sourceCandidate.isEmpty() && !targetCandidate.isEmpty()) {
            sourceLanguage = sourceCandidate;
            targetLanguage = targetCandidate;
            return true;
        }
    }

    return false;
}

bool parseOpusPreprocessedLanguagePair(const QString &langPair,
                                       QString &sourceLanguage,
                                       QString &targetLanguage)
{
    const LanguageNormalizer normalizer;
    for (qsizetype index = 1; index < langPair.size() - 1; ++index) {
        if (langPair.at(index) != QLatin1Char('-')) {
            continue;
        }

        const QString sourceCandidate = normalizer.normalize(langPair.left(index));
        const QString targetCandidate = normalizer.normalize(langPair.mid(index + 1));
        if (normalizer.isSupported(sourceCandidate)
            && normalizer.isSupported(targetCandidate)) {
            sourceLanguage = sourceCandidate;
            targetLanguage = targetCandidate;
            return true;
        }
    }

    return false;
}

bool parsePreprocessedOpusFileName(const QFileInfo &fileInfo,
                                   QString &corpusName,
                                   QString &sourceLanguage,
                                   QString &targetLanguage)
{
    QString baseName = fileInfo.completeBaseName();
    if (!baseName.endsWith(QStringLiteral("_preprocessed"), Qt::CaseInsensitive)) {
        return false;
    }

    baseName.chop(QStringLiteral("_preprocessed").size());
    for (qsizetype index = baseName.size() - 2; index > 0; --index) {
        if (baseName.at(index) != QLatin1Char('_')) {
            continue;
        }

        QString parsedSource;
        QString parsedTarget;
        if (!parseOpusPreprocessedLanguagePair(baseName.mid(index + 1), parsedSource, parsedTarget)) {
            continue;
        }

        corpusName = baseName.left(index);
        sourceLanguage = parsedSource;
        targetLanguage = parsedTarget;
        return !corpusName.isEmpty() && !sourceLanguage.isEmpty() && !targetLanguage.isEmpty();
    }

    return false;
}

bool parsePreprocessedMediaWikiFileName(const QFileInfo &fileInfo,
                                        QString &corpusName,
                                        QString &sourceLanguage,
                                        QString &targetLanguage)
{
    QString baseName = fileInfo.completeBaseName();
    if (!baseName.endsWith(QStringLiteral("_preprocessed"), Qt::CaseInsensitive)) {
        return false;
    }

    baseName.chop(QStringLiteral("_preprocessed").size());

    for (qsizetype index = 1; index < baseName.size() - 1; ++index) {
        if (baseName.at(index) != QLatin1Char('_')) {
            continue;
        }

        QString parsedSource;
        QString parsedTarget;
        if (!parseMediaWikiPreprocessedLanguagePair(baseName.mid(index + 1), parsedSource, parsedTarget)) {
            continue;
        }

        corpusName = baseName.left(index);
        sourceLanguage = parsedSource;
        targetLanguage = parsedTarget;
        return !corpusName.isEmpty() && !sourceLanguage.isEmpty() && !targetLanguage.isEmpty();
    }

    return false;
}

QHash<QString, QString> mediaWikiHeadingLanguageMap()
{
    return {
        {QStringLiteral("Portuguese"), QStringLiteral("pt_BR")},
        {QStringLiteral("English"), QStringLiteral("en")},
        {QStringLiteral("Spanish"), QStringLiteral("es")},
        {QStringLiteral("French"), QStringLiteral("fr")},
        {QStringLiteral("German"), QStringLiteral("de")},
        {QStringLiteral("Italian"), QStringLiteral("it")},
        {QStringLiteral("Dutch"), QStringLiteral("nl")},
        {QStringLiteral("Russian"), QStringLiteral("ru")},
        {QStringLiteral("Ukrainian"), QStringLiteral("uk")},
        {QStringLiteral("Polish"), QStringLiteral("pl")},
        {QStringLiteral("Czech"), QStringLiteral("cs")},
        {QStringLiteral("Swedish"), QStringLiteral("sv")},
        {QStringLiteral("Norwegian"), QStringLiteral("no")},
        {QStringLiteral("Norwegian Bokmål"), QStringLiteral("nb")},
        {QStringLiteral("Norwegian Nynorsk"), QStringLiteral("nn")},
        {QStringLiteral("Danish"), QStringLiteral("da")},
        {QStringLiteral("Finnish"), QStringLiteral("fi")},
        {QStringLiteral("Greek"), QStringLiteral("el")},
        {QStringLiteral("Turkish"), QStringLiteral("tr")},
        {QStringLiteral("Arabic"), QStringLiteral("ar")},
        {QStringLiteral("Hebrew"), QStringLiteral("he")},
        {QStringLiteral("Persian"), QStringLiteral("fa")},
        {QStringLiteral("Hindi"), QStringLiteral("hi")},
        {QStringLiteral("Urdu"), QStringLiteral("ur")},
        {QStringLiteral("Bengali"), QStringLiteral("bn")},
        {QStringLiteral("Punjabi"), QStringLiteral("pa")},
        {QStringLiteral("Panjabi"), QStringLiteral("pa")},
        {QStringLiteral("Tamil"), QStringLiteral("ta")},
        {QStringLiteral("Telugu"), QStringLiteral("te")},
        {QStringLiteral("Marathi"), QStringLiteral("mr")},
        {QStringLiteral("Gujarati"), QStringLiteral("gu")},
        {QStringLiteral("Chinese"), QStringLiteral("zh")},
        {QStringLiteral("Mandarin"), QStringLiteral("cmn")},
        {QStringLiteral("Cantonese"), QStringLiteral("yue")},
        {QStringLiteral("Wu"), QStringLiteral("wuu")},
        {QStringLiteral("Min Nan"), QStringLiteral("nan")},
        {QStringLiteral("Hakka"), QStringLiteral("hak")},
        {QStringLiteral("Gan"), QStringLiteral("gan")},
        {QStringLiteral("Xiang"), QStringLiteral("hsn")},
        {QStringLiteral("Japanese"), QStringLiteral("ja")},
        {QStringLiteral("Korean"), QStringLiteral("ko")},
        {QStringLiteral("Vietnamese"), QStringLiteral("vi")},
        {QStringLiteral("Thai"), QStringLiteral("th")},
        {QStringLiteral("Indonesian"), QStringLiteral("id")},
        {QStringLiteral("Malay"), QStringLiteral("ms")},
        {QStringLiteral("Tagalog"), QStringLiteral("tl")},
        {QStringLiteral("Filipino"), QStringLiteral("fil")},
        {QStringLiteral("Swahili"), QStringLiteral("sw")},
        {QStringLiteral("Afrikaans"), QStringLiteral("af")},
        {QStringLiteral("Zulu"), QStringLiteral("zu")},
        {QStringLiteral("Xhosa"), QStringLiteral("xh")},
        {QStringLiteral("Yoruba"), QStringLiteral("yo")},
        {QStringLiteral("Hausa"), QStringLiteral("ha")},
        {QStringLiteral("Amharic"), QStringLiteral("am")},
        {QStringLiteral("Pashto"), QStringLiteral("ps")},
        {QStringLiteral("Lahnda"), QStringLiteral("lah")}
    };
}

QHash<QString, QString> mediaWikiTemplateLanguageMap()
{
    return {
        {QStringLiteral("pt"), QStringLiteral("pt_BR")}, {QStringLiteral("pt_br"), QStringLiteral("pt_BR")}, {QStringLiteral("por"), QStringLiteral("pt_BR")},
        {QStringLiteral("en"), QStringLiteral("en")}, {QStringLiteral("eng"), QStringLiteral("en")},
        {QStringLiteral("es"), QStringLiteral("es")}, {QStringLiteral("spa"), QStringLiteral("es")},
        {QStringLiteral("fr"), QStringLiteral("fr")}, {QStringLiteral("fra"), QStringLiteral("fr")}, {QStringLiteral("fre"), QStringLiteral("fr")},
        {QStringLiteral("de"), QStringLiteral("de")}, {QStringLiteral("deu"), QStringLiteral("de")}, {QStringLiteral("ger"), QStringLiteral("de")},
        {QStringLiteral("it"), QStringLiteral("it")}, {QStringLiteral("ita"), QStringLiteral("it")},
        {QStringLiteral("nl"), QStringLiteral("nl")}, {QStringLiteral("nld"), QStringLiteral("nl")}, {QStringLiteral("dut"), QStringLiteral("nl")},
        {QStringLiteral("ru"), QStringLiteral("ru")}, {QStringLiteral("rus"), QStringLiteral("ru")},
        {QStringLiteral("uk"), QStringLiteral("uk")}, {QStringLiteral("ukr"), QStringLiteral("uk")},
        {QStringLiteral("pl"), QStringLiteral("pl")}, {QStringLiteral("pol"), QStringLiteral("pl")},
        {QStringLiteral("cs"), QStringLiteral("cs")}, {QStringLiteral("ces"), QStringLiteral("cs")}, {QStringLiteral("cze"), QStringLiteral("cs")},
        {QStringLiteral("sv"), QStringLiteral("sv")}, {QStringLiteral("swe"), QStringLiteral("sv")},
        {QStringLiteral("no"), QStringLiteral("no")}, {QStringLiteral("nor"), QStringLiteral("no")},
        {QStringLiteral("nb"), QStringLiteral("nb")}, {QStringLiteral("nn"), QStringLiteral("nn")},
        {QStringLiteral("da"), QStringLiteral("da")}, {QStringLiteral("dan"), QStringLiteral("da")},
        {QStringLiteral("fi"), QStringLiteral("fi")}, {QStringLiteral("fin"), QStringLiteral("fi")},
        {QStringLiteral("el"), QStringLiteral("el")}, {QStringLiteral("grc"), QStringLiteral("grc")},
        {QStringLiteral("tr"), QStringLiteral("tr")}, {QStringLiteral("tur"), QStringLiteral("tr")},
        {QStringLiteral("ar"), QStringLiteral("ar")}, {QStringLiteral("ara"), QStringLiteral("ar")},
        {QStringLiteral("he"), QStringLiteral("he")}, {QStringLiteral("heb"), QStringLiteral("he")},
        {QStringLiteral("fa"), QStringLiteral("fa")}, {QStringLiteral("fas"), QStringLiteral("fa")}, {QStringLiteral("per"), QStringLiteral("fa")},
        {QStringLiteral("hi"), QStringLiteral("hi")}, {QStringLiteral("hin"), QStringLiteral("hi")},
        {QStringLiteral("ur"), QStringLiteral("ur")}, {QStringLiteral("urd"), QStringLiteral("ur")},
        {QStringLiteral("bn"), QStringLiteral("bn")}, {QStringLiteral("ben"), QStringLiteral("bn")},
        {QStringLiteral("pa"), QStringLiteral("pa")}, {QStringLiteral("pan"), QStringLiteral("pa")},
        {QStringLiteral("ta"), QStringLiteral("ta")}, {QStringLiteral("tam"), QStringLiteral("ta")},
        {QStringLiteral("te"), QStringLiteral("te")}, {QStringLiteral("tel"), QStringLiteral("te")},
        {QStringLiteral("mr"), QStringLiteral("mr")}, {QStringLiteral("mar"), QStringLiteral("mr")},
        {QStringLiteral("gu"), QStringLiteral("gu")}, {QStringLiteral("guj"), QStringLiteral("gu")},
        {QStringLiteral("zh"), QStringLiteral("zh")}, {QStringLiteral("cmn"), QStringLiteral("cmn")},
        {QStringLiteral("yue"), QStringLiteral("yue")}, {QStringLiteral("wuu"), QStringLiteral("wuu")},
        {QStringLiteral("nan"), QStringLiteral("nan")}, {QStringLiteral("hak"), QStringLiteral("hak")},
        {QStringLiteral("gan"), QStringLiteral("gan")}, {QStringLiteral("hsn"), QStringLiteral("hsn")},
        {QStringLiteral("ja"), QStringLiteral("ja")}, {QStringLiteral("jpn"), QStringLiteral("ja")},
        {QStringLiteral("ko"), QStringLiteral("ko")}, {QStringLiteral("kor"), QStringLiteral("ko")},
        {QStringLiteral("vi"), QStringLiteral("vi")}, {QStringLiteral("vie"), QStringLiteral("vi")},
        {QStringLiteral("th"), QStringLiteral("th")}, {QStringLiteral("tha"), QStringLiteral("th")},
        {QStringLiteral("id"), QStringLiteral("id")}, {QStringLiteral("ind"), QStringLiteral("id")},
        {QStringLiteral("ms"), QStringLiteral("ms")}, {QStringLiteral("msa"), QStringLiteral("ms")},
        {QStringLiteral("tl"), QStringLiteral("tl")}, {QStringLiteral("tgl"), QStringLiteral("tl")},
        {QStringLiteral("fil"), QStringLiteral("fil")},
        {QStringLiteral("sw"), QStringLiteral("sw")}, {QStringLiteral("swa"), QStringLiteral("sw")},
        {QStringLiteral("af"), QStringLiteral("af")}, {QStringLiteral("afr"), QStringLiteral("af")},
        {QStringLiteral("zu"), QStringLiteral("zu")}, {QStringLiteral("zul"), QStringLiteral("zu")},
        {QStringLiteral("xh"), QStringLiteral("xh")}, {QStringLiteral("xho"), QStringLiteral("xh")},
        {QStringLiteral("yo"), QStringLiteral("yo")}, {QStringLiteral("yor"), QStringLiteral("yo")},
        {QStringLiteral("ha"), QStringLiteral("ha")}, {QStringLiteral("hau"), QStringLiteral("ha")},
        {QStringLiteral("am"), QStringLiteral("am")}, {QStringLiteral("amh"), QStringLiteral("am")},
        {QStringLiteral("ps"), QStringLiteral("ps")}, {QStringLiteral("pus"), QStringLiteral("ps")},
        {QStringLiteral("lah"), QStringLiteral("lah")}
    };
}

QString fallbackMediaWikiLanguageToken(QString text)
{
    text = text.trimmed().toLower();
    text.replace(QRegularExpression(QStringLiteral(R"([\s-]+)")), QStringLiteral("_"));
    text.remove(QRegularExpression(QStringLiteral(R"([^a-z0-9_])")));
    text.replace(QRegularExpression(QStringLiteral(R"(_+)")), QStringLiteral("_"));
    while (text.startsWith(QLatin1Char('_'))) {
        text.remove(0, 1);
    }
    while (text.endsWith(QLatin1Char('_'))) {
        text.chop(1);
    }
    if (text.isEmpty() || text.at(0).isDigit()) {
        return QString();
    }
    return text.left(48);
}

QString mediaWikiHeadingLanguageCode(const QString &heading)
{
    const LanguageNormalizer normalizer;
    static const QHash<QString, QString> knownHeadings = mediaWikiHeadingLanguageMap();
    const QString knownCode = knownHeadings.value(heading.trimmed());
    if (!knownCode.isEmpty()) {
        const QString normalizedCode = normalizer.normalize(knownCode);
        return normalizer.isSupported(normalizedCode) ? normalizedCode : QString();
    }

    const QString fallbackCode = normalizer.normalize(fallbackMediaWikiLanguageToken(heading));
    return normalizer.isSupported(fallbackCode) ? fallbackCode : QString();
}

QString mediaWikiTemplateLanguageCode(const QString &languageCode)
{
    const LanguageNormalizer normalizer;
    static const QHash<QString, QString> knownCodes = mediaWikiTemplateLanguageMap();
    const QString rawCode = languageCode.trimmed().toLower();
    const QString knownCode = knownCodes.value(rawCode);
    if (!knownCode.isEmpty()) {
        const QString normalizedCode = normalizer.normalize(knownCode);
        return normalizer.isSupported(normalizedCode) ? normalizedCode : QString();
    }

    static const QRegularExpression plausibleCodeExpression(
        QStringLiteral(R"(^[a-z][a-z0-9]{1,15}(?:-[a-z0-9]{1,12}){0,3}$)"));
    if (!plausibleCodeExpression.match(rawCode).hasMatch()) {
        return QString();
    }

    const QString normalizedCode = normalizer.normalize(rawCode);
    return normalizer.isSupported(normalizedCode) ? normalizedCode : QString();
}

}

DatasetScanner::DatasetScanner(const QString &datasetsPath)
    : m_datasetsPath(resolveDatasetsPath(datasetsPath))
{
}

QList<DatasetInfo> DatasetScanner::scan()
{
    m_lastError.clear();
    m_unsupportedLanguageItems = 0;
    m_unsupportedLanguageSamples.clear();

    QList<DatasetInfo> datasets;
    QDir datasetsDirectory(m_datasetsPath);
    if (!datasetsDirectory.exists()) {
        m_lastError = QStringLiteral("Diretorio de datasets nao encontrado: %1").arg(m_datasetsPath);
        return datasets;
    }

    QHash<QString, QList<CandidateFile>> candidatesByPair;
    QSet<QString> emittedSingleFileDatasets;

    QDirIterator iterator(m_datasetsPath,
                          QDir::Files | QDir::Readable,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QFileInfo fileInfo(iterator.next());

        const QString sourceDirectory = directoryName(fileInfo);
        if (sourceDirectory != QStringLiteral("mediawiki") && isGeneratedOrSupportFile(fileInfo)) {
            continue;
        }
        if (sourceDirectory == QStringLiteral("opus_preprocessed")
            && fileInfo.suffix().compare(QStringLiteral("tsv"), Qt::CaseInsensitive) == 0) {
            QString corpusName;
            QString sourceLanguage;
            QString targetLanguage;
            if (!parsePreprocessedOpusFileName(fileInfo, corpusName, sourceLanguage, targetLanguage)) {
                recordUnsupportedLanguageItem(QStringLiteral("arquivo OPUS pre-processado ignorado por par de idiomas invalido ou fora da lista suportada: %1")
                                                  .arg(fileInfo.fileName()));
                continue;
            }
            if (!hasReadableContent(fileInfo.absoluteFilePath())) {
                continue;
            }

            const QString datasetKey = QStringLiteral("opus-preprocessed\n%1\n%2\n%3")
                                           .arg(fileInfo.absoluteFilePath(), sourceLanguage, targetLanguage);
            if (emittedSingleFileDatasets.contains(datasetKey)) {
                continue;
            }

            emittedSingleFileDatasets.insert(datasetKey);
            datasets.append(DatasetInfo{
                DatasetInfo::SourceType::OpusPreprocessed,
                QStringLiteral("OPUS preprocessed %1").arg(corpusName),
                sourceLanguage,
                targetLanguage,
                fileInfo.absoluteFilePath(),
                QString()
            });
            continue;
        }

        if (sourceDirectory == QStringLiteral("freedict") && fileInfo.suffix().compare(QStringLiteral("tei"), Qt::CaseInsensitive) == 0) {
            const QString baseName = fileInfo.completeBaseName();
            QString sourceLanguage;
            QString targetLanguage;
            if (!parseLanguagePair(baseName, sourceLanguage, targetLanguage)) {
                recordUnsupportedLanguageItem(QStringLiteral("arquivo FreeDict ignorado por par de idiomas invalido ou fora da lista suportada: %1")
                                                  .arg(fileInfo.fileName()));
                continue;
            }
            if (!hasFreeDictTranslationEvidence(fileInfo.absoluteFilePath())) {
                continue;
            }

            const QString datasetKey = QStringLiteral("freedict\n%1\n%2\n%3")
                                           .arg(fileInfo.absoluteFilePath(), sourceLanguage, targetLanguage);
            if (emittedSingleFileDatasets.contains(datasetKey)) {
                continue;
            }

            emittedSingleFileDatasets.insert(datasetKey);
            datasets.append(DatasetInfo{
                DatasetInfo::SourceType::FreeDictTei,
                QStringLiteral("FreeDict %1").arg(baseName),
                sourceLanguage,
                targetLanguage,
                fileInfo.absoluteFilePath(),
                QString()
            });
            continue;
        }

        if (sourceDirectory == QStringLiteral("mediawiki_preprocessed")
            && fileInfo.suffix().compare(QStringLiteral("tsv"), Qt::CaseInsensitive) == 0) {
            QString corpusName;
            QString sourceLanguage;
            QString targetLanguage;
            if (!parsePreprocessedMediaWikiFileName(fileInfo, corpusName, sourceLanguage, targetLanguage)) {
                recordUnsupportedLanguageItem(QStringLiteral("arquivo MediaWiki pre-processado ignorado por par de idiomas invalido ou fora da lista suportada: %1")
                                                  .arg(fileInfo.fileName()));
                continue;
            }
            if (!hasReadableContent(fileInfo.absoluteFilePath())) {
                continue;
            }

            const QString datasetKey = QStringLiteral("mediawiki-preprocessed\n%1\n%2\n%3")
                                           .arg(fileInfo.absoluteFilePath(), sourceLanguage, targetLanguage);
            if (emittedSingleFileDatasets.contains(datasetKey)) {
                continue;
            }

            emittedSingleFileDatasets.insert(datasetKey);
            datasets.append(DatasetInfo{
                DatasetInfo::SourceType::MediaWikiPreprocessed,
                QStringLiteral("MediaWiki preprocessed %1").arg(corpusName),
                sourceLanguage,
                targetLanguage,
                fileInfo.absoluteFilePath(),
                QString()
            });
            continue;
        }

        if (sourceDirectory == QStringLiteral("mediawiki") && fileInfo.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0) {
            const QString datasetKey = QStringLiteral("mediawiki-xml\n%1").arg(fileInfo.absoluteFilePath());
            if (emittedSingleFileDatasets.contains(datasetKey)) {
                continue;
            }

            emittedSingleFileDatasets.insert(datasetKey);
            datasets.append(DatasetInfo{
                DatasetInfo::SourceType::MediaWikiXml,
                QStringLiteral("MediaWiki %1").arg(fileInfo.completeBaseName()),
                QString(),
                QString(),
                fileInfo.absoluteFilePath(),
                QString()
            });
            continue;
        }

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

        QString sourceLanguage;
        QString targetLanguage;
        if (!parseLanguagePair(keyParts.at(1), sourceLanguage, targetLanguage)) {
            continue;
        }
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
            DatasetInfo::SourceType::ParallelText,
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

    const QString unsupportedSummary = unsupportedLanguageSummary();
    if (!unsupportedSummary.isEmpty()) {
        if (!m_lastError.isEmpty()) {
            m_lastError.append(QLatin1Char('\n'));
        }
        m_lastError.append(unsupportedSummary);
    }

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

    return QDir::cleanPath(AppPaths::datasetsPath());
}

void DatasetScanner::recordUnsupportedLanguageItem(const QString &description) const
{
    ++m_unsupportedLanguageItems;
    if (m_unsupportedLanguageSamples.size() < 8 && !m_unsupportedLanguageSamples.contains(description)) {
        m_unsupportedLanguageSamples.append(description);
    }
}

QString DatasetScanner::unsupportedLanguageSummary() const
{
    if (m_unsupportedLanguageItems == 0) {
        return QString();
    }

    QStringList lines;
    lines.append(QStringLiteral("Idiomas fora da lista suportada ignorados na verificacao de datasets: %1 item(ns).")
                     .arg(m_unsupportedLanguageItems));
    for (const QString &sample : m_unsupportedLanguageSamples) {
        lines.append(QStringLiteral("- %1").arg(sample));
    }
    if (m_unsupportedLanguageItems > m_unsupportedLanguageSamples.size()) {
        lines.append(QStringLiteral("- ... mais %1 item(ns) ignorado(s)")
                         .arg(m_unsupportedLanguageItems - m_unsupportedLanguageSamples.size()));
    }
    return lines.join(QLatin1Char('\n'));
}

bool DatasetScanner::parseCandidate(const QFileInfo &fileInfo, CandidateFile &candidate) const
{
    if (!fileInfo.isFile() || !fileInfo.isReadable()) {
        return false;
    }

    if (fileInfo.size() < MinimumDatasetFileSize) {
        return false;
    }

    const QString language = normalizedLanguageToken(fileInfo.suffix());
    if (!isKnownLanguageToken(language)) {
        recordUnsupportedLanguageItem(QStringLiteral("arquivo ignorado por extensao/idioma fora da lista suportada: %1")
                                          .arg(fileInfo.fileName()));
        return false;
    }

    const QString baseName = fileInfo.completeBaseName();
    const int langPairSeparator = baseName.lastIndexOf(QLatin1Char('.'));
    if (langPairSeparator <= 0 || langPairSeparator >= baseName.size() - 1) {
        return false;
    }

    const QString corpusName = baseName.left(langPairSeparator);
    const QString langPair = baseName.mid(langPairSeparator + 1);

    QString sourceLanguage;
    QString targetLanguage;
    if (!parseLanguagePair(langPair, sourceLanguage, targetLanguage)) {
        recordUnsupportedLanguageItem(QStringLiteral("arquivo ignorado por par de idiomas fora da lista suportada no nome: %1")
                                          .arg(fileInfo.fileName()));
        return false;
    }
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

bool DatasetScanner::hasFreeDictTranslationEvidence(const QString &filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QXmlStreamReader xml(&file);
    bool insideEntry = false;
    bool insideTranslationCitation = false;
    bool hasHeadword = false;

    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.isStartElement()) {
            const QStringView name = xml.name();
            if (name == QStringLiteral("entry")) {
                insideEntry = true;
                insideTranslationCitation = false;
                hasHeadword = false;
            } else if (insideEntry && name == QStringLiteral("orth")) {
                hasHeadword = !xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed().isEmpty();
            } else if (insideEntry && name == QStringLiteral("cit")) {
                insideTranslationCitation = xml.attributes().value(QStringLiteral("type")) == QStringLiteral("trans");
            } else if (insideEntry && insideTranslationCitation && name == QStringLiteral("quote")) {
                if (hasHeadword && !xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed().isEmpty()) {
                    return true;
                }
            }
        } else if (xml.isEndElement()) {
            if (xml.name() == QStringLiteral("entry")) {
                insideEntry = false;
                insideTranslationCitation = false;
                hasHeadword = false;
            } else if (xml.name() == QStringLiteral("cit")) {
                insideTranslationCitation = false;
            }
        }
    }

    return false;
}

bool DatasetScanner::inspectMediaWikiEvidence(const QString &filePath, MediaWikiEvidence &evidence) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    static const QRegularExpression headingExpression(QStringLiteral(R"(^==([^=]+)==\s*$)"));
    static const QRegularExpression translationsHeadingExpression(QStringLiteral(R"(^={4,5}\s*Translations\s*={4,5}\s*$)"),
                                                                  QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression nextSectionExpression(QStringLiteral(R"(^={2,}[^=].*={2,}\s*$)"));
    static const QRegularExpression translationTemplateExpression(QStringLiteral(R"(\{\{\s*(?:t(?:\+check|\+|-check|-simple)?)\s*\|\s*([^|}]+)\s*\|\s*([^|}]+))"),
                                                                  QRegularExpression::CaseInsensitiveOption);

    QXmlStreamReader xml(&file);
    QString siteName;
    QString dbName;
    QString title;
    QString ns;
    QString text;
    bool inPage = false;
    int inspectedMainPages = 0;

    while (!xml.atEnd() && inspectedMainPages < MaximumMediaWikiPagesToInspect) {
        xml.readNext();

        if (xml.isStartElement()) {
            const QStringView name = xml.name();
            if (name == QStringLiteral("sitename")) {
                siteName = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
            } else if (name == QStringLiteral("dbname")) {
                dbName = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
            } else if (name == QStringLiteral("page")) {
                inPage = true;
                title.clear();
                ns.clear();
                text.clear();
            } else if (inPage && name == QStringLiteral("title")) {
                title = xml.readElementText(QXmlStreamReader::SkipChildElements);
            } else if (inPage && name == QStringLiteral("ns")) {
                ns = xml.readElementText(QXmlStreamReader::SkipChildElements);
            } else if (inPage && name == QStringLiteral("text")) {
                text = xml.readElementText(QXmlStreamReader::IncludeChildElements);
            }
        } else if (xml.isEndElement() && xml.name() == QStringLiteral("page")) {
            inPage = false;
            if (ns != QStringLiteral("0") || title.contains(QLatin1Char(':')) || text.isEmpty()) {
                continue;
            }

            ++inspectedMainPages;
            QString currentSourceLang;
            bool inTranslationsSection = false;
            const QStringList lines = text.split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                const QString trimmedLine = line.trimmed();
                const QRegularExpressionMatch headingMatch = headingExpression.match(trimmedLine);
                if (headingMatch.hasMatch()) {
                    currentSourceLang = mediaWikiHeadingLanguageCode(headingMatch.captured(1).trimmed());
                    inTranslationsSection = false;
                    continue;
                }

                if (currentSourceLang.isEmpty()) {
                    continue;
                }

                if (translationsHeadingExpression.match(trimmedLine).hasMatch()) {
                    inTranslationsSection = true;
                    continue;
                }

                if (inTranslationsSection && nextSectionExpression.match(trimmedLine).hasMatch()) {
                    inTranslationsSection = false;
                    continue;
                }

                if (!inTranslationsSection || !trimmedLine.startsWith(QLatin1Char('*'))) {
                    continue;
                }

                QRegularExpressionMatchIterator matches = translationTemplateExpression.globalMatch(trimmedLine);
                while (matches.hasNext()) {
                    const QRegularExpressionMatch match = matches.next();
                    const QString targetLanguage = mediaWikiTemplateLanguageCode(match.captured(1));
                    if (targetLanguage.isEmpty() || targetLanguage == currentSourceLang) {
                        continue;
                    }
                    evidence.sourceLanguages.insert(currentSourceLang);
                    evidence.targetLanguagesBySourceKey.insert(mediaWikiPairKey(currentSourceLang, targetLanguage));
                }
            }

            if (evidence.targetLanguagesBySourceKey.size() >= CompleteKnownMediaWikiPairCount) {
                break;
            }
        }
    }

    const QString corpusToken = !dbName.isEmpty()
                                    ? dbName
                                    : (!siteName.isEmpty() ? siteName : QFileInfo(filePath).completeBaseName());
    evidence.corpusName = QStringLiteral("MediaWiki %1").arg(corpusToken);
    return !evidence.targetLanguagesBySourceKey.isEmpty();
}

bool DatasetScanner::parseLanguagePair(const QString &langPair, QString &sourceLanguage, QString &targetLanguage) const
{
    for (qsizetype index = 1; index < langPair.size() - 1; ++index) {
        if (langPair.at(index) != QLatin1Char('-')) {
            continue;
        }

        const QString sourceCandidate = normalizedLanguageToken(langPair.left(index));
        const QString targetCandidate = normalizedLanguageToken(langPair.mid(index + 1));
        if (isKnownLanguageToken(sourceCandidate) && isKnownLanguageToken(targetCandidate)) {
            sourceLanguage = sourceCandidate;
            targetLanguage = targetCandidate;
            return true;
        }
    }

    return false;
}

bool DatasetScanner::isKnownLanguageToken(const QString &language) const
{
    const LanguageNormalizer languageNormalizer;
    return languageNormalizer.isSupported(language);
}

QString DatasetScanner::normalizedLanguageToken(const QString &language) const
{
    const LanguageNormalizer languageNormalizer;
    return languageNormalizer.normalize(language);
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

    return true;
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
