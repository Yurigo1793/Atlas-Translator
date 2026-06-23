#include "core/AppPaths.h"
#include "core/AtlasReport.h"
#include "core/AtlasImporter.h"
#include "core/DatasetScanner.h"
#include "core/LanguageNormalizer.h"
#include "core/TranslatorEngine.h"
#include "core/Utf8Streams.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QList>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <QUuid>
#include <algorithm>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr bool defaultDetailedDebug = false;

QString bidirectionalPairText(const QString &sourceLanguage, const QString &targetLanguage)
{
    return QStringLiteral("%1 -> %2 e %2 -> %1").arg(sourceLanguage, targetLanguage);
}

QString unorderedPairKey(const QString &sourceLanguage, const QString &targetLanguage)
{
    QString first = sourceLanguage;
    QString second = targetLanguage;
    if (second < first) {
        std::swap(first, second);
    }
    return QStringLiteral("%1\n%2").arg(first, second);
}

bool isSupportedDatasetPair(const DatasetInfo &dataset)
{
    if (dataset.sourceType == DatasetInfo::SourceType::MediaWikiXml) {
        return true;
    }

    const LanguageNormalizer normalizer;
    return normalizer.isSupported(dataset.sourceLanguage)
        && normalizer.isSupported(dataset.targetLanguage);
}

QString safeOutputFileToken(QString text)
{
    text.replace(QRegularExpression(QStringLiteral(R"([^A-Za-z0-9._-]+)")), QStringLiteral("_"));
    text.replace(QRegularExpression(QStringLiteral(R"(_+)")), QStringLiteral("_"));
    while (text.startsWith(QLatin1Char('_'))) {
        text.remove(0, 1);
    }
    while (text.endsWith(QLatin1Char('_'))) {
        text.chop(1);
    }
    return text.isEmpty() ? QStringLiteral("dataset") : text;
}

QString readInputLine(QTextStream &in)
{
#ifdef _WIN32
    const HANDLE inputHandle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD consoleMode = 0;
    if (inputHandle != INVALID_HANDLE_VALUE && GetConsoleMode(inputHandle, &consoleMode)) {
        QString line;
        wchar_t buffer[256];
        DWORD charsRead = 0;

        while (ReadConsoleW(inputHandle, buffer, 255, &charsRead, nullptr) && charsRead > 0) {
            line.append(QString::fromWCharArray(buffer, static_cast<int>(charsRead)));
            if (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r'))) {
                break;
            }
        }

        while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        return line;
    }
#endif

    return in.readLine();
}

QString datasetConsolidationKey(const DatasetInfo &dataset)
{
    const QString pairKey = unorderedPairKey(dataset.sourceLanguage, dataset.targetLanguage);
    if (dataset.sourceType == DatasetInfo::SourceType::FreeDictTei) {
        return QStringLiteral("freedict\n%1").arg(pairKey);
    }

    return QStringLiteral("%1\n%2\n%3")
        .arg(static_cast<int>(dataset.sourceType))
        .arg(dataset.corpusName, pairKey);
}

void appendGroupedDataset(DatasetInfo &target, const DatasetInfo &source)
{
    if (target.groupedSourceFiles.isEmpty()) {
        target.groupedSourceFiles.append(target.sourceFile);
        target.groupedTargetFiles.append(target.targetFile);
        target.groupedSourceLanguages.append(target.sourceLanguage);
        target.groupedTargetLanguages.append(target.targetLanguage);
        target.groupedCorpusNames.append(target.corpusName);
    }

    if (source.sourceFile.isEmpty() || target.groupedSourceFiles.contains(source.sourceFile)) {
        return;
    }

    target.groupedSourceFiles.append(source.sourceFile);
    target.groupedTargetFiles.append(source.targetFile);
    target.groupedSourceLanguages.append(source.sourceLanguage);
    target.groupedTargetLanguages.append(source.targetLanguage);
    target.groupedCorpusNames.append(source.corpusName);
    if (!target.corpusName.contains(source.corpusName)) {
        target.corpusName = QStringLiteral("%1 + %2").arg(target.corpusName, source.corpusName);
    }
}

