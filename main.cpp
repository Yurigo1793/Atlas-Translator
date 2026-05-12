#include "core/TranslatorEngine.h"

#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream input(stdin);
    QTextStream output(stdout);

    TranslatorEngine translator;
    if (!translator.initialize()) {
        output << "Erro ao inicializar o Atlas-Translator: " << translator.lastError() << Qt::endl;
        return 1;
    }

    output << "Atlas-Translator offline iniciado. Digite 'exit' para sair." << Qt::endl;

    while (true) {
        output << "Digite: " << Qt::flush;
        const QString line = input.readLine().trimmed();

        if (line.compare(QStringLiteral("exit"), Qt::CaseInsensitive) == 0) {
            break;
        }

        output << translator.translate(line) << Qt::endl;
    }

    return 0;
}
