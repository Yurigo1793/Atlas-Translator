#include "TranslatorEngine.h"

#include "Utf8Streams.h"

#include <QChar>
#include <QElapsedTimer>
#include <QList>
#include <QRegularExpression>
#include <QTextStream>

namespace {
struct SentenceSegment {
    QString text;
    QString delimiter;
};

bool isCommonAbbreviation(const QString &text)
{
    static const QStringList abbreviations = {
        QStringLiteral("mr"), QStringLiteral("mrs"), QStringLiteral("ms"), QStringLiteral("dr"),
        QStringLiteral("prof"), QStringLiteral("sr"), QStringLiteral("jr"), QStringLiteral("etc"),
        QStringLiteral("e.g"), QStringLiteral("i.e"), QStringLiteral("vs")
    };

    const QStringList tokens = text.trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (tokens.isEmpty()) {
        return false;
    }

    return abbreviations.contains(tokens.last().toLower());
}

bool isSentenceBoundary(const QString &text, qsizetype index)
{
    const QChar character = text.at(index);
    if (character != QLatin1Char('.') && character != QLatin1Char('!') && character != QLatin1Char('?')) {
        return false;
    }

    if (character == QLatin1Char('.') && index > 0 && (index + 1) < text.size()
        && text.at(index - 1).isDigit() && text.at(index + 1).isDigit()) {
        return false;
    }

    if (character == QLatin1Char('.') && isCommonAbbreviation(text.left(index))) {
        return false;
    }

    return true;
}

QList<SentenceSegment> splitSentenceSegments(const QString &text)
{
    QList<SentenceSegment> segments;
    QString current;

    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        if (isSentenceBoundary(text, index)) {
            QString delimiter(character);
            while ((index + 1) < text.size()) {
                const QChar next = text.at(index + 1);
                if (next == QLatin1Char('.') || next == QLatin1Char('!') || next == QLatin1Char('?')
                    || next == QLatin1Char('"') || next == QLatin1Char('\'') || next == QLatin1Char(')')
                    || next == QLatin1Char(']') || next == QLatin1Char('}')) {
                    delimiter.append(next);
                    ++index;
                    continue;
                }
                break;
            }
            segments.append(SentenceSegment{current.trimmed(), delimiter});
            current.clear();
            continue;
        }

        current.append(character);
    }

    if (!current.trimmed().isEmpty() || segments.isEmpty()) {
        segments.append(SentenceSegment{current.trimmed(), QString()});
    }

    return segments;
}

bool hasLetters(const QString &text)
{
    for (const QChar character : text) {
        if (character.isLetter()) {
            return true;
        }
    }
    return false;
}

bool isAllCapsText(const QString &text)
{
    bool foundLetter = false;
    for (const QChar character : text) {
        if (!character.isLetter()) {
            continue;
        }
        foundLetter = true;
        if (character.isLower()) {
            return false;
        }
    }
    return foundLetter;
}

bool startsWithUppercaseLetter(const QString &text)
{
    for (const QChar character : text) {
        if (character.isLetter()) {
            return character.isUpper();
        }
    }
    return false;
}

QString uppercaseFirstLetter(QString text)
{
    for (qsizetype index = 0; index < text.size(); ++index) {
        if (text.at(index).isLetter()) {
            text[index] = text.at(index).toUpper();
            break;
        }
    }
    return text;
}

QString applyCapitalization(const QString &source, const QString &translation)
{
    if (!hasLetters(source) || translation.isEmpty()) {
        return translation;
    }

    if (isAllCapsText(source)) {
        return translation.toUpper();
    }

    if (startsWithUppercaseLetter(source)) {
        return uppercaseFirstLetter(translation);
    }

    return translation;
}

QStringList inlinePunctuationMarks(const QString &source)
{
    QStringList marks;
    static const QRegularExpression tokenExpression(QStringLiteral(R"((\p{L}|\p{N})+[\p{L}\p{N}'’_-]*|[^\s])"));
    QRegularExpressionMatchIterator iterator = tokenExpression.globalMatch(source);
    qsizetype wordIndex = -1;
    while (iterator.hasNext()) {
        const QString token = iterator.next().captured(0);
        const bool isWord = token.at(0).isLetterOrNumber();
        if (isWord) {
            ++wordIndex;
            while (marks.size() <= wordIndex) {
                marks.append(QString());
            }
            continue;
        }

        if (wordIndex >= 0
            && (token == QStringLiteral(",") || token == QStringLiteral(";") || token == QStringLiteral(":"))) {
            marks[wordIndex].append(token);
        }
    }
    return marks;
}


