#include "TranslatorEngine.h"

TranslatorEngine::TranslatorEngine(const QString &databasePath)
    : m_databaseManager(databasePath)
{
}

bool TranslatorEngine::initialize()
{
    return m_databaseManager.initialize();
}

QString TranslatorEngine::translate(const QString &text,
                                    const QString &sourceLang,
                                    const QString &targetLang) const
{
    const QString cleanText = m_tokenizer.cleanBasicPunctuation(text).toLower().trimmed();
    if (cleanText.isEmpty()) {
        return QString();
    }

    if (const auto fullPhrase = m_databaseManager.findTranslation(cleanText, sourceLang, targetLang)) {
        return *fullPhrase;
    }

    QStringList translatedWords;
    const QStringList words = m_tokenizer.splitWords(cleanText);
    translatedWords.reserve(words.size());

    for (const QString &word : words) {
        if (const auto translatedWord = m_databaseManager.findTranslation(word, sourceLang, targetLang)) {
            translatedWords.append(*translatedWord);
        } else {
            translatedWords.append(word);
        }
    }

    return translatedWords.join(QStringLiteral(" "));
}

QString TranslatorEngine::lastError() const
{
    return m_databaseManager.lastError();
}
