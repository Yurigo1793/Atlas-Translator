#include "TextNormalizer.h"

#include <QRegularExpression>
#include <QSet>

QString TextNormalizer::normalizeWhitespace(const QString &text) const
{
    QString normalized = text;
    static const QRegularExpression lineBreakExpression(QStringLiteral("[\\t\\r\\n]+"));
    static const QRegularExpression whitespaceExpression(QStringLiteral("\\s+"));
    normalized.replace(lineBreakExpression, QStringLiteral(" "));
    normalized.replace(whitespaceExpression, QStringLiteral(" "));
    return normalized.trimmed();
}

QString TextNormalizer::normalizeForLookup(const QString &text) const
{
    QString normalized = text.toLower();

    const bool hasContractionMark = normalized.contains(QLatin1Char('\''))
        || normalized.contains(QChar(0x2018))
        || normalized.contains(QChar(0x2019));

    normalized.replace(QChar(0x2018), QLatin1Char('\''));
    normalized.replace(QChar(0x2019), QLatin1Char('\''));
    normalized.replace(QChar(0x201C), QLatin1Char('"'));
    normalized.replace(QChar(0x201D), QLatin1Char('"'));
    if (hasContractionMark) {
        static const QRegularExpression cantExpression(QStringLiteral("\\bcan't\\b"));
        static const QRegularExpression wontExpression(QStringLiteral("\\bwon't\\b"));
        static const QRegularExpression ntExpression(QStringLiteral("\\bn't\\b"));
        static const QRegularExpression imExpression(QStringLiteral("\\bI'm\\b"), QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression reExpression(QStringLiteral("\\b(\\w+)'re\\b"));
        static const QRegularExpression llExpression(QStringLiteral("\\b(\\w+)'ll\\b"));
        static const QRegularExpression veExpression(QStringLiteral("\\b(\\w+)'ve\\b"));
        static const QRegularExpression dExpression(QStringLiteral("\\b(\\w+)'d\\b"));
        static const QRegularExpression sExpression(QStringLiteral("\\b(\\w+)'s\\b"));
        normalized.replace(cantExpression, QStringLiteral("cannot"));
        normalized.replace(wontExpression, QStringLiteral("will not"));
        normalized.replace(ntExpression, QStringLiteral(" not"));
        normalized.replace(imExpression, QStringLiteral("i am"));
        normalized.replace(reExpression, QStringLiteral("\\1 are"));
        normalized.replace(llExpression, QStringLiteral("\\1 will"));
        normalized.replace(veExpression, QStringLiteral("\\1 have"));
        normalized.replace(dExpression, QStringLiteral("\\1 would"));
        normalized.replace(sExpression, QStringLiteral("\\1"));
    }

    // Lookup is word-based: punctuation, symbols, and visual separators become
    // spaces so they do not block phrase matches.
    static const QRegularExpression nonWordExpression(QStringLiteral("[^\\p{L}\\p{N}]+"));
    normalized.replace(nonWordExpression, QStringLiteral(" "));

    return normalizeWhitespace(normalized);
}

QStringList TextNormalizer::words(const QString &text) const
{
    const QString normalized = normalizeForLookup(text);
    return normalized.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

QStringList TextNormalizer::lookupCandidates(const QStringList &words) const
{
    QStringList candidates;
    const QString exactPhrase = normalizeForLookup(words.join(QStringLiteral(" ")));
    if (!exactPhrase.isEmpty()) {
        candidates.append(exactPhrase);
    }

    static const QSet<QString> optionalWords = {
        QStringLiteral("a"),
        QStringLiteral("an"),
        QStringLiteral("the"),
        QStringLiteral("please"),
        QStringLiteral("just")
    };

    QStringList compactWords;
    compactWords.reserve(words.size());
    for (const QString &word : words) {
        if (!optionalWords.contains(word)) {
            compactWords.append(word);
        }
    }

    const QString compactPhrase = normalizeForLookup(compactWords.join(QStringLiteral(" ")));
    if (compactWords.size() >= 2 && !compactPhrase.isEmpty() && !candidates.contains(compactPhrase)) {
        candidates.append(compactPhrase);
    }

    return candidates;
}
