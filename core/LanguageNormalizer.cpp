#include "LanguageNormalizer.h"

#include <QHash>
#include <QSet>
#include <QStringList>

namespace {

QString titleCaseScript(QString script)
{
    script = script.toLower();
    if (!script.isEmpty()) {
        script[0] = script.at(0).toUpper();
    }
    return script;
}

const QHash<QString, QString> &languageAliases()
{
    static const QHash<QString, QString> aliases = {
        {QStringLiteral("por"), QStringLiteral("pt_BR")},
        {QStringLiteral("ptbr"), QStringLiteral("pt_BR")},
        {QStringLiteral("pt_br"), QStringLiteral("pt_BR")},
        {QStringLiteral("pt_brazil"), QStringLiteral("pt_BR")},
        {QStringLiteral("pt-pt"), QStringLiteral("pt_PT")},
        {QStringLiteral("pt_pt"), QStringLiteral("pt_PT")},
        {QStringLiteral("eng"), QStringLiteral("en")},
        {QStringLiteral("en_us"), QStringLiteral("en")},
        {QStringLiteral("en_gb"), QStringLiteral("en")},
        {QStringLiteral("spa"), QStringLiteral("es")},
        {QStringLiteral("es_es"), QStringLiteral("es")},
        {QStringLiteral("es_mx"), QStringLiteral("es")},
        {QStringLiteral("fra"), QStringLiteral("fr")},
        {QStringLiteral("fre"), QStringLiteral("fr")},
        {QStringLiteral("fr_fr"), QStringLiteral("fr")},
        {QStringLiteral("fr_ca"), QStringLiteral("fr")},
        {QStringLiteral("deu"), QStringLiteral("de")},
        {QStringLiteral("ger"), QStringLiteral("de")},
        {QStringLiteral("ita"), QStringLiteral("it")},
        {QStringLiteral("nld"), QStringLiteral("nl")},
        {QStringLiteral("dut"), QStringLiteral("nl")},
        {QStringLiteral("rus"), QStringLiteral("ru")},
        {QStringLiteral("ukr"), QStringLiteral("uk")},
        {QStringLiteral("pol"), QStringLiteral("pl")},
        {QStringLiteral("ces"), QStringLiteral("cs")},
        {QStringLiteral("cze"), QStringLiteral("cs")},
        {QStringLiteral("swe"), QStringLiteral("sv")},
        {QStringLiteral("dan"), QStringLiteral("da")},
        {QStringLiteral("fin"), QStringLiteral("fi")},
        {QStringLiteral("tur"), QStringLiteral("tr")},
        {QStringLiteral("ara"), QStringLiteral("ar")},
        {QStringLiteral("heb"), QStringLiteral("he")},
        {QStringLiteral("fas"), QStringLiteral("fa")},
        {QStringLiteral("per"), QStringLiteral("fa")},
        {QStringLiteral("hin"), QStringLiteral("hi")},
        {QStringLiteral("urd"), QStringLiteral("ur")},
        {QStringLiteral("ben"), QStringLiteral("bn")},
        {QStringLiteral("pan"), QStringLiteral("pa")},
        {QStringLiteral("tam"), QStringLiteral("ta")},
        {QStringLiteral("tel"), QStringLiteral("te")},
        {QStringLiteral("mar"), QStringLiteral("mr")},
        {QStringLiteral("guj"), QStringLiteral("gu")},
        {QStringLiteral("jpn"), QStringLiteral("ja")},
        {QStringLiteral("kor"), QStringLiteral("ko")},
        {QStringLiteral("vie"), QStringLiteral("vi")},
        {QStringLiteral("tha"), QStringLiteral("th")},
        {QStringLiteral("ind"), QStringLiteral("id")},
        {QStringLiteral("msa"), QStringLiteral("ms")},
        {QStringLiteral("afr"), QStringLiteral("af")},
        {QStringLiteral("amh"), QStringLiteral("am")},
        {QStringLiteral("az_az"), QStringLiteral("az_Latn")},
        {QStringLiteral("az_latn_az"), QStringLiteral("az_Latn")},
        {QStringLiteral("bs_latn_ba"), QStringLiteral("bs_Latn")},
        {QStringLiteral("sr_latn_rs"), QStringLiteral("sr_Latn")},
        {QStringLiteral("sr_cyrl_rs"), QStringLiteral("sr_Cyrl")},
        {QStringLiteral("sr_cyrl_ba"), QStringLiteral("sr_Cyrl")},
        {QStringLiteral("uz_latn_uz"), QStringLiteral("uz_Latn")},
        {QStringLiteral("ku_arab_iq"), QStringLiteral("ku_Arab")},
        {QStringLiteral("zh"), QStringLiteral("zh_CN")},
        {QStringLiteral("zh_hans"), QStringLiteral("zh_CN")},
        {QStringLiteral("zh_hans_cn"), QStringLiteral("zh_CN")},
        {QStringLiteral("zh_cn"), QStringLiteral("zh_CN")},
        {QStringLiteral("zh_hant"), QStringLiteral("zh_TW")},
        {QStringLiteral("zh_hant_tw"), QStringLiteral("zh_TW")},
        {QStringLiteral("zh_tw"), QStringLiteral("zh_TW")},
        {QStringLiteral("zho"), QStringLiteral("zh_CN")},
        {QStringLiteral("chi"), QStringLiteral("zh_CN")},
        {QStringLiteral("cmn"), QStringLiteral("zh_CN")},
        {QStringLiteral("no"), QStringLiteral("nb")},
        {QStringLiteral("nor"), QStringLiteral("nb")},
        {QStringLiteral("tl"), QStringLiteral("fil")},
        {QStringLiteral("tgl"), QStringLiteral("fil")},
        {QStringLiteral("ca_es_valencia"), QStringLiteral("ca_ES_valencia")},
        {QStringLiteral("chr_cher_us"), QStringLiteral("chr")},
        {QStringLiteral("quz_pe"), QStringLiteral("quz")}
    };
    return aliases;
}

const QSet<QString> &primaryLanguageCodes()
{
    static const QSet<QString> codes = {
        QStringLiteral("ar"), QStringLiteral("eu"), QStringLiteral("bg"), QStringLiteral("ca"),
        QStringLiteral("zh_CN"), QStringLiteral("zh_TW"), QStringLiteral("hr"), QStringLiteral("cs"),
        QStringLiteral("da"), QStringLiteral("nl"), QStringLiteral("en"), QStringLiteral("et"),
        QStringLiteral("fi"), QStringLiteral("fr"), QStringLiteral("gl"), QStringLiteral("de"),
        QStringLiteral("el"), QStringLiteral("he"), QStringLiteral("hu"), QStringLiteral("id"),
        QStringLiteral("it"), QStringLiteral("ja"), QStringLiteral("ko"), QStringLiteral("lv"),
        QStringLiteral("lt"), QStringLiteral("nb"), QStringLiteral("pl"), QStringLiteral("pt_BR"),
        QStringLiteral("pt_PT"), QStringLiteral("ro"), QStringLiteral("ru"), QStringLiteral("sr_Latn"),
        QStringLiteral("sk"), QStringLiteral("sl"), QStringLiteral("es"), QStringLiteral("sv"),
        QStringLiteral("th"), QStringLiteral("tr"), QStringLiteral("uk"), QStringLiteral("vi")
    };
    return codes;
}

const QSet<QString> &secondaryLanguageCodes()
{
    static const QSet<QString> codes = {
        QStringLiteral("af"), QStringLiteral("sq"), QStringLiteral("am"), QStringLiteral("hy"),
        QStringLiteral("as"), QStringLiteral("az_Latn"), QStringLiteral("bn"), QStringLiteral("be"),
        QStringLiteral("bs_Latn"), QStringLiteral("chr"), QStringLiteral("fil"), QStringLiteral("ka"),
        QStringLiteral("gu"), QStringLiteral("hi"), QStringLiteral("is"), QStringLiteral("ga"),
        QStringLiteral("kn"), QStringLiteral("kk"), QStringLiteral("km"), QStringLiteral("kok"),
        QStringLiteral("lo"), QStringLiteral("lb"), QStringLiteral("mk"), QStringLiteral("ms"),
        QStringLiteral("ml"), QStringLiteral("mt"), QStringLiteral("mi"), QStringLiteral("mr"),
        QStringLiteral("ne"), QStringLiteral("nn"), QStringLiteral("or"), QStringLiteral("fa"),
        QStringLiteral("pa"), QStringLiteral("quz"), QStringLiteral("gd"), QStringLiteral("sr_Cyrl"),
        QStringLiteral("ta"), QStringLiteral("tt"), QStringLiteral("te"), QStringLiteral("ur"),
        QStringLiteral("ug"), QStringLiteral("uz_Latn"), QStringLiteral("ca_ES_valencia"),
        QStringLiteral("cy"), QStringLiteral("ku_Arab"), QStringLiteral("la")
    };
    return codes;
}

const QSet<QString> &supportedLanguageCodes()
{
    static const QSet<QString> codes = [] {
        QSet<QString> result = primaryLanguageCodes();
        result.unite(secondaryLanguageCodes());
        return result;
    }();
    return codes;
}

}

