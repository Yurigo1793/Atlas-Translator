#include "TextNormalizer.h"

#include <QRegularExpression>
#include <QSet>

QString TextNormalizer::normalizeWhitespace(const QString &text) const
{
    QString normalized = text;
    normalized.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return normalized.trimmed();
}

QString TextNormalizer::normalizeForLookup(const QString &text) const
{
    QString normalized = text.toLower();

    // Pontuação leve é ignorada no índice de busca. Acentos, UTF-8 e pontuação
    // estrutural (por exemplo '/', '-', '+', '#') são preservados.
    normalized.replace(QRegularExpression(QStringLiteral("[\\.,;:!?\\(\\)\\[\\]\\{\\}\\\"']")), QStringLiteral(" "));

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
        QStringLiteral("the")
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
