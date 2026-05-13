#include "TranslatorEngine.h"

#include "Utf8Streams.h"

#include <QElapsedTimer>
#include <QList>
#include <QTextStream>

TranslatorEngine::TranslatorEngine(const QString &databasePath)
    : m_databaseManager(databasePath),
      m_translationCache(4096)
{
}

bool TranslatorEngine::initialize()
{
    return m_databaseManager.initialize();
}

QString TranslatorEngine::translate(const QString &text,
                                    const QString &sourceLang,
                                    const QString &targetLang) const
{
    return translateDetailed(text, sourceLang, targetLang).translation;
}

TranslatorEngine::TranslationResult TranslatorEngine::translateDetailed(const QString &text,
                                                                        const QString &sourceLang,
                                                                        const QString &targetLang) const
{
    QElapsedTimer timer;
    timer.start();

    TranslationResult result;
    result.input = text;

    const QString normalizedSourceLang = m_languageNormalizer.normalize(sourceLang);
    const QString normalizedTargetLang = m_languageNormalizer.normalize(targetLang);
    const QStringList words = m_normalizer.words(text);
    if (words.isEmpty()) {
        result.translationTimeNs = timer.nsecsElapsed();
        recordTranslationTime(result.translationTimeNs);
        return result;
    }

    const PhraseMatch completeMatch = findBestMatch(words, 0, normalizedSourceLang, normalizedTargetLang);
    if (completeMatch.found && completeMatch.consumedWords == words.size()) {
        result.translation = completeMatch.translation;
        result.matchedText = completeMatch.matchedText;
        result.matchType = QStringLiteral("full");
        result.consumedWords = completeMatch.consumedWords;
        logMatch(text, completeMatch, normalizedSourceLang, normalizedTargetLang);
        result.translationTimeNs = timer.nsecsElapsed();
        recordTranslationTime(result.translationTimeNs);
        return result;
    }

    QStringList translatedParts;
    translatedParts.reserve(words.size());

    for (qsizetype position = 0; position < words.size();) {
        const PhraseMatch match = findBestMatch(words, position, normalizedSourceLang, normalizedTargetLang);
        if (match.found) {
            translatedParts.append(match.translation);
            if (result.matchType == QStringLiteral("none")) {
                result.matchedText = match.matchedText;
                result.matchType = match.matchType;
                result.consumedWords = match.consumedWords;
            }
            logMatch(text, match, normalizedSourceLang, normalizedTargetLang);
            position += match.consumedWords;
            continue;
        }

        translatedParts.append(words.at(position));
        ++position;
    }

    result.translation = translatedParts.join(QStringLiteral(" "));
    result.translationTimeNs = timer.nsecsElapsed();
    recordTranslationTime(result.translationTimeNs);
    return result;
}

TranslatorEngine::PhraseMatch TranslatorEngine::findBestMatch(const QStringList &words,
                                                              qsizetype position,
                                                              const QString &sourceLang,
                                                              const QString &targetLang) const
{
    struct CandidateGroup {
        qsizetype length = 0;
        QString exactCandidate;
        QStringList candidates;
    };

    QList<CandidateGroup> groups;
    QStringList missingCandidates;
    QHash<QString, QString> availableTranslations;

    for (qsizetype length = words.size() - position; length > 0; --length) {
        const QStringList slice = words.mid(position, length);
        const QString exactCandidate = m_normalizer.normalizeForLookup(slice.join(QStringLiteral(" ")));
        const QStringList candidates = m_normalizer.lookupCandidates(slice);
        if (candidates.isEmpty()) {
            continue;
        }

        groups.append(CandidateGroup{length, exactCandidate, candidates});

        for (const QString &candidate : candidates) {
            QString cachedTranslation;
            const QString key = cacheKey(candidate, sourceLang, targetLang);
            if (m_translationCache.get(key, cachedTranslation)) {
                ++m_statistics.cacheHits;
                availableTranslations.insert(candidate, cachedTranslation);
                if (m_debugEnabled) {
                    QTextStream output(stdout);
                    configureUtf8Stream(output);
                    output << "[CACHE HIT] " << candidate << Qt::endl;
                }
                continue;
            }

            ++m_statistics.cacheMisses;
            if (!missingCandidates.contains(candidate)) {
                missingCandidates.append(candidate);
            }
            if (m_debugEnabled) {
                QTextStream output(stdout);
                configureUtf8Stream(output);
                output << "[CACHE MISS] " << candidate << Qt::endl;
            }
        }
    }

    const DatabaseManager::SqlStatistics sqlBefore = m_databaseManager.sqlStatistics();
    const QHash<QString, QString> databaseMatches = m_databaseManager.findTranslations(missingCandidates,
                                                                                        sourceLang,
                                                                                        targetLang);
    const DatabaseManager::SqlStatistics sqlAfter = m_databaseManager.sqlStatistics();
    if (m_debugEnabled && sqlAfter.queryCount > sqlBefore.queryCount) {
        QTextStream output(stdout);
        configureUtf8Stream(output);
        output << "[SQL QUERY TIME] "
               << QString::number(static_cast<double>(sqlAfter.totalQueryTimeNs - sqlBefore.totalQueryTimeNs) / 1000000.0, 'f', 3)
               << " ms" << Qt::endl;
    }

    for (auto iterator = databaseMatches.constBegin(); iterator != databaseMatches.constEnd(); ++iterator) {
        availableTranslations.insert(iterator.key(), iterator.value());
        cacheTranslation(cacheKey(iterator.key(), sourceLang, targetLang), iterator.value());
    }

    for (const CandidateGroup &group : groups) {
        for (const QString &candidate : group.candidates) {
            if (availableTranslations.contains(candidate)) {
                PhraseMatch match;
                match.found = true;
                match.matchedText = candidate;
                match.translation = availableTranslations.value(candidate);
                match.consumedWords = group.length;
                match.matchType = matchType(position, group.length, words.size(), candidate == group.exactCandidate);
                return match;
            }
        }
    }

    return PhraseMatch();
}