QString leadingWrappers(const QString &text)
{
    QString wrappers;
    for (const QChar character : text) {
        if (character.isSpace()) {
            continue;
        }
        if (character == QLatin1Char('"') || character == QLatin1Char('(') || character == QLatin1Char('[')
            || character == QLatin1Char('{') || character == QLatin1Char('\'')) {
            wrappers.append(character);
            continue;
        }
        break;
    }
    return wrappers;
}

QString trailingWrappers(const QString &text)
{
    QString wrappers;
    for (qsizetype index = text.size() - 1; index >= 0; --index) {
        const QChar character = text.at(index);
        if (character.isSpace()) {
            continue;
        }
        if (character == QLatin1Char('"') || character == QLatin1Char(')') || character == QLatin1Char(']')
            || character == QLatin1Char('}') || character == QLatin1Char('\'')) {
            wrappers.prepend(character);
            continue;
        }
        break;
    }
    return wrappers;
}

QString reapplyInlinePunctuation(const QString &source, const QString &translation)
{
    const QStringList marks = inlinePunctuationMarks(source);
    if (marks.isEmpty()) {
        return translation;
    }

    QStringList words = translation.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (words.isEmpty()) {
        return translation;
    }

    const qsizetype limit = qMin(words.size(), marks.size());
    for (qsizetype index = 0; index < limit; ++index) {
        if (marks.at(index).isEmpty()) {
            continue;
        }
        if (!words[index].endsWith(marks.at(index))) {
            words[index].append(marks.at(index));
        }
    }

    return words.join(QStringLiteral(" "));
}
}

TranslatorEngine::TranslatorEngine(const QString &databasePath)
    : m_databaseManager(databasePath),
      m_translationCache(4096)
{
}

bool TranslatorEngine::initialize()
{
    const bool initialized = m_databaseManager.initialize();
    m_ready = initialized;
    return initialized;
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

    if (!m_ready) {
        result.translation = text;
        result.translationTimeNs = timer.nsecsElapsed();
        return result;
    }

    const QString normalizedSourceLang = m_languageNormalizer.normalize(sourceLang);
    const QString normalizedTargetLang = m_languageNormalizer.normalize(targetLang);
    QString translatedText;
    QString currentLine;

    auto translateLine = [&](const QString &line) {
        if (line.trimmed().isEmpty()) {
            return line;
        }

        qsizetype firstContent = 0;
        while (firstContent < line.size() && line.at(firstContent).isSpace()) {
            ++firstContent;
        }
        qsizetype lastContent = line.size() - 1;
        while (lastContent >= firstContent && line.at(lastContent).isSpace()) {
            --lastContent;
        }

        const QString leadingWhitespace = line.left(firstContent);
        const QString trailingWhitespace = line.mid(lastContent + 1);
        const QString content = line.mid(firstContent, lastContent - firstContent + 1);

        const QList<SentenceSegment> segments = splitSentenceSegments(content);
        QStringList translatedSegments;
        translatedSegments.reserve(segments.size());

        for (const SentenceSegment &segment : segments) {
            TranslationResult segmentResult = translateSegment(segment.text, normalizedSourceLang, normalizedTargetLang);
            QString translatedSegment = segmentResult.translation;
            if (translatedSegment.isEmpty()) {
                translatedSegment = segment.text;
            }
            if (segmentResult.matchType != QStringLiteral("full")) {
                translatedSegment = reapplyInlinePunctuation(segment.text, translatedSegment);
            }
            translatedSegment = applyCapitalization(segment.text, translatedSegment);
            const QString leading = leadingWrappers(segment.text);
            const QString trailing = trailingWrappers(segment.text);
            if (!leading.isEmpty() && !translatedSegment.startsWith(leading)) {
                translatedSegment.prepend(leading);
            }
            if (!trailing.isEmpty() && !translatedSegment.endsWith(trailing)) {
                translatedSegment.append(trailing);
            }
            translatedSegment.append(segment.delimiter);
            translatedSegments.append(translatedSegment.trimmed());

            if (result.matchType == QStringLiteral("none") && segmentResult.matchType != QStringLiteral("none")) {
                result.matchedText = segmentResult.matchedText;
                result.matchType = segmentResult.matchType;
                result.consumedWords = segmentResult.consumedWords;
            }
        }

        return leadingWhitespace + translatedSegments.join(QStringLiteral(" ")).trimmed() + trailingWhitespace;
    };

    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        if (character == QLatin1Char('\n')) {
            translatedText.append(translateLine(currentLine));
            translatedText.append(character);
            currentLine.clear();
            continue;
        }
        currentLine.append(character);
    }
    translatedText.append(translateLine(currentLine));

    result.translation = translatedText;
    result.translationTimeNs = timer.nsecsElapsed();
    recordTranslationTime(result.translationTimeNs);
    return result;
}

