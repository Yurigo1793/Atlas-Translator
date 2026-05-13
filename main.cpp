#include "core/AppPaths.h"
#include "core/AtlasImporter.h"
#include "core/DatasetScanner.h"
#include "core/TranslatorEngine.h"
#include "core/Utf8Streams.h"

#include <QCoreApplication>
#include <QList>
#include <QStringList>
#include <QTextStream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr bool defaultDetailedDebug = false;

void printAvailableLanguages(const QStringList &languages, QTextStream &out)
{
    out << "Idiomas disponíveis:" << Qt::endl;
    for (qsizetype index = 0; index < languages.size(); ++index) {
        out << "[" << index << "] " << languages.at(index) << Qt::endl;
    }
}

void printAvailablePairs(const QList<DatabaseManager::LanguagePair> &pairs, QTextStream &out)
{
    out << "Pares disponíveis:" << Qt::endl;
    if (pairs.isEmpty()) {
        out << "- none" << Qt::endl;
        return;
    }

    for (qsizetype index = 0; index < pairs.size(); ++index) {
        const DatabaseManager::LanguagePair &pair = pairs.at(index);
        out << "[" << index << "] " << pair.sourceLang << " -> " << pair.targetLang
            << " (" << pair.translationCount << ")" << Qt::endl;
    }
}

DatabaseManager::LanguagePair chooseLanguagePair(QTextStream &in,
                                                 QTextStream &out,
                                                 const QList<DatabaseManager::LanguagePair> &pairs)
{
    while (true) {
        out << "Escolha o par de idiomas (índice): " << Qt::flush;
        bool validIndex = false;
        const int selectedIndex = in.readLine().trimmed().toInt(&validIndex);
        if (validIndex && selectedIndex >= 0 && selectedIndex < pairs.size()) {
            return pairs.at(selectedIndex);
        }

        out << "Par inválido. Escolha um índice listado." << Qt::endl;
    }
}

void translateText(TranslatorEngine &translator, QTextStream &in, QTextStream &out)
{
    const QStringList languages = translator.availableLanguages();
    const QList<DatabaseManager::LanguagePair> pairs = translator.availableLanguagePairs();
    if (languages.isEmpty() || pairs.isEmpty()) {
        out << "Nenhum par de idiomas disponível no banco. Importe um dataset primeiro." << Qt::endl;
        return;
    }

    printAvailableLanguages(languages, out);
    printAvailablePairs(pairs, out);

    const DatabaseManager::LanguagePair selectedPair = chooseLanguagePair(in, out, pairs);
    const QString sourceLang = selectedPair.sourceLang;
    const QString targetLang = selectedPair.targetLang;

    if (!translator.hasLanguagePair(sourceLang, targetLang)) {
        out << "No translations available for:" << Qt::endl;
        out << sourceLang << " -> " << targetLang << Qt::endl;
        return;
    }

    out << "Usando par: " << sourceLang << " -> " << targetLang << Qt::endl;
    out << "Digite o texto para traduzir (ou 'exit' para voltar):" << Qt::endl;

    while (true) {
        out << "Digite: " << Qt::flush;
        const QString line = in.readLine();

        if (line.trimmed().compare(QStringLiteral("exit"), Qt::CaseInsensitive) == 0) {
            break;
        }

        const TranslatorEngine::TranslationResult result = translator.translateDetailed(line, sourceLang, targetLang);
        out << result.translation << Qt::endl;
        out << "Tempo: " << QString::number(static_cast<double>(result.translationTimeNs) / 1000000.0, 'f', 3)
               << " ms | Tipo: " << result.matchType << Qt::endl;
    }
}