QList<DatasetInfo> consolidatedDatasets(const QList<DatasetInfo> &datasets)
{
    QList<DatasetInfo> consolidated;
    QHash<QString, qsizetype> indexesByKey;

    for (const DatasetInfo &dataset : datasets) {
        const QString key = datasetConsolidationKey(dataset);
        if (indexesByKey.contains(key)) {
            appendGroupedDataset(consolidated[indexesByKey.value(key)], dataset);
            continue;
        }

        DatasetInfo grouped = dataset;
        appendGroupedDataset(grouped, dataset);
        indexesByKey.insert(key, consolidated.size());
        consolidated.append(grouped);
    }

    return consolidated;
}

QList<DatasetInfo> importableDatasets(const QList<DatasetInfo> &datasets)
{
    QList<DatasetInfo> filtered;
    for (const DatasetInfo &dataset : datasets) {
        if (dataset.sourceType == DatasetInfo::SourceType::MediaWikiXml) {
            continue;
        }
        if (dataset.sourceType == DatasetInfo::SourceType::ParallelText) {
            continue;
        }
        if (!isSupportedDatasetPair(dataset)) {
            continue;
        }
        filtered.append(dataset);
    }
    return filtered;
}

qint64 translationCountInDatabase(const QString &databasePath)
{
    const QString connectionName = QStringLiteral("atlas_probe_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(databasePath);
    if (!database.open()) {
        QSqlDatabase::removeDatabase(connectionName);
        return -1;
    }

    qint64 count = -1;
    QSqlQuery tableQuery(database);
    if (tableQuery.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'translations' LIMIT 1"))
        && tableQuery.next()) {
        const LanguageNormalizer normalizer;
        QSqlQuery countQuery(database);
        if (countQuery.exec(QStringLiteral(R"(
            SELECT source_lang, target_lang, COUNT(*)
            FROM translations
            GROUP BY source_lang, target_lang
        )"))) {
            count = 0;
            while (countQuery.next()) {
                if (normalizer.isSupported(countQuery.value(0).toString())
                    && normalizer.isSupported(countQuery.value(1).toString())) {
                    count += countQuery.value(2).toLongLong();
                }
            }
        }
        countQuery.finish();
    }
    tableQuery.finish();
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
    return count;
}

QString activeDatabasePath(const QString &preferredDatabasePath = QString())
{
    if (!preferredDatabasePath.isEmpty() && translationCountInDatabase(preferredDatabasePath) > 0) {
        return preferredDatabasePath;
    }

    const QDir databaseDirectory(AppPaths::databasePath());
    const QFileInfoList pairDatabases = databaseDirectory.entryInfoList(QStringList{QStringLiteral("Atlas_*.db")},
                                                                        QDir::Files | QDir::Readable,
                                                                        QDir::Time);
    for (const QFileInfo &databaseInfo : pairDatabases) {
        const QString candidatePath = databaseInfo.absoluteFilePath();
        if (translationCountInDatabase(candidatePath) > 0) {
            return candidatePath;
        }
    }

    return QString();
}

void printAvailableLanguages(const QStringList &languages, QTextStream &out)
{
    out << "Idiomas disponiveis:" << Qt::endl;
    for (qsizetype index = 0; index < languages.size(); ++index) {
        out << "[" << index << "] " << languages.at(index) << Qt::endl;
    }
}

void printAvailablePairs(const QList<DatabaseManager::LanguagePair> &pairs, QTextStream &out)
{
    out << "Pares disponiveis:" << Qt::endl;
    if (pairs.isEmpty()) {
        out << "- nenhum" << Qt::endl;
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
        out << "Escolha o indice do par de idiomas: " << Qt::flush;
        bool validIndex = false;
        const int selectedIndex = readInputLine(in).trimmed().toInt(&validIndex);
        if (validIndex && selectedIndex >= 0 && selectedIndex < pairs.size()) {
            return pairs.at(selectedIndex);
        }

        out << "Par invalido. Escolha um dos indices listados." << Qt::endl;
    }
}

void translateText(TranslatorEngine &translator, QTextStream &in, QTextStream &out)
{
    const QStringList languages = translator.availableLanguages();
    const QList<DatabaseManager::LanguagePair> pairs = translator.availableLanguagePairs();
    if (languages.isEmpty() || pairs.isEmpty()) {
        out << "Nenhum par de idiomas disponivel no banco. Importe um dataset primeiro." << Qt::endl;
        return;
    }

    printAvailableLanguages(languages, out);
    printAvailablePairs(pairs, out);

    const DatabaseManager::LanguagePair selectedPair = chooseLanguagePair(in, out, pairs);
    const QString sourceLang = selectedPair.sourceLang;
    const QString targetLang = selectedPair.targetLang;

    if (!translator.hasLanguagePair(sourceLang, targetLang)) {
        out << "Nenhuma traducao disponivel para:" << Qt::endl;
        out << sourceLang << " -> " << targetLang << Qt::endl;
        return;
    }

    out << "Usando par: " << sourceLang << " -> " << targetLang << Qt::endl;
    out << "Digite o texto para traduzir (ou 'exit' para voltar):" << Qt::endl;

    while (true) {
        out << "Entrada: " << Qt::flush;
        const QString line = readInputLine(in);

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
        out << "Nenhum dataset OPUS/Moses valido foi encontrado." << Qt::endl;
        return;
    }

    for (qsizetype index = 0; index < datasets.size(); ++index) {
        const DatasetInfo &dataset = datasets.at(index);
        QString sourceType;
        switch (dataset.sourceType) {
        case DatasetInfo::SourceType::ParallelText:
            sourceType = QStringLiteral("OPUS/Moses");
            break;
        case DatasetInfo::SourceType::OpusPreprocessed:
            sourceType = QStringLiteral("OPUS/Moses pre-processado");
            break;
        case DatasetInfo::SourceType::FreeDictTei:
            sourceType = QStringLiteral("FreeDict TEI");
            break;
        case DatasetInfo::SourceType::MediaWikiXml:
            sourceType = QStringLiteral("MediaWiki");
            break;
        case DatasetInfo::SourceType::MediaWikiPreprocessed:
            sourceType = QStringLiteral("MediaWiki pre-processado");
            break;
        }

        out << "[" << index << "] " << sourceType << " - " << dataset.corpusName
               << " (" << bidirectionalPairText(dataset.sourceLanguage, dataset.targetLanguage) << ")"
               << Qt::endl;
    }
}

QString importDatasetBidirectional(const DatasetInfo &dataset, QTextStream &out, bool debugRejectedDetails)
{
    if (!isSupportedDatasetPair(dataset)) {
        out << "Dataset ignorado porque o par de idiomas esta fora da lista suportada: "
            << dataset.sourceLanguage << " -> " << dataset.targetLanguage << Qt::endl;
        return QString();
    }

    const QString databasePath = AtlasImporter::databasePathForLanguagePair(dataset.sourceLanguage, dataset.targetLanguage);

    out << "Importando fonte detectada:" << Qt::endl;
    out << dataset.corpusName << " (" << bidirectionalPairText(dataset.sourceLanguage, dataset.targetLanguage) << ")" << Qt::endl;
    out << "Banco: " << databasePath << Qt::endl;

    const QStringList sourceFiles = dataset.groupedSourceFiles.isEmpty()
                                        ? QStringList{dataset.sourceFile}
                                        : dataset.groupedSourceFiles;
    const QStringList targetFiles = dataset.groupedTargetFiles.isEmpty()
                                        ? QStringList{dataset.targetFile}
                                        : dataset.groupedTargetFiles;
    const QStringList sourceLanguages = dataset.groupedSourceLanguages.isEmpty()
                                            ? QStringList{dataset.sourceLanguage}
                                            : dataset.groupedSourceLanguages;
    const QStringList targetLanguages = dataset.groupedTargetLanguages.isEmpty()
                                            ? QStringList{dataset.targetLanguage}
                                            : dataset.groupedTargetLanguages;
    const QStringList corpusNames = dataset.groupedCorpusNames.isEmpty()
                                        ? QStringList{dataset.corpusName}
                                        : dataset.groupedCorpusNames;

    qsizetype importedCount = 0;
    for (qsizetype index = 0; index < sourceFiles.size(); ++index) {
        const QString sourceFile = sourceFiles.value(index);
        const QString targetFile = targetFiles.value(index);
        const QString sourceLanguage = sourceLanguages.value(index, dataset.sourceLanguage);
        const QString targetLanguage = targetLanguages.value(index, dataset.targetLanguage);
        const QString corpusName = corpusNames.value(index, dataset.corpusName);

        AtlasImporter importer(databasePath);
        importer.setDebugRejectedDetailsEnabled(debugRejectedDetails);

        out << "Item de origem " << (index + 1) << "/" << sourceFiles.size()
            << ": " << corpusName << Qt::endl;
        out << "Origem: " << sourceFile << Qt::endl;
        if (!targetFile.isEmpty()) {
            out << "Destino: " << targetFile << Qt::endl;
        }

        bool success = false;
        if (dataset.sourceType == DatasetInfo::SourceType::ParallelText) {
            success = importer.importMosesDatasetBidirectional(sourceFile,
                                                               targetFile,
                                                               sourceLanguage,
                                                               targetLanguage);
        } else if (dataset.sourceType == DatasetInfo::SourceType::OpusPreprocessed) {
            success = importer.importMosesPreprocessedDataset(sourceFile,
                                                             sourceLanguage,
                                                             targetLanguage);
        } else if (dataset.sourceType == DatasetInfo::SourceType::FreeDictTei) {
            success = importer.importFreeDictTeiDataset(sourceFile,
                                                        sourceLanguage,
                                                        targetLanguage);
        } else if (dataset.sourceType == DatasetInfo::SourceType::MediaWikiXml) {
            success = importer.importMediaWikiDataset(sourceFile,
                                                      sourceLanguage,
                                                      targetLanguage);
        } else if (dataset.sourceType == DatasetInfo::SourceType::MediaWikiPreprocessed) {
            success = importer.importMediaWikiPreprocessedDataset(sourceFile,
                                                                  sourceLanguage,
                                                                  targetLanguage);
        }

        if (!success) {
            out << "Erro ao importar fonte: " << importer.lastError() << Qt::endl;
            return QString();
        }
        ++importedCount;
    }

    out << "Importacao bidirecional concluida. Itens importados: " << importedCount << Qt::endl;
    out << "Banco: " << databasePath << Qt::endl;
    return databasePath;
}

QString importDetectedDataset(QTextStream &in, QTextStream &out, bool debugRejectedDetails)
{
    DatasetScanner scanner;
    out << "Verificando datasets em: " << scanner.datasetsPath() << Qt::endl;
    QElapsedTimer scannerTimer;
    scannerTimer.start();
    const QList<DatasetInfo> datasets = importableDatasets(consolidatedDatasets(scanner.scan()));
    AtlasReport::appendFlux(QStringLiteral("INFO"),
                            QStringLiteral("Verificacao de datasets concluida"),
                            QStringLiteral("Tempo: %1\nItens detectados: %2\nDiretorio: %3")
                                .arg(AtlasReport::formatMilliseconds(scannerTimer.nsecsElapsed()))
                                .arg(datasets.size())
                                .arg(scanner.datasetsPath()));

    if (!scanner.lastError().isEmpty()) {
        out << scanner.lastError() << Qt::endl;
    }

    printDatasets(datasets, out);
    if (datasets.isEmpty()) {
        out << "Adicione arquivos FreeDict/OPUS ou gere arquivos MediaWiki pre-processados com a opcao 4 e tente de novo." << Qt::endl;
        return QString();
    }

    out << "Escolha o indice do dataset: " << Qt::flush;

    bool validIndex = false;
    const int selectedIndex = readInputLine(in).trimmed().toInt(&validIndex);
    if (!validIndex || selectedIndex < 0 || selectedIndex >= datasets.size()) {
        out << "Indice invalido." << Qt::endl;
        return QString();
    }

    const DatasetInfo &dataset = datasets.at(selectedIndex);
    return importDatasetBidirectional(dataset, out, debugRejectedDetails);
}

QString importAllDetectedDatasets(QTextStream &out, bool debugRejectedDetails)
{
    DatasetScanner scanner;
    out << "Verificando datasets em: " << scanner.datasetsPath() << Qt::endl;
    QElapsedTimer scannerTimer;
    scannerTimer.start();
    const QList<DatasetInfo> datasets = importableDatasets(consolidatedDatasets(scanner.scan()));
    AtlasReport::appendFlux(QStringLiteral("INFO"),
                            QStringLiteral("Verificacao de datasets concluida"),
                            QStringLiteral("Tempo: %1\nItens detectados: %2\nDiretorio: %3")
                                .arg(AtlasReport::formatMilliseconds(scannerTimer.nsecsElapsed()))
                                .arg(datasets.size())
                                .arg(scanner.datasetsPath()));

    if (!scanner.lastError().isEmpty()) {
        out << scanner.lastError() << Qt::endl;
    }

    printDatasets(datasets, out);
    if (datasets.isEmpty()) {
        out << "Adicione arquivos FreeDict/OPUS ou gere arquivos MediaWiki pre-processados com a opcao 4 e tente de novo." << Qt::endl;
        return QString();
    }

    qsizetype importedCount = 0;
    qsizetype failedCount = 0;
    QString lastImportedDatabasePath;
    for (const DatasetInfo &dataset : datasets) {
        const QString importedDatabasePath = importDatasetBidirectional(dataset, out, debugRejectedDetails);
        if (!importedDatabasePath.isEmpty()) {
            ++importedCount;
            lastImportedDatabasePath = importedDatabasePath;
        } else {
            ++failedCount;
        }
    }

    out << "Importacao em lote concluida. Sucessos: " << importedCount
        << " | Falhas: " << failedCount << Qt::endl;
    return lastImportedDatabasePath;
}

void preprocessAllMediaWikiDatasets(QTextStream &out)
{
    DatasetScanner scanner;
    out << "[MediaWiki] verificando datasets" << Qt::endl;
    const QList<DatasetInfo> datasets = consolidatedDatasets(scanner.scan());
    if (!scanner.lastError().isEmpty()) {
        out << scanner.lastError() << Qt::endl;
    }

    const QString outputDirectory = QDir(scanner.datasetsPath()).filePath(QStringLiteral("MediaWiki_Preprocessed"));
    if (!QDir().mkpath(outputDirectory)) {
        out << "Erro ao criar diretorio de saida pre-processada: " << outputDirectory << Qt::endl;
        return;
    }

    QSet<QString> processedKeys;
    qsizetype successCount = 0;
    qsizetype failureCount = 0;
    for (const DatasetInfo &dataset : datasets) {
        if (dataset.sourceType != DatasetInfo::SourceType::MediaWikiXml) {
            continue;
        }

        const QString key = QFileInfo(dataset.sourceFile).absoluteFilePath();
        if (processedKeys.contains(key)) {
            continue;
        }
        processedKeys.insert(key);

        AtlasImporter importer;
        if (importer.preprocessMediaWikiDatasetAllLanguages(dataset.sourceFile,
                                                            dataset.sourceLanguage,
                                                            outputDirectory,
                                                            &out)) {
            ++successCount;
        } else {
            ++failureCount;
            out << "Erro: " << importer.lastError() << Qt::endl;
        }
    }

    out << "[MediaWiki] concluido Sucessos=" << successCount << " Falhas=" << failureCount << Qt::endl;
}

void preprocessAllMosesDatasets(QTextStream &out)
{
    DatasetScanner scanner;
    out << "[OPUS] verificando datasets" << Qt::endl;
    const QList<DatasetInfo> datasets = consolidatedDatasets(scanner.scan());
    if (!scanner.lastError().isEmpty()) {
        out << scanner.lastError() << Qt::endl;
    }

    const QString outputDirectory = QDir(scanner.datasetsPath()).filePath(QStringLiteral("OPUS_Preprocessed"));
    if (!QDir().mkpath(outputDirectory)) {
        out << "Erro ao criar diretorio de saida OPUS pre-processada: " << outputDirectory << Qt::endl;
        return;
    }

    QSet<QString> processedKeys;
    qsizetype successCount = 0;
    qsizetype failureCount = 0;
    for (const DatasetInfo &dataset : datasets) {
        if (dataset.sourceType != DatasetInfo::SourceType::ParallelText) {
            continue;
        }
        if (!isSupportedDatasetPair(dataset)) {
            continue;
        }

        const QStringList sourceFiles = dataset.groupedSourceFiles.isEmpty()
                                            ? QStringList{dataset.sourceFile}
                                            : dataset.groupedSourceFiles;
        const QStringList targetFiles = dataset.groupedTargetFiles.isEmpty()
                                            ? QStringList{dataset.targetFile}
                                            : dataset.groupedTargetFiles;
        const QStringList sourceLanguages = dataset.groupedSourceLanguages.isEmpty()
                                                ? QStringList{dataset.sourceLanguage}
                                                : dataset.groupedSourceLanguages;
        const QStringList targetLanguages = dataset.groupedTargetLanguages.isEmpty()
                                                ? QStringList{dataset.targetLanguage}
                                                : dataset.groupedTargetLanguages;
        const QStringList corpusNames = dataset.groupedCorpusNames.isEmpty()
                                            ? QStringList{dataset.corpusName}
                                            : dataset.groupedCorpusNames;

        for (qsizetype index = 0; index < sourceFiles.size(); ++index) {
            const QString sourceFile = sourceFiles.value(index);
            const QString targetFile = targetFiles.value(index);
            const QString sourceLanguage = sourceLanguages.value(index, dataset.sourceLanguage);
            const QString targetLanguage = targetLanguages.value(index, dataset.targetLanguage);
            const QString corpusName = corpusNames.value(index, dataset.corpusName);
            const QString key = QStringLiteral("%1\n%2\n%3\n%4")
                                    .arg(QFileInfo(sourceFile).absoluteFilePath(),
                                         QFileInfo(targetFile).absoluteFilePath(),
                                         sourceLanguage,
                                         targetLanguage);
            if (processedKeys.contains(key)) {
                continue;
            }
            processedKeys.insert(key);

            const QString outputFilePath = QDir(outputDirectory).filePath(
                QStringLiteral("%1_%2-%3_preprocessed.tsv")
                    .arg(safeOutputFileToken(corpusName),
                         safeOutputFileToken(sourceLanguage),
                         safeOutputFileToken(targetLanguage)));

            AtlasImporter importer;
            if (importer.preprocessMosesDataset(sourceFile,
                                                targetFile,
                                                sourceLanguage,
                                                targetLanguage,
                                                corpusName,
                                                outputFilePath,
                                                &out)) {
                ++successCount;
            } else {
                ++failureCount;
                out << "Erro: " << importer.lastError() << Qt::endl;
            }
        }
    }

    out << "[OPUS] concluido Sucessos=" << successCount << " Falhas=" << failureCount << Qt::endl;
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
        out << "Erro ao preparar diretorios do Atlas-Translator: " << pathError << Qt::endl;
        return 1;
    }

    const bool detailedDebug = app.arguments().contains(QStringLiteral("--debug")) ? true : defaultDetailedDebug;

    AtlasReport::appendFlux(QStringLiteral("INFO"), QStringLiteral("Inicializacao do Atlas-Translator"));

    QString currentDatabasePath = activeDatabasePath();
    QElapsedTimer databaseLoadTimer;
    databaseLoadTimer.start();
    auto translator = std::make_unique<TranslatorEngine>(currentDatabasePath);
    translator->setDebugEnabled(detailedDebug);
    if (!translator->initialize()) {
        AtlasReport::appendFlux(QStringLiteral("ERROR"),
                                QStringLiteral("Falha ao inicializar motor"),
                                translator->lastError());
        out << "Erro ao inicializar Atlas-Translator: " << translator->lastError() << Qt::endl;
        return 1;
    }
    AtlasReport::appendFlux(QStringLiteral("INFO"),
                            QStringLiteral("Banco carregado"),
                            QStringLiteral("Arquivo: %1\nTempo: %2")
                                .arg(currentDatabasePath,
                                     AtlasReport::formatMilliseconds(databaseLoadTimer.nsecsElapsed())));

    out << "Atlas-Translator offline iniciado." << Qt::endl;
    out << "Pasta de datasets: " << AppPaths::datasetsPath() << Qt::endl;
    translator->printDatabaseSummary(out);

    while (true) {
        out << Qt::endl;
        out << "1 - Traduzir texto" << Qt::endl;
        out << "2 - Importar dataset detectado" << Qt::endl;
        out << "3 - Importar todos os datasets detectados" << Qt::endl;
        out << "4 - Pre-processar todos os arquivos MediaWiki compativeis" << Qt::endl;
        out << "5 - Pre-processar todos os arquivos OPUS/Moses compativeis" << Qt::endl;
        out << "0 - Sair" << Qt::endl;
        out << "Opcao: " << Qt::flush;

        const QString option = readInputLine(in).trimmed();

        if (option == QStringLiteral("1")) {
            translateText(*translator, in, out);
        } else if (option == QStringLiteral("2")) {
            AtlasReport::appendFlux(QStringLiteral("INFO"), QStringLiteral("Importacao individual iniciada"));
            translator->shutdown();
            const QString importedDatabasePath = importDetectedDataset(in, out, detailedDebug);
            currentDatabasePath = activeDatabasePath(importedDatabasePath);
            databaseLoadTimer.restart();
            translator = std::make_unique<TranslatorEngine>(currentDatabasePath);
            translator->setDebugEnabled(detailedDebug);
            if (!translator->initialize()) {
                AtlasReport::appendFlux(QStringLiteral("ERROR"),
                                        QStringLiteral("Erro ao recarregar banco apos importacao"),
                                        translator->lastError());
                out << "Erro ao recarregar banco apos importacao: " << translator->lastError() << Qt::endl;
            } else {
                AtlasReport::appendFlux(QStringLiteral("INFO"),
                                        QStringLiteral("Banco recarregado apos importacao"),
                                        QStringLiteral("Arquivo: %1\nTempo: %2")
                                            .arg(currentDatabasePath,
                                                 AtlasReport::formatMilliseconds(databaseLoadTimer.nsecsElapsed())));
                translator->printDatabaseSummary(out);
            }
        } else if (option == QStringLiteral("3")) {
            AtlasReport::appendFlux(QStringLiteral("INFO"), QStringLiteral("Importacao em lote iniciada"));
            translator->shutdown();
            const QString importedDatabasePath = importAllDetectedDatasets(out, detailedDebug);
            currentDatabasePath = activeDatabasePath(importedDatabasePath);
            databaseLoadTimer.restart();
            translator = std::make_unique<TranslatorEngine>(currentDatabasePath);
            translator->setDebugEnabled(detailedDebug);
            if (!translator->initialize()) {
                AtlasReport::appendFlux(QStringLiteral("ERROR"),
                                        QStringLiteral("Erro ao recarregar banco apos importacao em lote"),
                                        translator->lastError());
                out << "Erro ao recarregar banco apos importacao: " << translator->lastError() << Qt::endl;
            } else {
                AtlasReport::appendFlux(QStringLiteral("INFO"),
                                        QStringLiteral("Banco recarregado apos importacao em lote"),
                                        QStringLiteral("Arquivo: %1\nTempo: %2")
                                            .arg(currentDatabasePath,
                                                 AtlasReport::formatMilliseconds(databaseLoadTimer.nsecsElapsed())));
                translator->printDatabaseSummary(out);
            }
        } else if (option == QStringLiteral("4")) {
            AtlasReport::appendFlux(QStringLiteral("INFO"), QStringLiteral("Pre-processamento MediaWiki iniciado"));
            preprocessAllMediaWikiDatasets(out);
        } else if (option == QStringLiteral("5")) {
            AtlasReport::appendFlux(QStringLiteral("INFO"), QStringLiteral("Pre-processamento OPUS/Moses iniciado"));
            preprocessAllMosesDatasets(out);
        } else if (option == QStringLiteral("0")) {
            break;
        } else {
            out << "Opcao invalida." << Qt::endl;
        }
    }

    out << "Fechando Atlas-Translator." << Qt::endl;
    AtlasReport::appendFlux(QStringLiteral("INFO"), QStringLiteral("Atlas-Translator encerrado"));
    return 0;
}
