#ifndef TOKENIZER_H
#define TOKENIZER_H

#include "TextNormalizer.h"

#include <QString>
#include <QStringList>

class Tokenizer
{
public:
    QStringList splitSentences(const QString &text) const;
    QStringList splitWords(const QString &text) const;
    QString cleanBasicPunctuation(const QString &text) const;
    QString normalizeForLookup(const QString &text) const;

private:
    TextNormalizer m_normalizer;
};

#endif // TOKENIZER_H