void printDatasets(const QList<DatasetInfo> &datasets, QTextStream &out)
{
    if (datasets.isEmpty()) {
        out << "Nenhum dataset OPUS/Moses válido foi encontrado." << Qt::endl;
        return;
    }

    for (qsizetype index = 0; index < datasets.size(); ++index) {
        const DatasetInfo &dataset = datasets.at(index);
        out << "[" << index << "] " << dataset.corpusName
               << " (" << dataset.sourceLanguage << " -> " << dataset.targetLanguage << ")"
               << Qt::endl;
    }
}

bool importDetectedDataset(QTextStream &in, QTextStream &out)
{
    DatasetScanner scanner;
    out << "Escaneando datasets em: " << scanner.datasetsPath() << Qt::endl;
    const QList<DatasetInfo> datasets = scanner.scan();

    if (!scanner.lastError().isEmpty()) {
        out << scanner.lastError() << Qt::endl;
    }

    printDatasets(datasets, out);
    if (datasets.isEmpty()) {
        out << "Coloque arquivos Moses/OPUS na pasta acima e tente novamente." << Qt::endl;
        return false;
    }

    out << "Escolha o índice do dataset: " << Qt::flush;

    bool validIndex = false;
    const int selectedIndex = in.readLine().trimmed().toInt(&validIndex);
    if (!validIndex || selectedIndex < 0 || selectedIndex >= datasets.size()) {
        out << "Índice inválido." << Qt::endl;
        return false;
    }

    const DatasetInfo &dataset = datasets.at(selectedIndex);
    AtlasImporter importer;

    out << "Importando dataset Moses/OPUS detectado:" << Qt::endl;
    out << dataset.corpusName << " (" << dataset.sourceLanguage << " -> " << dataset.targetLanguage << ")" << Qt::endl;
    out << "Origem: " << dataset.sourceFile << Qt::endl;
    out << "Destino: " << dataset.targetFile << Qt::endl;

    if (!importer.importMosesDataset(dataset.sourceFile,
                                     dataset.targetFile,
                                     dataset.sourceLanguage,
                                     dataset.targetLanguage)) {
        out << "Erro ao importar dataset: " << importer.lastError() << Qt::endl;
        return false;
    }

    out << "Importação concluída em: " << importer.databasePath() << Qt::endl;
    return true;
}
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    QCoreApplication app(argc, argv);
    QTextStream in(stdin);
    QTextStream out(stdout);
    configureUtf8Stream(in);
    configureUtf8Stream(out);

    QString pathError;
    if (!AppPaths::ensureRequiredDirectories(&pathError)) {
        out << "Erro ao preparar diretórios do Atlas-Translator: " << pathError << Qt::endl;
        return 1;
    }

    const bool detailedDebug = app.arguments().contains(QStringLiteral("--debug")) ? true : defaultDetailedDebug;

    TranslatorEngine translator;
    translator.setDebugEnabled(detailedDebug);
    if (!translator.initialize()) {
        out << "Erro ao inicializar o Atlas-Translator: " << translator.lastError() << Qt::endl;
        return 1;
    }

    out << "Atlas-Translator offline iniciado." << Qt::endl;
    out << "Datasets: " << AppPaths::datasetsPath() << Qt::endl;
    translator.printDatabaseSummary(out);

    while (true) {
        out << Qt::endl;
        out << "1 - Traduzir texto" << Qt::endl;
        out << "2 - Importar dataset detectado" << Qt::endl;
        out << "0 - Sair" << Qt::endl;
        out << "Opção: " << Qt::flush;

        const QString option = in.readLine().trimmed();

        if (option == QStringLiteral("1")) {
            translateText(translator, in, out);
        } else if (option == QStringLiteral("2")) {
            importDetectedDataset(in, out);
            if (!translator.initialize()) {
                out << "Erro ao recarregar banco após importação: " << translator.lastError() << Qt::endl;
            } else {
                translator.printDatabaseSummary(out);
            }
        } else if (option == QStringLiteral("0")) {
            break;
        } else {
            out << "Opção inválida." << Qt::endl;
        }
    }

    out << "Encerrando Atlas-Translator." << Qt::endl;
    return 0;
}
