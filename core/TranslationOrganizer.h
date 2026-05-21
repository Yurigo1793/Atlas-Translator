#ifndef TRANSLATIONORGANIZER_H
#define TRANSLATIONORGANIZER_H

#include <QString>

class TranslationOrganizer
{
public:
    struct PairDecision {
        bool accepted = true;
        QString sourceText;
        QString targetText;
        QString reason;
    };

    PairDecision organizePair(const QString &sourceText,
                              const QString &targetText,
                              const QString &sourceLang,
                              const QString &targetLang) const;
};

#endif // TRANSLATIONORGANIZER_H
