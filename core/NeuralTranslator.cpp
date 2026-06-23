#include "NeuralTranslator.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QStringList>

namespace {
QString neuralCommandTemplate()
{
    return qEnvironmentVariable("ATLAS_NEURAL_TRANSLATOR_CMD").trimmed();
}

int neuralTimeoutMs()
{
    bool valid = false;
    const int configuredTimeout = qEnvironmentVariableIntValue("ATLAS_NEURAL_TRANSLATOR_TIMEOUT_MS", &valid);
    if (valid && configuredTimeout > 0) {
        return configuredTimeout;
    }

    return 10000;
}

QString commandQuote(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(value);
}

QString commandLineFor(const QString &commandTemplate,
                       const QString &text,
                       const QString &sourceLang,
                       const QString &targetLang)
{
    QString commandLine = commandTemplate;
    if (commandLine.contains(QStringLiteral("{source}"))
        || commandLine.contains(QStringLiteral("{target}"))
        || commandLine.contains(QStringLiteral("{text}"))) {
        commandLine.replace(QStringLiteral("{source}"), commandQuote(sourceLang));
        commandLine.replace(QStringLiteral("{target}"), commandQuote(targetLang));
        commandLine.replace(QStringLiteral("{text}"), commandQuote(text));
        return commandLine;
    }

    return QStringLiteral("%1 %2 %3")
        .arg(commandLine, commandQuote(sourceLang), commandQuote(targetLang));
}
}

bool NeuralTranslator::isEnabled() const
{
    return !neuralCommandTemplate().isEmpty();
}

QString NeuralTranslator::commandTemplate() const
{
    return neuralCommandTemplate();
}

NeuralTranslator::Result NeuralTranslator::translate(const QString &text,
                                                     const QString &sourceLang,
                                                     const QString &targetLang) const
{
    Result result;
    const QString configuredCommand = neuralCommandTemplate();
    if (configuredCommand.isEmpty() || text.trimmed().isEmpty()) {
        return result;
    }

    QElapsedTimer timer;
    timer.start();

    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.startCommand(commandLineFor(configuredCommand, text, sourceLang, targetLang));
    if (!process.waitForStarted(neuralTimeoutMs())) {
        result.error = process.errorString();
        result.elapsedNs = timer.nsecsElapsed();
        return result;
    }

    process.write(text.toUtf8());
    process.closeWriteChannel();

    if (!process.waitForFinished(neuralTimeoutMs())) {
        process.kill();
        process.waitForFinished(1000);
        result.error = QStringLiteral("Tempo limite excedido ao executar backend neural");
        result.elapsedNs = timer.nsecsElapsed();
        return result;
    }

    result.elapsedNs = timer.nsecsElapsed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
        result.error = stderrText.isEmpty() ? process.errorString() : stderrText;
        return result;
    }

    result.translation = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    result.translated = !result.translation.isEmpty();
    return result;
}
