#include "core/AtlasImporter.h"
#include "core/DatasetScanner.h"
#include "core/TranslatorEngine.h"

#include <QCoreApplication>
#include <QList>
#include <QTextStream>

namespace {
void translateText(TranslatorEngine &translator, QTextStream &input, QTextStream &output)
{
    output << "Digite o texto para traduzir (ou 'exit' para voltar):" << Qt::endl;

    while (true) {
        output << "Digite: " << Qt::flush;
        const QString line = input.readLine().trimmed();

        if (line.compare(QStringLiteral("exit"), Qt::CaseInsensitive) == 0) {
            break;
        }

        output << translator.translate(line) << Qt::endl;
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

QList<DatasetInfo> scanDatasets(DatasetScanner &scanner, QTextStream &output)
{
    output << "Escaneando datasets em: " << scanner.datasetsPath() << Qt::endl;
    QList<DatasetInfo> datasets = scanner.scan();

    if (!scanner.lastError().isEmpty()) {
        output << scanner.lastError() << Qt::endl;
    }

    printDatasets(datasets, output);
    return datasets;
}

bool importDetectedDataset(const QList<DatasetInfo> &datasets, QTextStream &input, QTextStream &output)
{
    if (datasets.isEmpty()) {
        output << "Nenhum dataset detectado para importar. Use a opção 2 primeiro." << Qt::endl;
        return false;
    }

    printDatasets(datasets, output);
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

    TranslatorEngine translator;
    if (!translator.initialize()) {
        output << "Erro ao inicializar o Atlas-Translator: " << translator.lastError() << Qt::endl;
        return 1;
    }

    DatasetScanner scanner;
    QList<DatasetInfo> detectedDatasets;

    output << "Atlas-Translator offline iniciado." << Qt::endl;

    while (true) {
        output << Qt::endl;
        output << "1 - Traduzir texto" << Qt::endl;
        output << "2 - Escanear datasets" << Qt::endl;
        output << "3 - Importar dataset detectado" << Qt::endl;
        output << "0 - Sair" << Qt::endl;
        output << "Escolha: " << Qt::flush;

        const QString option = input.readLine().trimmed();

        if (option == QStringLiteral("0") || option.compare(QStringLiteral("exit"), Qt::CaseInsensitive) == 0) {
            break;
        }

        if (option == QStringLiteral("1")) {
            translateText(translator, input, output);
        } else if (option == QStringLiteral("2")) {
            detectedDatasets = scanDatasets(scanner, output);
        } else if (option == QStringLiteral("3")) {
            if (detectedDatasets.isEmpty()) {
                detectedDatasets = scanDatasets(scanner, output);
            }
            importDetectedDataset(detectedDatasets, input, output);
        } else {
            output << "Opção inválida." << Qt::endl;
        }
    }

    return 0;
}
