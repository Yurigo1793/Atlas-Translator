#ifndef TRANSLATORENGINE_H
#define TRANSLATORENGINE_H

#include "DatabaseManager.h"
#include "Tokenizer.h"

#include <QString>

class TranslatorEngine
{
public:
    explicit TranslatorEngine(const QString &databasePath = QString());

    bool initialize();
    QString translate(const QString &text,
                      const QString &sourceLang = QStringLiteral("en"),
                      const QString &targetLang = QStringLiteral("pt")) const;
    QString lastError() const;

private:
    DatabaseManager m_databaseManager;
    Tokenizer m_tokenizer;
};

#endif // TRANSLATORENGINE_H
