ATLAS - GOOGLE TRADUTOR OFICIAL

Objetivo:
Consultar palavras selecionadas no site oficial do Google Tradutor e gerar um TSV importavel para o Atlas.

Entrada padrao:
Qualquer arquivo .tsv dentro da pasta input.
Deixe apenas um .tsv por vez nessa pasta.

Formato aceito:
lema<TAB>marcas_abreviacoes

Exemplo:
cama	f. | Prov. dur. | Bras. | med. lat. | Cp. | gr.

Regra:
A ferramenta entende somente esse modelo novo.
A primeira coluna e enviada ao Google.
As marcas do dicionario sao preservadas no TSV final.
Entrada tecnico-cientifica pura entra como termo_tecnico_cientifico direto, na mesma ordem do arquivo, sem criar saida separada.
Entrada com marca comum e marca tecnico-cientifica passa pelo Google, mas continua com motivo_tecnico preenchido.
Se o Google devolver traducao igual ao lema original, a linha e salva mesmo assim com status_traducao=sem_traducao.

Como executar:
GOOGLE_OFICIAL_LAB.cmd

Comportamento:
Ao abrir o CMD, ele ja comeca automaticamente usando o .tsv encontrado na pasta input.
Ele continua do ultimo checkpoint salvo em output\NOME_DO_TSV_google_oficial_progresso.jsonl.
Para reprocessar do zero, rode RESETAR_GOOGLE_OFICIAL.cmd antes.

Saida principal:
output\NOME_DO_TSV_google_oficial_importavel.tsv

Colunas:
lema_pt
traducao_en
marcas_dicionario
motivo_tecnico
status_traducao

Exemplo normal:
bom	good	adj.		

Exemplo tecnico-cientifico direto:
accipitrario	accipitrario	Anat.	termo_tecnico_cientifico:Anat.	sem_traducao

Exemplo tecnico-cientifico misto:
ananas	pineapple	m. | Bot.	termo_tecnico_cientifico:Bot.	

Outras saidas:
output\NOME_DO_TSV_google_oficial_bruto.jsonl
Texto bruto e dados coletados por palavra.

output\NOME_DO_TSV_google_oficial_progresso.jsonl
Controle de retomada. Se parar, rode de novo e ele ignora o que ja concluiu.

output\NOME_DO_TSV_google_oficial_relatorio.txt
Resumo e auditoria da execucao.

Comportamento seguro:
- usa o site oficial translate.google.com;
- nao usa API escondida;
- nao salva traducao;
- nao favorita;
- nao avalia;
- nao envia feedback;
- nao abre Mostrar dicionario;
- coleta apenas o que aparece na tela principal;
- salva uma palavra por vez;
- recarrega a pagina do tradutor antes de cada nova palavra;
- se uma palavra falhar, segue para a proxima com checkpoint salvo.
