#include "core/AppPaths.h"
#include "core/AtlasImporter.h"
#include "core/DatasetScanner.h"
#include "core/TranslatorEngine.h"

#include <QCoreApplication>
#include <QList>
#include <QTextStream>

namespace {
constexpr bool detailedDebug = false;

QString readLanguage(QTextStream &input,
                     QTextStream &output,
                     const QString &label,
                     const QString &defaultLanguage)
{
    output << label << " [" << defaultLanguage << "]: " << Qt::flush;
    const QString language = input.readLine().trimmed();
    return language.isEmpty() ? defaultLanguage : language;
}

void translateText(TranslatorEngine &translator, QTextStream &input, QTextStream &output)
{
    const QString sourceLang = readLanguage(input, output, QStringLiteral("Idioma de origem"), QStringLiteral("en"));
    const QString targetLang = readLanguage(input, output, QStringLiteral("Idioma de destino"), QStringLiteral("pt_BR"));

    output << "Digite o texto para traduzir (ou 'exit' para voltar):" << Qt::endl;

    while (true) {
        output << "Digite: " << Qt::flush;
        const QString line = input.readLine().trimmed();

        if (line.compare(QStringLiteral("exit"), Qt::CaseInsensitive) == 0) {
            break;
        }

        const TranslatorEngine::TranslationResult result = translator.translateDetailed(line, sourceLang, targetLang);
        output << result.translation << Qt::endl;
        output << "Tempo: " << QString::number(static_cast<double>(result.translationTimeNs) / 1000000.0, 'f', 3)
               << " ms | Tipo: " << result.matchType << Qt::endl;
    }
}

void printDatasets(const QList<DatasetInfo> &datasets, QTextStream &output)
{
    if (datasets.isEmpty()) {
        output << "Nenhum dataset OPUS/Moses válido foi encontrado." << Qt::endl;
        return;
    }

    for (qsizetype index = 0; index < datasets.size(); ++index) {
        const DatasetInfo &dataset = datasets.at(index);
        output << "[" << index << "] " << dataset.corpusName
               << " (" << dataset.sourceLanguage << " -> " << dataset.targetLanguage << ")"
               << Qt::endl;
    }
}

bool importDetectedDataset(QTextStream &input, QTextStream &output)
{
    DatasetScanner scanner;
    output << "Escaneando datasets em: " << scanner.datasetsPath() << Qt::endl;
    const QList<DatasetInfo> datasets = scanner.scan();

    if (!scanner.lastError().isEmpty()) {
        output << scanner.lastError() << Qt::endl;
    }

    printDatasets(datasets, output);
    if (datasets.isEmpty()) {
        output << "Coloque arquivos Moses/OPUS na pasta acima e tente novamente." << Qt::endl;
        return false;
    }

    output << "Escolha o índice do dataset: " << Qt::flush;

    bool validIndex = false;
    const int selectedIndex = input.readLine().trimmed().toInt(&validIndex);
    if (!validIndex || selectedIndex < 0 || selectedIndex >= datasets.size()) {
        output << "Índice inválido." << Qt::endl;
        return false;
    }

    const DatasetInfo &dataset = datasets.at(selectedIndex);
    AtlasImporter importer;

    output << "Importando dataset Moses/OPUS detectado:" << Qt::endl;
    output << dataset.corpusName << " (" << dataset.sourceLanguage << " -> " << dataset.targetLanguage << ")" << Qt::endl;
    output << "Origem: " << dataset.sourceFile << Qt::endl;
    output << "Destino: " << dataset.targetFile << Qt::endl;

    if (!importer.importMosesDataset(dataset.sourceFile,
                                     dataset.targetFile,
                                     dataset.sourceLanguage,
                                     dataset.targetLanguage)) {
        output << "Erro ao importar dataset: " << importer.lastError() << Qt::endl;
        return false;
    }

    output << "Importação concluída em: " << importer.databasePath() << Qt::endl;
    return true;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream input(stdin);
    QTextStream output(stdout);

    QString pathError;
    if (!AppPaths::ensureRequiredDirectories(&pathError)) {
        output << "Erro ao preparar diretórios do Atlas-Translator: " << pathError << Qt::endl;
        return 1;
    }

    TranslatorEngine translator;
    translator.setDebugEnabled(detailedDebug);
    if (!translator.initialize()) {
        output << "Erro ao inicializar o Atlas-Translator: " << translator.lastError() << Qt::endl;
        return 1;
    }

    output << "Atlas-Translator offline iniciado." << Qt::endl;
    output << "Datasets: " << AppPaths::datasetsPath() << Qt::endl;
    output << "Database: " << AppPaths::databaseFile() << Qt::endl;

    while (true) {
        output << Qt::endl;
        output << "1 - Traduzir texto" << Qt::endl;
        output << "2 - Importar dataset detectado" << Qt::endl;
        output << "0 - Sair" << Qt::endl;
        output << "Opção: " << Qt::flush;

        const QString option = input.readLine().trimmed();

        if (option == QStringLiteral("1")) {
            translateText(translator, input, output);
        } else if (option == QStringLiteral("2")) {
            importDetectedDataset(input, output);
            translator.initialize();
        } else if (option == QStringLiteral("0")) {
            break;
        } else {
            output << "Opção inválida." << Qt::endl;
        }
    }

    output << "Encerrando Atlas-Translator." << Qt::endl;
    return 0;
}
