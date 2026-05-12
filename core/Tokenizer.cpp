#include "Tokenizer.h"

#include <QRegularExpression>

QStringList Tokenizer::splitSentences(const QString &text) const
{
    const QString cleaned = text.trimmed();
    return cleaned.split(QRegularExpression(QStringLiteral("[.!?]+\\s*")), Qt::SkipEmptyParts);
}

QStringList Tokenizer::splitWords(const QString &text) const
{
    const QString cleaned = cleanBasicPunctuation(text).toLower().trimmed();
    return cleaned.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

QString Tokenizer::cleanBasicPunctuation(const QString &text) const
{
    QString cleaned = text;
    cleaned.replace(QRegularExpression(QStringLiteral("[\\.,;:!?\\(\\)\\[\\]\\{\\}\"']")), QStringLiteral(" "));
    cleaned.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return cleaned.trimmed();
}
