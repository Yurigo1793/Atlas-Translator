#ifndef TEXTNORMALIZER_H
#define TEXTNORMALIZER_H

#include <QString>
#include <QStringList>

class TextNormalizer
{
public:
    QString normalizeWhitespace(const QString &text) const;
    QString normalizeForLookup(const QString &text) const;
    QStringList words(const QString &text) const;
    QStringList lookupCandidates(const QStringList &words) const;
};

#endif // TEXTNORMALIZER_H
