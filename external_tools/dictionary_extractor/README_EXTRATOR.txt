ATLAS - EXTRATOR DO DICIONARIO PDF

Comando:
EXTRAIR_DICIONARIO_PDF.cmd

Entrada:
C:\Atlas-Translator\external_tools\dicionario-pt.pdf

Saida:
output\dicionario_pt_extraido.tsv
output\dicionario_pt_extraido_marcas_abreviacoes.tsv

Formato do TSV:
lema<TAB>classe<TAB>definicao

Exemplo:
a-cantaros	loc. adv.	Copiosamente, com abundancia.

Formato do TSV de marcas:
lema<TAB>classe e abreviacoes encontradas

Exemplo:
cama	f. | Prov. dur. | Bras. | med. lat. | Cp. | gr.

Observacoes:
- O PDF tem duas colunas por pagina.
- A ferramenta le a coluna esquerda de cima para baixo e depois a direita.
- Uma nova entrada comeca quando aparece novo lema em negrito.
- Linhas seguintes sao juntadas na definicao da entrada atual.
- O arquivo TSV nao tem cabecalho: cada linha e uma entrada do dicionario.
- O arquivo de marcas remove a definicao longa e preserva apenas as abreviacoes/marcas detectadas.
- Quando duas abreviacoes ocupam o mesmo trecho, a mais longa vence: Prov. dur. nao vira Prov. + dur.
- Tambem e gerado um relatorio ao lado do TSV.