QString LanguageNormalizer::normalize(const QString &languageCode) const
{
    QString normalized = languageCode.trimmed();
    normalized.replace(QLatin1Char('-'), QLatin1Char('_'));

    if (normalized.isEmpty()) {
        return QString();
    }

    const QString lowerCode = normalized.toLower();
    const QString aliased = languageAliases().value(lowerCode);
    if (!aliased.isEmpty()) {
        return aliased;
    }

    const QStringList parts = lowerCode.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    const QString baseLanguage = parts.value(0);

    if (baseLanguage == QStringLiteral("pt")) {
        return parts.value(1) == QStringLiteral("pt") ? QStringLiteral("pt_PT") : QStringLiteral("pt_BR");
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

    if (baseLanguage == QStringLiteral("zh")) {
        return parts.contains(QStringLiteral("tw")) || parts.contains(QStringLiteral("hant"))
            ? QStringLiteral("zh_TW")
            : QStringLiteral("zh_CN");
    }

    if (baseLanguage == QStringLiteral("sr")) {
        return parts.contains(QStringLiteral("latn")) ? QStringLiteral("sr_Latn") : QStringLiteral("sr_Cyrl");
    }

    if (baseLanguage == QStringLiteral("az") && parts.contains(QStringLiteral("latn"))) {
        return QStringLiteral("az_Latn");
    }

    if (baseLanguage == QStringLiteral("bs") && parts.contains(QStringLiteral("latn"))) {
        return QStringLiteral("bs_Latn");
    }

    if (baseLanguage == QStringLiteral("uz") && parts.contains(QStringLiteral("latn"))) {
        return QStringLiteral("uz_Latn");
    }

    if (baseLanguage == QStringLiteral("ku") && parts.contains(QStringLiteral("arab"))) {
        return QStringLiteral("ku_Arab");
    }

    if (baseLanguage == QStringLiteral("ca") && parts.contains(QStringLiteral("valencia"))) {
        return QStringLiteral("ca_ES_valencia");
    }

    if (parts.size() >= 2 && parts.at(1).size() == 4) {
        return QStringLiteral("%1_%2").arg(baseLanguage, titleCaseScript(parts.at(1)));
    }

    return baseLanguage;
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
            QStringLiteral("pt"),
            QStringLiteral("por")
        };
        for (const QString &variant : legacyPortugueseVariants) {
            if (!variants.contains(variant)) {
                variants.append(variant);
            }
        }
    }

    return variants;
}

bool LanguageNormalizer::isSupported(const QString &languageCode) const
{
    const QString canonical = normalize(languageCode);
    return !canonical.isEmpty() && supportedLanguageCodes().contains(canonical);
}

bool LanguageNormalizer::isPrimary(const QString &languageCode) const
{
    const QString canonical = normalize(languageCode);
    return !canonical.isEmpty() && primaryLanguageCodes().contains(canonical);
}
