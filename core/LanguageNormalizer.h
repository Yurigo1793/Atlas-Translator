#ifndef LANGUAGENORMALIZER_H
#define LANGUAGENORMALIZER_H

#include <QString>
#include <QStringList>

class LanguageNormalizer
{
public:
    QString normalize(const QString &languageCode) const;
    QStringList lookupVariants(const QString &languageCode) const;
};

#endif // LANGUAGENORMALIZER_H
