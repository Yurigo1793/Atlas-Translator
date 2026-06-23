#ifndef NEURALTRANSLATOR_H
#define NEURALTRANSLATOR_H

#include <QString>
#include <QtGlobal>

class NeuralTranslator
{
public:
    struct Result {
        bool translated = false;
        QString translation;
        QString error;
        qint64 elapsedNs = 0;
    };

    bool isEnabled() const;
    QString commandTemplate() const;
    Result translate(const QString &text, const QString &sourceLang, const QString &targetLang) const;
};

#endif // NEURALTRANSLATOR_H
