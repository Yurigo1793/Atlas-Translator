#include "TranslationOrganizer.h"

#include <QtGlobal>

TranslationOrganizer::PairDecision TranslationOrganizer::organizePair(const QString &sourceText,
                                                                      const QString &targetText,
                                                                      const QString &sourceLang,
                                                                      const QString &targetLang) const
{
    Q_UNUSED(sourceLang);
    Q_UNUSED(targetLang);

    PairDecision decision;
    decision.sourceText = sourceText;
    decision.targetText = targetText;
    decision.reason = QStringLiteral("rule-based-pass-through");
    return decision;
}
