#ifndef TOKENIZER_H
#define TOKENIZER_H

#include <QString>
#include <QStringList>

class Tokenizer
{
public:
    QStringList splitSentences(const QString &text) const;
    QStringList splitWords(const QString &text) const;
    QString cleanBasicPunctuation(const QString &text) const;
};

#endif // TOKENIZER_H
