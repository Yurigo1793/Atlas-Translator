#include "LanguageNormalizer.h"

#include <QStringList>

QString LanguageNormalizer::normalize(const QString &languageCode) const
{
    QString normalized = languageCode.trimmed();
    normalized.replace(QLatin1Char('-'), QLatin1Char('_'));

    if (normalized.isEmpty()) {
        return QString();
    }

    const QString lowerCode = normalized.toLower();
    const QString baseLanguage = lowerCode.section(QLatin1Char('_'), 0, 0);

    if (baseLanguage == QStringLiteral("pt")
        || lowerCode == QStringLiteral("por")
        || lowerCode == QStringLiteral("ptbr")) {
        return QStringLiteral("pt_BR");
    }

    if (baseLanguage == QStringLiteral("en")) {
        return QStringLiteral("en");
    }

    if (baseLanguage == QStringLiteral("fr")) {
        return QStringLiteral("fr");
    }

    if (baseLanguage == QStringLiteral("es")) {
        return QStringLiteral("es");
    }

    const QStringList parts = lowerCode.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    if (parts.size() >= 2) {
        return QStringLiteral("%1_%2").arg(parts.at(0), parts.at(1).toUpper());
    }

    return lowerCode;
}

QStringList LanguageNormalizer::lookupVariants(const QString &languageCode) const
{
    QStringList variants;
    const QString canonical = normalize(languageCode);
    if (!canonical.isEmpty()) {
        variants.append(canonical);
    }

    QString original = languageCode.trimmed();
    original.replace(QLatin1Char('-'), QLatin1Char('_'));
    const QString lowerOriginal = original.toLower();
    if (!lowerOriginal.isEmpty() && !variants.contains(lowerOriginal)) {
        variants.append(lowerOriginal);
    }

    if (canonical == QStringLiteral("pt_BR")) {
        const QStringList legacyPortugueseVariants = {
            QStringLiteral("pt_br"),
            QStringLiteral("pt")
        };
        for (const QString &variant : legacyPortugueseVariants) {
            if (!variants.contains(variant)) {
                variants.append(variant);
            }
        }
    }

    return variants;
}