QString TranslatorEngine::lastError() const
{
    return m_databaseManager.lastError();
}

TranslatorEngine::Statistics TranslatorEngine::statistics() const
{
    return m_statistics;
}

DatabaseManager::SqlStatistics TranslatorEngine::sqlStatistics() const
{
    return m_databaseManager.sqlStatistics();
}

double TranslatorEngine::averageTranslationTimeMs() const
{
    if (m_statistics.totalTranslations == 0) {
        return 0.0;
    }

    return static_cast<double>(m_statistics.totalTranslationTimeNs)
        / static_cast<double>(m_statistics.totalTranslations)
        / 1000000.0;
}

void TranslatorEngine::printStatistics(QTextStream &output) const
{
    configureUtf8Stream(output);
    const DatabaseManager::SqlStatistics sqlStats = m_databaseManager.sqlStatistics();

    output << Qt::endl;
    output << "Estatísticas do Atlas-Translator:" << Qt::endl;
    output << "Total de traduções: " << m_statistics.totalTranslations << Qt::endl;
    output << "Cache hits: " << m_statistics.cacheHits << Qt::endl;
    output << "Cache misses: " << m_statistics.cacheMisses << Qt::endl;
    output << "Tempo médio de tradução: " << QString::number(averageTranslationTimeMs(), 'f', 3) << " ms" << Qt::endl;
    output << "SQL queries: " << sqlStats.queryCount << Qt::endl;
    output << "Tempo médio SQL: " << QString::number(m_databaseManager.averageSqlQueryTimeMs(), 'f', 3) << " ms" << Qt::endl;
}

void TranslatorEngine::setDebugEnabled(bool enabled)
{
    m_debugEnabled = enabled;
}

bool TranslatorEngine::debugEnabled() const
{
    return m_debugEnabled;
}

QString TranslatorEngine::cacheKey(const QString &sourceText,
                                   const QString &sourceLang,
                                   const QString &targetLang) const
{
    return QStringLiteral("%1|%2|%3").arg(m_languageNormalizer.normalize(sourceLang),
                                           m_languageNormalizer.normalize(targetLang),
                                           m_normalizer.normalizeForLookup(sourceText));
}

void TranslatorEngine::cacheTranslation(const QString &key, const QString &translation) const
{
    m_translationCache.put(key, translation);
}

void TranslatorEngine::recordTranslationTime(qint64 elapsedNs) const
{
    ++m_statistics.totalTranslations;
    m_statistics.totalTranslationTimeNs += elapsedNs;
}

void TranslatorEngine::logMatch(const QString &input,
                                const PhraseMatch &match,
                                const QString &sourceLang,
                                const QString &targetLang) const
{
    if (!m_debugEnabled) {
        return;
    }

    QTextStream output(stdout);
    configureUtf8Stream(output);
    output << "[MATCH]" << Qt::endl;
    output << "MATCH FOUND" << Qt::endl;
    output << "Input: " << input << Qt::endl;
    output << "Languages: " << sourceLang << " -> " << targetLang << Qt::endl;
    output << "Matched: " << match.matchedText << Qt::endl;
    output << "MATCH TYPE: " << match.matchType << Qt::endl;
    output << "WORDS CONSUMED: " << match.consumedWords << Qt::endl;
}

QString TranslatorEngine::matchType(qsizetype position,
                                    qsizetype consumedWords,
                                    qsizetype totalWords,
                                    bool exactCandidate) const
{
    if (position == 0 && consumedWords == totalWords && exactCandidate) {
        return QStringLiteral("full");
    }

    if (!exactCandidate) {
        return QStringLiteral("flexible");
    }

    if (consumedWords == 1) {
        return QStringLiteral("word");
    }

    return QStringLiteral("phrase");
}