TranslatorEngine::TranslationResult TranslatorEngine::translateSegment(const QString &text,
                                                                       const QString &sourceLang,
                                                                       const QString &targetLang) const
{
    TranslationResult result;
    result.input = text;

    const QStringList words = m_normalizer.words(text);
    if (words.isEmpty()) {
        return result;
    }

    const PhraseMatch completeMatch = findBestMatch(words, 0, sourceLang, targetLang);
    if (completeMatch.found && completeMatch.consumedWords == words.size()) {
        result.translation = completeMatch.translation;
        result.matchedText = completeMatch.matchedText;
        result.matchType = QStringLiteral("full");
        result.consumedWords = completeMatch.consumedWords;
        logMatch(text, completeMatch, sourceLang, targetLang);
        return result;
    }

    QStringList translatedParts;
    translatedParts.reserve(words.size());

    for (qsizetype position = 0; position < words.size();) {
        const PhraseMatch match = findBestMatch(words, position, sourceLang, targetLang);
        if (match.found) {
            translatedParts.append(match.translation);
            if (result.matchType == QStringLiteral("none")) {
                result.matchedText = match.matchedText;
                result.matchType = match.matchType;
                result.consumedWords = match.consumedWords;
            }
            logMatch(text, match, sourceLang, targetLang);
            position += match.consumedWords;
            continue;
        }

        translatedParts.append(words.at(position));
        ++position;
    }

    result.translation = translatedParts.join(QStringLiteral(" "));
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

    constexpr qsizetype MaximumPhraseWords = 12;
    const qsizetype maxLength = qMin(MaximumPhraseWords, words.size() - position);
    for (qsizetype length = maxLength; length > 0; --length) {
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
    output << "Consultas SQL: " << sqlStats.queryCount << Qt::endl;
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

DatabaseManager::DatabaseSummary TranslatorEngine::databaseSummary() const
{
    return m_databaseManager.databaseSummary();
}


QStringList TranslatorEngine::availableLanguages() const
{
    return m_databaseManager.availableLanguages();
}

QList<DatabaseManager::LanguagePair> TranslatorEngine::availableLanguagePairs() const
{
    return m_databaseManager.availableLanguagePairs();
}

bool TranslatorEngine::hasLanguagePair(const QString &sourceLang, const QString &targetLang) const
{
    return m_databaseManager.hasLanguagePair(sourceLang, targetLang);
}

void TranslatorEngine::printDatabaseSummary(QTextStream &output) const
{
    m_databaseManager.printDatabaseSummary(output);
}

QString TranslatorEngine::cacheKey(const QString &sourceText,
                                   const QString &sourceLang,
                                   const QString &targetLang) const
{
    return QStringLiteral("%1|%2|%3").arg(sourceLang, targetLang, sourceText);
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
    output << "[MATCH] " << sourceLang << " -> " << targetLang
           << " | input=\"" << input << "\""
           << " | matched=\"" << match.matchedText << "\""
           << " | translation=\"" << match.translation << "\""
           << " | type=" << match.matchType
           << " | words=" << match.consumedWords
           << Qt::endl;
}

QString TranslatorEngine::matchType(qsizetype position, qsizetype consumedWords, qsizetype totalWords, bool exactCandidate) const
{
    if (position == 0 && consumedWords == totalWords && exactCandidate) {
        return QStringLiteral("full");
    }

    if (consumedWords > 1) {
        return exactCandidate ? QStringLiteral("phrase") : QStringLiteral("phrase-normalized");
    }

    return exactCandidate ? QStringLiteral("word") : QStringLiteral("word-normalized");
}
