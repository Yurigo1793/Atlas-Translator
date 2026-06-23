#include "TranslatorEngine.h"

#include "AtlasReport.h"
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

enum class SurfaceTokenType {
    Word,
    Space,
    Technical,
    Shortcut,
    Number,
    Punctuation,
    Symbol
};

struct SurfaceToken {
    SurfaceTokenType type = SurfaceTokenType::Symbol;
    QString text;
    qsizetype wordIndex = -1;
    qsizetype start = 0;
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
            while ((index + 1) < text.size() && text.at(index + 1).isSpace()) {
                delimiter.append(text.at(index + 1));
                ++index;
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

bool isIntraWordMark(const QString &text, qsizetype index)
{
    const QChar character = text.at(index);
    if (character != QLatin1Char('\'') && character != QLatin1Char('-') && character != QChar(0x2019)) {
        return false;
    }

    return index > 0
        && (index + 1) < text.size()
        && text.at(index - 1).isLetterOrNumber()
        && text.at(index + 1).isLetterOrNumber();
}

bool isCombiningMark(QChar character)
{
    const QChar::Category category = character.category();
    return category == QChar::Mark_NonSpacing
        || category == QChar::Mark_SpacingCombining
        || category == QChar::Mark_Enclosing;
}

bool isLetterCategory(QChar::Category category)
{
    return category == QChar::Letter_Uppercase
        || category == QChar::Letter_Lowercase
        || category == QChar::Letter_Titlecase
        || category == QChar::Letter_Modifier
        || category == QChar::Letter_Other;
}

bool isLetterAt(const QString &text, qsizetype index, qsizetype *advance = nullptr)
{
    if (advance != nullptr) {
        *advance = 1;
    }

    const QChar character = text.at(index);
    if (character.isHighSurrogate()
        && (index + 1) < text.size()
        && text.at(index + 1).isLowSurrogate()) {
        if (advance != nullptr) {
            *advance = 2;
        }
        const char32_t codepoint = QChar::surrogateToUcs4(character, text.at(index + 1));
        return isLetterCategory(QChar::category(codepoint));
    }

    return character.isLetter();
}

bool isWordContinuationAt(const QString &text, qsizetype index, qsizetype *advance = nullptr)
{
    if (advance != nullptr) {
        *advance = 1;
    }

    const QChar character = text.at(index);
    if (character.isHighSurrogate()
        && (index + 1) < text.size()
        && text.at(index + 1).isLowSurrogate()) {
        if (advance != nullptr) {
            *advance = 2;
        }
        const char32_t codepoint = QChar::surrogateToUcs4(character, text.at(index + 1));
        const QChar::Category category = QChar::category(codepoint);
        return isLetterCategory(category)
            || category == QChar::Number_DecimalDigit
            || category == QChar::Number_Letter
            || category == QChar::Number_Other
            || category == QChar::Mark_NonSpacing
            || category == QChar::Mark_SpacingCombining
            || category == QChar::Mark_Enclosing;
    }

    return character.isLetterOrNumber() || isCombiningMark(character);
}

bool isTechnicalMark(const QString &text, qsizetype index)
{
    const QChar character = text.at(index);
    if (character != QLatin1Char('+') && character != QLatin1Char('#') && character != QLatin1Char('.')) {
        return false;
    }

    const bool hasLeft = index > 0 && text.at(index - 1).isLetterOrNumber();
    const bool hasRight = (index + 1) < text.size() && text.at(index + 1).isLetterOrNumber();
    if (character == QLatin1Char('.') && hasLeft && hasRight) {
        return true;
    }
    if ((character == QLatin1Char('+') || character == QLatin1Char('#')) && hasLeft) {
        return true;
    }

    return false;
}

QString cleanStoredTranslationForSource(const QString &source, const QString &translation)
{
    QString cleaned;
    cleaned.reserve(translation.size());

    for (qsizetype index = 0; index < translation.size(); ++index) {
        const QChar character = translation.at(index);
        if (character.isLetterOrNumber()
            || character.isSpace()
            || isCombiningMark(character)
            || isIntraWordMark(translation, index)
            || isTechnicalMark(translation, index)) {
            cleaned.append(character);
        } else {
            cleaned.append(QLatin1Char(' '));
        }
    }

    cleaned.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    cleaned = cleaned.trimmed();
    return cleaned.isEmpty() && !translation.trimmed().isEmpty() ? source : cleaned;
}

bool isShortcutAt(const QString &text, qsizetype index, qsizetype &length)
{
    static const QRegularExpression shortcutExpression(
        QStringLiteral(R"((?:Ctrl|Shift|Alt|Meta|Cmd)\+[A-Za-z0-9]+(?:\+[A-Za-z0-9]+)*)"),
        QRegularExpression::CaseInsensitiveOption);

    const QRegularExpressionMatch match = shortcutExpression.match(text,
                                                                   index,
                                                                   QRegularExpression::NormalMatch,
                                                                   QRegularExpression::AnchorAtOffsetMatchOption);
    if (!match.hasMatch()) {
        return false;
    }

    length = match.capturedLength(0);
    return length > 0;
}

bool isTechnicalTermAt(const QString &text, qsizetype index, qsizetype &length)
{
    static const QRegularExpression technicalExpression(
        QStringLiteral(R"((?:[A-Za-z][A-Za-z0-9]*(?:\+\+|#)|\.[A-Za-z][A-Za-z0-9]+|[A-Za-z][A-Za-z0-9]*(?:\.[A-Za-z0-9]+)+))"));

    const QRegularExpressionMatch match = technicalExpression.match(text,
                                                                    index,
                                                                    QRegularExpression::NormalMatch,
                                                                    QRegularExpression::AnchorAtOffsetMatchOption);
    if (!match.hasMatch()) {
        return false;
    }

    length = match.capturedLength(0);
    return length > 0;
}

bool isNumberSeparator(const QString &text, qsizetype index)
{
    const QChar character = text.at(index);
    if (character != QLatin1Char('.') && character != QLatin1Char(',')
        && character != QLatin1Char('/') && character != QLatin1Char(':')) {
        return false;
    }

    return index > 0
        && (index + 1) < text.size()
        && text.at(index - 1).isDigit()
        && text.at(index + 1).isDigit();
}

bool isPunctuationCharacter(QChar character)
{
    static const QString punctuation = QStringLiteral(".,;:!?()[]{}\"'");
    return punctuation.contains(character);
}

QList<SurfaceToken> tokenizeSurface(const QString &text)
{
    QList<SurfaceToken> tokens;
    qsizetype wordIndex = 0;

    for (qsizetype index = 0; index < text.size();) {
        qsizetype shortcutLength = 0;
        if (isShortcutAt(text, index, shortcutLength)) {
            tokens.append(SurfaceToken{SurfaceTokenType::Shortcut, text.mid(index, shortcutLength), -1, index});
            index += shortcutLength;
            continue;
        }

        qsizetype technicalLength = 0;
        if (isTechnicalTermAt(text, index, technicalLength)) {
            tokens.append(SurfaceToken{SurfaceTokenType::Technical, text.mid(index, technicalLength), -1, index});
            index += technicalLength;
            continue;
        }

        const QChar character = text.at(index);
        if (character.isSpace()) {
            const qsizetype start = index;
            while (index < text.size() && text.at(index).isSpace()) {
                ++index;
            }
            tokens.append(SurfaceToken{SurfaceTokenType::Space, text.mid(start, index - start), -1, start});
            continue;
        }

        qsizetype letterLength = 1;
        if (isLetterAt(text, index, &letterLength)) {
            const qsizetype start = index;
            index += letterLength;
            while (index < text.size()) {
                qsizetype continuationLength = 1;
                if (isWordContinuationAt(text, index, &continuationLength)) {
                    index += continuationLength;
                    continue;
                }
                if (isIntraWordMark(text, index)) {
                    ++index;
                    continue;
                }
                break;
            }
            tokens.append(SurfaceToken{SurfaceTokenType::Word, text.mid(start, index - start), wordIndex, start});
            ++wordIndex;
            continue;
        }

        if (character.isDigit()) {
            const qsizetype start = index;
            ++index;
            while (index < text.size()
                   && (text.at(index).isDigit() || isNumberSeparator(text, index))) {
                ++index;
            }
            tokens.append(SurfaceToken{SurfaceTokenType::Number, text.mid(start, index - start), -1, start});
            continue;
        }

        tokens.append(SurfaceToken{isPunctuationCharacter(character) ? SurfaceTokenType::Punctuation : SurfaceTokenType::Symbol,
                                   QString(character),
                                   -1,
                                   index});
        ++index;
    }

    return tokens;
}

QString replaceTokenSpanWords(const QList<SurfaceToken> &tokens,
                              qsizetype firstTokenIndex,
                              qsizetype lastTokenIndex,
                              const QString &translation,
                              qsizetype consumedWords)
{
    const QStringList translatedWords = translation.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (translatedWords.size() != consumedWords) {
        return translation;
    }

    QString rebuilt;
    qsizetype translatedIndex = 0;
    for (qsizetype index = firstTokenIndex; index <= lastTokenIndex; ++index) {
        const SurfaceToken &token = tokens.at(index);
        if (token.type == SurfaceTokenType::Word) {
            rebuilt.append(translatedWords.at(translatedIndex));
            ++translatedIndex;
        } else {
            rebuilt.append(token.text);
        }
    }

    return rebuilt;
}

bool hasProtectedTokenInsideSpan(const QList<SurfaceToken> &tokens,
                                 qsizetype firstTokenIndex,
                                 qsizetype lastTokenIndex)
{
    for (qsizetype index = firstTokenIndex; index <= lastTokenIndex; ++index) {
        const SurfaceToken &token = tokens.at(index);
        if (token.type != SurfaceTokenType::Word && token.type != SurfaceTokenType::Space) {
            return true;
        }
    }

    return false;
}

bool canApplyMatchToTokenSpan(const QList<SurfaceToken> &tokens,
                              qsizetype firstTokenIndex,
                              qsizetype lastTokenIndex,
                              const QString &translation,
                              qsizetype consumedWords)
{
    const QStringList translatedWords = translation.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (translatedWords.size() == consumedWords) {
        return true;
    }

    return !hasProtectedTokenInsideSpan(tokens, firstTokenIndex, lastTokenIndex);
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

void TranslatorEngine::shutdown()
{
    m_ready = false;
    m_databaseManager.close();
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
            translatedSegment = applyCapitalization(segment.text, translatedSegment);
            translatedSegment.append(segment.delimiter);
            translatedSegments.append(translatedSegment);

            if (result.matchType == QStringLiteral("none") && segmentResult.matchType != QStringLiteral("none")) {
                result.matchedText = segmentResult.matchedText;
                result.matchType = segmentResult.matchType;
                result.consumedWords = segmentResult.consumedWords;
            }
            result.matchesFound += segmentResult.matchesFound;
            result.unknownWords += segmentResult.unknownWords;
        }

        return leadingWhitespace + translatedSegments.join(QString()) + trailingWhitespace;
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
    if (result.matchType == QStringLiteral("none")) {
        result.matchType = QStringLiteral("fallback");
    }
    recordTranslationTime(result.translationTimeNs);

    QString report;
    QTextStream reportStream(&report);
    reportStream << "[TRADUCAO]" << Qt::endl;
    reportStream << "Data/hora: " << AtlasReport::timestamp() << Qt::endl;
    reportStream << "Idioma de origem: " << normalizedSourceLang << Qt::endl;
    reportStream << "Idioma de destino: " << normalizedTargetLang << Qt::endl;
    reportStream << Qt::endl;
    reportStream << "Entrada:" << Qt::endl << text.left(2000) << Qt::endl;
    reportStream << Qt::endl;
    reportStream << "Saida:" << Qt::endl << result.translation.left(2000) << Qt::endl;
    reportStream << Qt::endl;
    reportStream << "Tipo: " << result.matchType << Qt::endl;
    reportStream << "Tempo: " << AtlasReport::formatMilliseconds(result.translationTimeNs) << Qt::endl;
    reportStream << "Correspondencias: " << result.matchesFound << Qt::endl;
    reportStream << "Palavras desconhecidas: " << result.unknownWords << Qt::endl;
    AtlasReport::append(AtlasReport::File::Translate, report);
    AtlasReport::appendFlux(QStringLiteral("INFO"),
                            QStringLiteral("Traducao executada"),
                            QStringLiteral("Par: %1 -> %2\nTempo: %3\nTipo: %4")
                                .arg(normalizedSourceLang,
                                     normalizedTargetLang,
                                     AtlasReport::formatMilliseconds(result.translationTimeNs),
                                     result.matchType));
    return result;
}

TranslatorEngine::TranslationResult TranslatorEngine::translateSegment(const QString &text,
                                                                       const QString &sourceLang,
                                                                       const QString &targetLang) const
{
    TranslationResult result;
    result.input = text;

    const QList<SurfaceToken> tokens = tokenizeSurface(text);
    QStringList words;
    QList<qsizetype> wordTokenIndexes;
    for (qsizetype index = 0; index < tokens.size(); ++index) {
        if (tokens.at(index).type != SurfaceTokenType::Word) {
            continue;
        }

        const QString normalizedWord = m_normalizer.normalizeForLookup(tokens.at(index).text);
        if (normalizedWord.isEmpty()) {
            continue;
        }

        words.append(normalizedWord);
        wordTokenIndexes.append(index);
    }

    if (words.isEmpty()) {
        result.translation = text;
        return result;
    }

    const PhraseMatch completeMatch = findBestMatch(words, 0, sourceLang, targetLang);
    if (completeMatch.found && completeMatch.consumedWords == words.size()) {
        const qsizetype firstTokenIndex = wordTokenIndexes.first();
        const qsizetype lastTokenIndex = wordTokenIndexes.at(completeMatch.consumedWords - 1);
        const SurfaceToken &firstToken = tokens.at(firstTokenIndex);
        const SurfaceToken &lastToken = tokens.at(lastTokenIndex);
        const QString cleanedTranslation = cleanStoredTranslationForSource(text, completeMatch.translation);
        if (canApplyMatchToTokenSpan(tokens,
                                     firstTokenIndex,
                                     lastTokenIndex,
                                     cleanedTranslation,
                                     completeMatch.consumedWords)) {
            result.translation = replaceTokenSpanWords(tokens,
                                                       firstTokenIndex,
                                                       lastTokenIndex,
                                                       cleanedTranslation,
                                                       completeMatch.consumedWords);
            result.translation.prepend(text.left(firstToken.start));
            result.translation.append(text.mid(lastToken.start + lastToken.text.size()));
            result.matchedText = completeMatch.matchedText;
            result.matchType = QStringLiteral("full");
            result.consumedWords = completeMatch.consumedWords;
            result.matchesFound = completeMatch.consumedWords;
            result.unknownWords = 0;
            logMatch(text, completeMatch, sourceLang, targetLang);
            return result;
        }
    }

    QString translated;

    for (qsizetype position = 0; position < words.size();) {
        const qsizetype firstTokenIndex = wordTokenIndexes.at(position);
        const SurfaceToken &firstToken = tokens.at(firstTokenIndex);
        const qsizetype previousTokenEnd = position == 0
            ? 0
            : tokens.at(wordTokenIndexes.at(position - 1)).start + tokens.at(wordTokenIndexes.at(position - 1)).text.size();
        translated.append(text.mid(previousTokenEnd, firstToken.start - previousTokenEnd));

        const PhraseMatch match = findBestMatch(words, position, sourceLang, targetLang);
        if (match.found) {
            const qsizetype lastConsumedWord = position + match.consumedWords - 1;
            const qsizetype lastTokenIndex = wordTokenIndexes.at(lastConsumedWord);
            const QString cleanedTranslation = cleanStoredTranslationForSource(match.matchedText, match.translation);
            if (canApplyMatchToTokenSpan(tokens,
                                         firstTokenIndex,
                                         lastTokenIndex,
                                         cleanedTranslation,
                                         match.consumedWords)) {
                translated.append(replaceTokenSpanWords(tokens,
                                                        firstTokenIndex,
                                                        lastTokenIndex,
                                                        cleanedTranslation,
                                                        match.consumedWords));
                if (result.matchType == QStringLiteral("none")) {
                    result.matchedText = match.matchedText;
                    result.matchType = match.matchType;
                    result.consumedWords = match.consumedWords;
                }
                logMatch(text, match, sourceLang, targetLang);
                result.matchesFound += match.consumedWords;
                position += match.consumedWords;
                continue;
            }
        }

        translated.append(tokens.at(firstTokenIndex).text);
        ++result.unknownWords;
        ++position;
    }

    const qsizetype lastWordTokenIndex = wordTokenIndexes.last();
    translated.append(text.mid(tokens.at(lastWordTokenIndex).start + tokens.at(lastWordTokenIndex).text.size()));
    result.translation = translated;
    if (result.matchesFound == 0 && result.unknownWords > 0 && m_neuralTranslator.isEnabled()) {
        const NeuralTranslator::Result neuralResult = m_neuralTranslator.translate(text, sourceLang, targetLang);
        if (neuralResult.translated) {
            result.translation = neuralResult.translation;
            result.matchType = QStringLiteral("neural");
            result.matchedText = text;
            result.consumedWords = words.size();
            result.matchesFound = words.size();
            result.unknownWords = 0;
            ++m_statistics.neuralTranslations;
            if (m_debugEnabled) {
                QTextStream output(stdout);
                configureUtf8Stream(output);
                output << "[NEURAL] " << sourceLang << " -> " << targetLang
                       << " | input=\"" << text << "\""
                       << " | translation=\"" << neuralResult.translation << "\""
                       << Qt::endl;
            }
        } else {
            ++m_statistics.neuralFailures;
            if (m_debugEnabled && !neuralResult.error.isEmpty()) {
                QTextStream output(stdout);
                configureUtf8Stream(output);
                output << "[NEURAL ERROR] " << neuralResult.error << Qt::endl;
            }
        }
    }
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
    output << "Estatisticas do Atlas-Translator:" << Qt::endl;
    output << "Total de traducoes: " << m_statistics.totalTranslations << Qt::endl;
    output << "Acertos de cache: " << m_statistics.cacheHits << Qt::endl;
    output << "Falhas de cache: " << m_statistics.cacheMisses << Qt::endl;
    output << "Traducoes neurais: " << m_statistics.neuralTranslations << Qt::endl;
    output << "Falhas neurais: " << m_statistics.neuralFailures << Qt::endl;
    output << "Tempo medio de traducao: " << QString::number(averageTranslationTimeMs(), 'f', 3) << " ms" << Qt::endl;
    output << "Consultas SQL: " << sqlStats.queryCount << Qt::endl;
    output << "Tempo medio SQL: " << QString::number(m_databaseManager.averageSqlQueryTimeMs(), 'f', 3) << " ms" << Qt::endl;
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
