#include "Tokenizer.h"

#include <QRegularExpression>

QStringList Tokenizer::splitSentences(const QString &text) const
{
    const QString cleaned = m_normalizer.normalizeWhitespace(text);
    return cleaned.split(QRegularExpression(QStringLiteral("[.!?]+\\s*")), Qt::SkipEmptyParts);
}

QStringList Tokenizer::splitWords(const QString &text) const
{
    return m_normalizer.words(text);
}

QString Tokenizer::cleanBasicPunctuation(const QString &text) const
{
    return m_normalizer.normalizeForLookup(text);
}

QString Tokenizer::normalizeForLookup(const QString &text) const
{
    return m_normalizer.normalizeForLookup(text);
}
