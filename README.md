# Atlas-Translator

Atlas-Translator e uma engine offline de traducao baseada em tabela de frases.
Ela nao e um tradutor neural, nao usa IA generativa e nao cria traducoes novas do zero.
O resultado depende diretamente dos pares de texto importados para o banco SQLite.

## O que a engine faz

- Importa datasets paralelos no formato Moses/OPUS a partir da pasta `datasets`.
- Ao importar um par, grava os dois sentidos no banco, por exemplo `en -> pt_BR` e `pt_BR -> en`.
- Salva os pares de traducao em `database/atlas.db`.
- Normaliza texto e codigos de idioma antes da busca.
- Procura a melhor correspondencia no banco usando frases de ate 12 palavras.
- Prioriza a maior frase encontrada e usa frequencia para desempatar traducoes repetidas.
- Usa cache em memoria para reduzir consultas repetidas ao SQLite.
- Preserva quebras de linha, pontuacao simples, aspas/parenteses e capitalizacao basica.
- Mantem palavras sem correspondencia no texto final em vez de inventar uma traducao.

## O que ela nao faz

- Nao entende contexto amplo como um modelo neural.
- Nao reordena frases complexas.
- Nao traduz termos que nao existem no dataset importado.
- Nao garante fluencia quando a frase completa nao esta no banco.
- Nao baixa datasets automaticamente.

## Formato dos datasets

Os arquivos devem ficar em `datasets` ou em uma pasta apontada pela variavel `ATLAS_DATASETS_PATH`.
O padrao esperado e o usado por corpora Moses/OPUS:

```text
NOME.en-pt_BR.en
NOME.en-pt_BR.pt_BR
```

Exemplo ja incluido:

```text
datasets/KDE4.en-pt_BR.en
datasets/KDE4.en-pt_BR.pt_BR
```

Cada linha do arquivo de origem precisa corresponder a linha equivalente no arquivo de destino.

## Como usar

Compile com CMake e Qt 6:

```sh
cmake -S . -B build
cmake --build build
```

Execute o binario:

```sh
./build/AtlasTranslator
```

No menu:

```text
1 - Traduzir texto
2 - Importar dataset detectado
0 - Sair
```

Use a opcao `2` primeiro para importar um dataset. A importacao cadastra automaticamente o sentido direto e o sentido inverso. Depois use a opcao `1` para traduzir usando os pares de idioma disponiveis.

## Resumo tecnico

O nucleo esta em `core/TranslatorEngine.*`. A engine divide o texto em linhas e sentencas, normaliza as palavras, tenta encontrar a maior frase cadastrada no SQLite e monta a saida com as traducoes encontradas. Quando nao encontra uma frase ou palavra, ela conserva o termo original.

O banco e gerenciado por `core/DatabaseManager.*`, a importacao por `core/AtlasImporter.*` e a deteccao de datasets por `core/DatasetScanner.*`.
