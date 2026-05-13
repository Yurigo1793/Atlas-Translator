#include "TextNormalizer.h"

#include <QRegularExpression>
#include <QSet>

QString TextNormalizer::normalizeWhitespace(const QString &text) const
{
    QString normalized = text;
    normalized.replace(QRegularExpression(QStringLiteral("[\\t\\r\\n]+")), QStringLiteral(" "));
    normalized.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return normalized.trimmed();
}

QString TextNormalizer::normalizeForLookup(const QString &text) const
{
    QString normalized = text.toLower();

    normalized.replace(QChar(0x2018), QLatin1Char('\''));
    normalized.replace(QChar(0x2019), QLatin1Char('\''));
    normalized.replace(QChar(0x201C), QLatin1Char('"'));
    normalized.replace(QChar(0x201D), QLatin1Char('"'));
    normalized.replace(QRegularExpression(QStringLiteral("\\bcan't\\b")), QStringLiteral("cannot"));
    normalized.replace(QRegularExpression(QStringLiteral("\\bwon't\\b")), QStringLiteral("will not"));
    normalized.replace(QRegularExpression(QStringLiteral("\\bn't\\b")), QStringLiteral(" not"));
    normalized.replace(QRegularExpression(QStringLiteral("\\bI'm\\b"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("i am"));
    normalized.replace(QRegularExpression(QStringLiteral("\\b(\\w+)'re\\b")), QStringLiteral("\\1 are"));
    normalized.replace(QRegularExpression(QStringLiteral("\\b(\\w+)'ll\\b")), QStringLiteral("\\1 will"));
    normalized.replace(QRegularExpression(QStringLiteral("\\b(\\w+)'ve\\b")), QStringLiteral("\\1 have"));
    normalized.replace(QRegularExpression(QStringLiteral("\\b(\\w+)'d\\b")), QStringLiteral("\\1 would"));
    normalized.replace(QRegularExpression(QStringLiteral("\\b(\\w+)'s\\b")), QStringLiteral("\\1"));

    // Pontuação leve é ignorada no índice de busca. Acentos, UTF-8 e pontuação
    // estrutural (por exemplo '/', '+', '#') são preservados. Hífens também geram
    // espaços para permitir correspondência entre "open-source" e "open source".
    normalized.replace(QRegularExpression(QStringLiteral("[\\.,;:!?\\(\\)\\[\\]\\{\\}\\\"']")), QStringLiteral(" "));
    normalized.replace(QRegularExpression(QStringLiteral("(?<=\\p{L})-(?=\\p{L})")), QStringLiteral(" "));

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
