const fs = require("fs");
const path = require("path");
const { chromium } = require("playwright");

const ROOT = path.resolve(__dirname, "..", "..");
const TOOL_DIR = __dirname;
const DEFAULT_INPUT = path.join(TOOL_DIR, "input");
const DEFAULT_OUTPUT_DIR = path.join(TOOL_DIR, "output");
const DEFAULT_PROFILE_DIR = path.join(TOOL_DIR, "browser_profile");
const DEFAULT_CHROME_PROFILE_DIR = path.join(process.env.LOCALAPPDATA || "", "Google", "Chrome", "User Data");

function parseArgs(argv) {
  const args = {
    input: DEFAULT_INPUT,
    outputDir: DEFAULT_OUTPUT_DIR,
    profileDir: DEFAULT_PROFILE_DIR,
    sourceLang: "pt",
    targetLang: "en",
    limit: 0,
    headed: false,
    timeout: 45000,
    preTranslateDelayMs: 1000,
    force: false,
    skipTechnicalDictionaryTerms: true,
    useDefaultChromeProfile: false,
    chromeProfileDirectory: "Default",
    cdpUrl: "",
  };

  for (let index = 2; index < argv.length; index += 1) {
    const arg = argv[index];
    const next = argv[index + 1];
    if (arg === "--input") args.input = path.resolve(next), index += 1;
    else if (arg === "--output-dir") args.outputDir = path.resolve(next), index += 1;
    else if (arg === "--profile-dir") args.profileDir = path.resolve(next), index += 1;
    else if (arg === "--source-lang") args.sourceLang = next, index += 1;
    else if (arg === "--target-lang") args.targetLang = next, index += 1;
    else if (arg === "--limit") args.limit = Number(next || "0"), index += 1;
    else if (arg === "--timeout") args.timeout = Number(next || "45000"), index += 1;
    else if (arg === "--pre-translate-delay") args.preTranslateDelayMs = Number(next || "1000"), index += 1;
    else if (arg === "--headed") args.headed = true;
    else if (arg === "--force") args.force = true;
    else if (arg === "--no-skip-technical") args.skipTechnicalDictionaryTerms = false;
    else if (arg === "--use-default-chrome-profile") args.useDefaultChromeProfile = true;
    else if (arg === "--chrome-profile-directory") args.chromeProfileDirectory = next || "Default", index += 1;
    else if (arg === "--cdp-url") args.cdpUrl = next || "", index += 1;
    else if (arg === "--help") {
      printHelp();
      process.exit(0);
    }
  }

  return args;
}

function printHelp() {
  console.log([
    "ATLAS - GOOGLE OFICIAL LAB",
    "",
    "Uso:",
    "  node google_official_translate_lab.js --input input",
    "",
    "Opcoes:",
    "  --input ARQUIVO/PASTA TSV de entrada, ou pasta com um unico .tsv. A primeira coluna e o lema.",
    "  --limit N             Processa no maximo N linhas.",
    "  --headed              Mostra o navegador na frente. Sem isso, abre Chrome minimizado.",
    "  --force               Reprocessa linhas ja concluidas.",
    "  --use-default-chrome-profile Usa o Chrome logado do usuario. Feche o Chrome antes.",
    "  --chrome-profile-directory Nome do perfil do Chrome. Padrao: Default.",
    "  --cdp-url URL          Conecta em um Chrome ja aberto com remote debugging.",
    "  --timeout MS          Tempo maximo para uma etapa travada.",
    "  --pre-translate-delay MS Espera antes de enviar uma nova palavra. Padrao: 1000.",
    "  --no-skip-technical   Nao grava termos tecnicos direto; envia tudo ao Google.",
    "",
  ].join("\n"));
}

function ensureDir(dir) {
  fs.mkdirSync(dir, { recursive: true });
}

function nowStamp() {
  const date = new Date();
  const pad = (value) => String(value).padStart(2, "0");
  return [
    date.getFullYear(),
    pad(date.getMonth() + 1),
    pad(date.getDate()),
    "_",
    pad(date.getHours()),
    pad(date.getMinutes()),
    pad(date.getSeconds()),
  ].join("");
}

function readEntries(inputPath) {
  const text = fs.readFileSync(inputPath, "utf8").replace(/^\uFEFF/, "");
  return text
    .split(/\r?\n/)
    .map((line, index) => ({ line, lineNumber: index + 1 }))
    .filter((item) => item.line.trim())
    .map((item) => {
      const parts = item.line.split("\t");
      if (parts.length !== 2) {
        throw new Error(`modelo_entrada_invalido_linha_${item.lineNumber}: esperado lema<TAB>marcas_abreviacoes`);
      }
      const dictionaryMarks = parseDictionaryMarks(parts.slice(1).join(" | "));
      return {
        lineNumber: item.lineNumber,
        lemma: (parts[0] || "").trim(),
        dictionaryMarks,
        inputModel: "marcas_abreviacoes",
        originalLine: item.line,
      };
    })
    .filter((item) => !(item.lineNumber === 1 && sameUiText(item.lemma, "lema")))
    .filter((item) => item.lemma);
}

function parseDictionaryMarks(value) {
  return [...new Set(String(value || "")
    .split("|")
    .map((item) => item.trim())
    .filter(Boolean))];
}

function dictionaryMarksText(entry) {
  return (entry.dictionaryMarks || []).join(" | ");
}

const SCIENTIFIC_DICTIONARY_MARKS = [
  "Anat.",
  "Anthrop.",
  "Arith.",
  "Astron.",
  "Bacter.",
  "Biol.",
  "Bot.",
  "Chim.",
  "Ch\u00edm.",
  "Cir.",
  "Entom.",
  "Geod.",
  "Geol.",
  "Geom.",
  "Hist. Nat.",
  "Icht.",
  "Med.",
  "Miner.",
  "Pathol.",
  "Pharm.",
  "Phot.",
  "Phys.",
  "Ph\u00fds.",
  "Physiol.",
  "Terat.",
  "Veter.",
  "Zool.",
];

function technicalDictionaryReason(entry) {
  const mark = (entry.dictionaryMarks || []).find((item) =>
    SCIENTIFIC_DICTIONARY_MARKS.some((scientificMark) => sameUiText(item, scientificMark))
  );
  return mark ? `termo_tecnico_cientifico:${mark}` : "";
}

function hasCommonDictionaryMark(entry) {
  return (entry.dictionaryMarks || []).some((mark) => {
    const text = normalizeUiText(mark);
    return /\bm\b/.test(text)
      || /\bf\b/.test(text)
      || /\badj\b/.test(text)
      || /\badv\b/.test(text)
      || /\bv\b/.test(text)
      || text.includes("subst")
      || text.includes("pron")
      || text.includes("prep")
      || text.includes("conj")
      || text.includes("interj")
      || text.includes("loc");
  });
}

function isPureScientificTechnical(entry) {
  return Boolean(technicalDictionaryReason(entry)) && !hasCommonDictionaryMark(entry);
}

function translationStatusForRow(entry, target) {
  return sameUiText(entry.lemma, target) ? "sem_traducao" : "";
}

function resolveInputPath(inputPath) {
  const stat = fs.statSync(inputPath);
  if (stat.isFile()) {
    if (path.extname(inputPath).toLowerCase() !== ".tsv") {
      throw new Error(`entrada_nao_tsv: ${inputPath}`);
    }
    return inputPath;
  }

  if (!stat.isDirectory()) {
    throw new Error(`entrada_invalida: ${inputPath}`);
  }

  const files = fs.readdirSync(inputPath)
    .filter((name) => path.extname(name).toLowerCase() === ".tsv")
    .sort((a, b) => a.localeCompare(b, "pt-BR"));

  if (!files.length) {
    throw new Error(`nenhum_tsv_em: ${inputPath}`);
  }

  if (files.length > 1) {
    throw new Error([
      `mais_de_um_tsv_em: ${inputPath}`,
      "Deixe apenas um TSV na pasta input ou chame o script com --input caminho\\arquivo.tsv.",
      ...files.map((file) => `- ${file}`),
    ].join("\n"));
  }

  return path.join(inputPath, files[0]);
}

function outputPrefixFor(inputPath) {
  return path.basename(inputPath, path.extname(inputPath))
    .replace(/[<>:"/\\|?*\x00-\x1F]/g, "_")
    .replace(/\s+/g, "_")
    .slice(0, 120);
}

function readDone(progressPath) {
  if (!fs.existsSync(progressPath)) return new Set();
  const lines = fs.readFileSync(progressPath, "utf8").split(/\r?\n/).filter(Boolean);
  return new Set(lines.map((line) => {
    try {
      const item = JSON.parse(line);
      return `${item.lineNumber}\t${item.lemma}`;
    } catch {
      return "";
    }
  }).filter(Boolean));
}

function appendLine(file, value) {
  fs.appendFileSync(file, `${value}\r\n`, "utf8");
}

function firstLine(file) {
  if (!fs.existsSync(file)) return "";
  const content = fs.readFileSync(file, "utf8");
  return (content.split(/\r?\n/, 1)[0] || "").replace(/^\uFEFF/, "");
}

function moveIfExists(file, suffix) {
  if (!fs.existsSync(file)) return;
  fs.renameSync(file, `${file}.${suffix}`);
}

function tsv(value) {
  return String(value ?? "")
    .replace(/\r?\n/g, " ")
    .replace(/\t/g, " ")
    .trim();
}

function normalizeUiText(value) {
  return String(value ?? "")
    .normalize("NFD")
    .replace(/[\u0300-\u036f]/g, "")
    .toLowerCase()
    .trim();
}

function sameUiText(value, expected) {
  return normalizeUiText(value) === normalizeUiText(expected);
}

function includesUiText(value, expected) {
  return normalizeUiText(value).includes(normalizeUiText(expected));
}

function startsWithUiText(value, expected) {
  return normalizeUiText(value).startsWith(normalizeUiText(expected));
}

function findInstalledBrowser() {
  const candidates = [
    "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
    "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
    "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
    "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
  ];
  return candidates.find((candidate) => fs.existsSync(candidate)) || "";
}

function isLikelyProfileLocked(profileDir) {
  return fs.existsSync(path.join(profileDir, "SingletonLock"))
    || fs.existsSync(path.join(profileDir, "SingletonCookie"))
    || fs.existsSync(path.join(profileDir, "SingletonSocket"));
}

async function waitForUsefulResult(page, timeoutMs) {
  const result = await waitForStableBodyText(page, timeoutMs, (text) => {
    const loading = isGoogleLoading(text);
    if (isGoogleTranslationError(text)) return true;
    const hasResult = includesUiText(text, "Resultado da tradução")
      || includesUiText(text, "Mais traduções")
      || includesUiText(text, "Mostrar dicionário")
      || includesUiText(text, "Mais detalhes");
    return hasResult && !loading;
  });
  return result;
}

async function waitUntilGoogleStopsLoading(page, timeoutMs) {
  return await waitForStableBodyText(page, timeoutMs, (text) => text.trim() && !isGoogleLoading(text));
}

function isGoogleLoading(text) {
  return includesUiText(text, "Carregando tradução") || includesUiText(text, "Translating");
}

function isGoogleTranslationError(text) {
  return includesUiText(text, "Erro de tradução")
    || includesUiText(text, "Tente de novo")
    || includesUiText(text, "Translation error")
    || includesUiText(text, "Try again");
}

async function waitForStableBodyText(page, timeoutMs, isReady) {
  const started = Date.now();
  let lastText = "";
  let lastChange = Date.now();
  let latestText = "";

  while (Date.now() - started < timeoutMs) {
    const text = await page.locator("body").innerText({ timeout: 3000 }).catch(() => "");
    latestText = text;

    if (text !== lastText) {
      lastText = text;
      lastChange = Date.now();
    }

    if (isReady(text) && Date.now() - lastChange >= 450) {
      return { ready: true, text };
    }

    await page.waitForTimeout(100);
  }

  return { ready: false, text: latestText };
}

async function fillSource(page, lemma) {
  for (let attempt = 1; attempt <= 3; attempt += 1) {
    const candidates = [
      page.locator("textarea[aria-label='Texto de origem']"),
      page.locator("[contenteditable='true'][aria-label='Texto de origem']"),
      page.locator("[role='combobox'][aria-label='Texto de origem']"),
      page.getByLabel("Texto de origem"),
      page.locator("[aria-label='Texto de origem']"),
      page.locator("textarea[aria-label]"),
      page.locator("[contenteditable='true']"),
    ];

    for (const locator of candidates) {
      const count = await locator.count().catch(() => 0);
      if (!count) continue;
      const first = locator.first();
      const visible = await first.isVisible().catch(() => false);
      if (!visible) continue;

      await first.click({ timeout: 10000 });
      await page.keyboard.press("Control+A");
      await page.keyboard.press("Backspace");
      await first.fill(lemma, { timeout: 10000 }).catch(async () => {
        await page.keyboard.type(lemma);
      });

      const ready = await waitForStableBodyText(page, 8000, (text) => includesUiText(text, lemma));
      if (ready.ready) return true;
    }
  }

  return false;
}

async function openTranslatorHome(page, args) {
  await page.goto(`https://translate.google.com/?sl=${args.sourceLang}&tl=${args.targetLang}&op=translate`, {
    waitUntil: "domcontentloaded",
    timeout: args.timeout,
  });

  const ready = await waitForStableBodyText(page, args.timeout, (text) => (
    includesUiText(text, "Texto de origem") || includesUiText(text, "Tradução de textos")
  ));
  return ready.ready;
}

async function openTranslationForLemma(page, args, lemma) {
  const url = `https://translate.google.com/?sl=${encodeURIComponent(args.sourceLang)}&tl=${encodeURIComponent(args.targetLang)}&text=${encodeURIComponent(lemma)}&op=translate`;

  let last = { ready: false, text: "" };
  for (let attempt = 1; attempt <= 4; attempt += 1) {
    if (args.preTranslateDelayMs > 0) {
      await page.waitForTimeout(args.preTranslateDelayMs);
    }

    if (attempt === 1) {
      await page.goto(url, {
        waitUntil: "domcontentloaded",
        timeout: args.timeout,
      });
    } else {
      console.log(`[Google oficial] ${lemma}: erro de traducao, recarregando mesma pagina (${attempt}/4)`);
      await page.reload({ waitUntil: "domcontentloaded", timeout: args.timeout });
    }

    last = await waitForUsefulResult(page, args.timeout);
    if (!isGoogleTranslationError(last.text)) return last;
  }

  return last;
}

function extractFirstAfter(lines, label, stopLabels = []) {
  const index = lines.findIndex((line) => sameUiText(line, label));
  if (index < 0) return "";
  for (let i = index + 1; i < lines.length; i += 1) {
    const line = lines[i].trim();
    if (!line) continue;
    if (stopLabels.some((stopLabel) => sameUiText(line, stopLabel))) return "";
    return line;
  }
  return "";
}

function extractSimpleSections(visibleText) {
  const lines = visibleText.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
  const result = {
    mainTranslation: extractFirstAfter(lines, "Resultado da tradução", [
      "Salvar tradução",
      "Mais traduções",
      "Enviar feedback",
    ]),
    hasDictionaryButton: lines.some((line) => sameUiText(line, "Mostrar dicionário")),
    hasDetailsPanel: lines.some((line) => sameUiText(line, "Mais detalhes")),
    hasMoreTranslations: lines.some((line) => sameUiText(line, "Mais traduções")),
    moreTranslationsText: "",
    detailsText: "",
    definitionsText: "",
    examplesText: "",
    detailedTranslationsText: "",
    mainClassText: "",
    mainContextText: "",
    suggestionContextText: "",
  };

  const moreIndex = lines.findIndex((line) => sameUiText(line, "Mais traduções"));
  if (moreIndex >= 0) {
    const end = lines.findIndex((line, index) => index > moreIndex && sameUiText(line, "Enviar feedback"));
    result.moreTranslationsText = lines.slice(moreIndex + 1, end > moreIndex ? end : moreIndex + 80).join(" | ");
  }

  result.suggestionContextText = extractSuggestionContext(lines, result.mainTranslation);

  const detailsIndex = lines.findIndex((line) => sameUiText(line, "Mais detalhes"));
  if (detailsIndex >= 0) {
    result.detailsText = lines.slice(detailsIndex, detailsIndex + 220).join(" | ");
  }

  const definitionsIndex = lines.findIndex((line) => startsWithUiText(line, "Definições de "));
  if (definitionsIndex >= 0) {
    const end = lines.findIndex((line, index) => index > definitionsIndex && startsWithUiText(line, "Exemplos de "));
    result.definitionsText = lines.slice(definitionsIndex + 1, end > definitionsIndex ? end : definitionsIndex + 80).join(" | ");
  }

  const examplesIndex = lines.findIndex((line) => startsWithUiText(line, "Exemplos de "));
  if (examplesIndex >= 0) {
    const end = lines.findIndex((line, index) => index > examplesIndex && startsWithUiText(line, "Traduções de "));
    result.examplesText = lines.slice(examplesIndex + 1, end > examplesIndex ? end : examplesIndex + 80).join(" | ");
  }

  const translationsIndex = lines.findIndex((line) => startsWithUiText(line, "Traduções de "));
  if (translationsIndex >= 0) {
    result.detailedTranslationsText = lines.slice(translationsIndex + 1, translationsIndex + 180).join(" | ");
  }

  return result;
}

function extractSuggestionContext(lines, mainTranslation) {
  const start = lines.findIndex((line) => sameUiText(line, "Texto de origem"));
  const end = lines.findIndex((line, index) => index > start && (sameUiText(line, "clear") || sameUiText(line, "Resultados de tradução")));
  if (start < 0 || end <= start) return "";

  const terms = [];
  for (let index = start + 1; index < end; index += 1) {
    const source = lines[index];
    const target = lines[index + 2];
    if (!source || !target) continue;
    if (source === "\u00a0" || target === "\u00a0") continue;
    if (sameUiText(source, "Texto de origem")) continue;
    if (sameUiText(target, mainTranslation)) {
      terms.push(source);
    }
  }

  return cleanContextTerms(terms);
}

async function collectDomHints(page) {
  return await page.evaluate(() => {
    const clean = (value) => String(value || "").replace(/\s+/g, " ").trim();
    const reverseLists = [...document.querySelectorAll('[aria-label]')]
      .filter((element) => /traduções reversas|traducoes reversas|reverse translations/i.test(element.getAttribute("aria-label") || ""));

    const firstReverseList = reverseLists[0] || null;
    let mainContextTerms = firstReverseList
      ? [...firstReverseList.querySelectorAll("button")]
        .map((element) => clean(element.textContent))
        .filter(Boolean)
      : [];
    if (firstReverseList && !mainContextTerms.length) {
      mainContextTerms = [...firstReverseList.children]
        .map((element) => clean(element.textContent))
        .filter(Boolean);
    }

    let mainBlockText = "";
    if (firstReverseList) {
      let node = firstReverseList;
      for (let index = 0; index < 4 && node; index += 1) {
        mainBlockText = clean(node.textContent);
        if (/Adjetivo|Substantivo|Verbo|Advérbio|Adverbio/i.test(mainBlockText)) break;
        node = node.parentElement;
      }
    }

    return {
      mainContextTerms: [...new Set(mainContextTerms)],
      mainBlockText,
    };
  }).catch(() => ({ mainContextTerms: [], mainBlockText: "" }));
}

const GOOGLE_CLASS_LABELS = new Set([
  "adjetivo",
  "substantivo",
  "verbo",
  "advérbio",
  "adverbio",
  "pronome",
  "preposição",
  "preposicao",
  "conjunção",
  "conjuncao",
  "interjeição",
  "interjeicao",
]);

function isGoogleClassLabel(value) {
  return GOOGLE_CLASS_LABELS.has(normalizeUiText(value));
}

function classToEnglish(value) {
  const key = normalizeUiText(value);
  const map = {
    "adjetivo": "adjective",
    "substantivo": "noun",
    "verbo": "verb",
    "advérbio": "adverb",
    "adverbio": "adverb",
    "pronome": "pronoun",
    "preposição": "preposition",
    "preposicao": "preposition",
    "conjunção": "conjunction",
    "conjuncao": "conjunction",
    "interjeição": "interjection",
    "interjeicao": "interjection",
  };
  return map[key] || value || "";
}

function sourceClassToEnglish(value) {
  const text = normalizeUiText(value);
  if (/\badj\b/.test(text)) return "adjective";
  if (/\badv\b/.test(text) || text.includes("loc adv")) return "adverb";
  if (/\bv\b/.test(text)) return "verb";
  if (/\bm\b/.test(text) || /\bf\b/.test(text) || text.includes("subst")) return "noun";
  return "";
}

function findClassLabelInText(value) {
  const labels = ["Adjetivo", "Substantivo", "Verbo", "Advérbio", "Adverbio", "Pronome", "Preposição", "Preposicao", "Conjunção", "Conjuncao", "Interjeição", "Interjeicao"];
  const normalized = normalizeUiText(value);
  return labels.find((label) => normalized.includes(normalizeUiText(label))) || "";
}

function extractTranslationRows(entry, extracted) {
  const rows = [];
  const mainClass = classToEnglish(extracted.mainClassText || extractClassNearMain(extracted.detailsText || extracted.moreTranslationsText || ""))
    || sourceClassToEnglish(dictionaryMarksText(entry));
  const moreTranslations = parseMoreTranslations(extracted.moreTranslationsText);
  const mainFromList = moreTranslations.find((item) => sameUiText(item.target, extracted.mainTranslation));
  if (extracted.mainTranslation) {
    rows.push({
      source: entry.lemma,
      target: extracted.mainTranslation,
      className: classToEnglish(mainFromList?.className || mainClass) || mainClass,
      context: cleanContextTerms(extracted.mainContextTerms?.length ? extracted.mainContextTerms : (mainFromList?.reverses || []))
        || extracted.suggestionContextText,
      translationStatus: translationStatusForRow(entry, extracted.mainTranslation),
    });
  }

  for (const item of moreTranslations) {
    if (!item.target || sameUiText(item.target, extracted.mainTranslation)) continue;
    rows.push({
      source: entry.lemma,
      target: item.target,
      className: classToEnglish(item.className),
      context: cleanContextTerms(item.reverses),
      translationStatus: translationStatusForRow(entry, item.target),
    });
  }

  return rows;
}

function cleanContextTerms(terms) {
  return [...new Set(
    (terms || [])
      .map((term) => String(term || "").trim())
      .filter(Boolean)
      .filter((term) => term !== "\u200b")
      .filter((term) => !sameUiText(term, "Abrir tudo"))
      .filter((term) => !sameUiText(term, "Abrir detalhes da tradução"))
  )].join(" ");
}

function extractClassNearMain(text) {
  const lines = String(text || "").split("|").map((line) => line.trim()).filter(Boolean);
  return lines.find(isGoogleClassLabel) || "";
}

function parseMoreTranslations(text) {
  const raw = String(text || "")
    .split("|")
    .map((line) => line.trim())
    .filter(Boolean)
    .filter((line) => !sameUiText(line, "Abrir tudo"))
    .filter((line) => !sameUiText(line, "Abrir detalhes da tradução"))
    .filter((line) => !sameUiText(line, "Enviar feedback"));

  const rows = [];
  for (let index = 0; index < raw.length - 1; index += 1) {
    if (!isGoogleClassLabel(raw[index + 1])) continue;
    const target = raw[index];
    const className = raw[index + 1];
    const reverses = [];
    let cursor = index + 2;
    while (cursor < raw.length) {
      if (cursor + 1 < raw.length && isGoogleClassLabel(raw[cursor + 1])) break;
      if (!isGoogleClassLabel(raw[cursor]) && raw[cursor] !== "\u200b") reverses.push(raw[cursor]);
      cursor += 1;
    }
    rows.push({ target, className, reverses });
    index = cursor - 1;
  }

  return rows;
}

async function main() {
  const args = parseArgs(process.argv);
  const browserProfileDir = args.useDefaultChromeProfile ? DEFAULT_CHROME_PROFILE_DIR : args.profileDir;

  ensureDir(args.outputDir);
  ensureDir(browserProfileDir);

  const inputPath = resolveInputPath(args.input);
  const outputPrefix = outputPrefixFor(inputPath);
  const entries = readEntries(inputPath);
  const selected = args.limit > 0 ? entries.slice(0, args.limit) : entries;
  const runId = nowStamp();
  const rawPath = path.join(args.outputDir, `${outputPrefix}_google_oficial_bruto.jsonl`);
  const progressPath = path.join(args.outputDir, `${outputPrefix}_google_oficial_progresso.jsonl`);
  const importPath = path.join(args.outputDir, `${outputPrefix}_google_oficial_importavel.tsv`);
  const reportPath = path.join(args.outputDir, `${outputPrefix}_google_oficial_relatorio.txt`);
  const importHeader = [
    "lema_pt",
    "traducao_en",
    "marcas_dicionario",
    "motivo_tecnico",
    "status_traducao",
  ].join("\t");

  if (!args.force && fs.existsSync(importPath) && firstLine(importPath) !== importHeader) {
    const backupSuffix = `modelo_antigo_${nowStamp()}.bak`;
    moveIfExists(importPath, backupSuffix);
    moveIfExists(progressPath, backupSuffix);
    moveIfExists(rawPath, backupSuffix);
  }

  const done = args.force ? new Set() : readDone(progressPath);

  if (!fs.existsSync(importPath) || args.force) {
    fs.writeFileSync(importPath, importHeader + "\r\n", "utf8");
    if (args.force) {
      fs.writeFileSync(rawPath, "", "utf8");
      fs.writeFileSync(progressPath, "", "utf8");
    }
  }

  fs.writeFileSync(reportPath, [
    "ATLAS - GOOGLE TRADUTOR OFICIAL",
    "",
    `Entrada: ${inputPath}`,
    `Saida importavel TSV: ${importPath}`,
    `Saida bruta JSONL: ${rawPath}`,
    `Progresso: ${progressPath}`,
    `Perfil do navegador: ${browserProfileDir}`,
    `Perfil Chrome: ${args.useDefaultChromeProfile ? args.chromeProfileDirectory : "(isolado)"}`,
    `Total selecionado: ${selected.length}`,
    `Inicio: ${new Date().toISOString()}`,
    "",
  ].join("\r\n"), "utf8");

  console.log("[Atlas] Google Tradutor oficial");
  console.log(`[Atlas] entrada=${inputPath}`);
  console.log(`[Atlas] selecionadas=${selected.length} segundo_plano=${args.headed ? "nao" : "sim, Chrome minimizado"}`);
  console.log("[Atlas] regra: interacao minima, sem login, sem salvar, sem avaliar, sem feedback.");

  const executablePath = findInstalledBrowser();
  if (executablePath) {
    console.log(`[Atlas] navegador=${executablePath}`);
  } else {
    console.log("[Atlas] navegador=Playwright Chromium");
  }
  console.log(`[Atlas] perfil=${args.cdpUrl ? "Chrome ja aberto via CDP" : browserProfileDir}`);
  console.log(`[Atlas] perfil_chrome=${args.useDefaultChromeProfile || args.cdpUrl ? args.chromeProfileDirectory : "isolado"}`);
  if (args.cdpUrl) console.log(`[Atlas] cdp=${args.cdpUrl}`);

  if (!args.cdpUrl && args.useDefaultChromeProfile && isLikelyProfileLocked(browserProfileDir)) {
    throw new Error([
      "perfil_chrome_em_uso",
      `Feche todas as janelas do Chrome antes de executar.`,
      `Perfil: ${browserProfileDir}`,
      `Comando equivalente: "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe" --profile-directory="${args.chromeProfileDirectory}"`,
    ].join("\n"));
  }

  let browser = null;
  let context = null;
  let shouldCloseContext = true;

  if (args.cdpUrl) {
    browser = await chromium.connectOverCDP(args.cdpUrl);
    context = browser.contexts()[0];
    if (!context) context = await browser.newContext({ viewport: { width: 1400, height: 900 }, locale: "pt-BR" });
    shouldCloseContext = false;
  } else {
    context = await chromium.launchPersistentContext(browserProfileDir, {
      ...(executablePath ? { executablePath } : {}),
      headless: false,
      viewport: { width: 1400, height: 900 },
      locale: "pt-BR",
      args: [
        "--disable-notifications",
        "--disable-features=Translate",
        "--disable-background-timer-throttling",
        "--disable-backgrounding-occluded-windows",
        "--disable-renderer-backgrounding",
        ...(args.useDefaultChromeProfile ? [`--profile-directory=${args.chromeProfileDirectory}`] : []),
        ...(args.headed ? [] : ["--start-minimized"]),
      ],
    });
  }

  let page = null;
  if (args.cdpUrl) {
    page = context.pages().find((candidate) => {
      const url = candidate.url() || "";
      return url === "about:blank" || url.includes("translate.google.com");
    }) || context.pages()[0] || await context.newPage();
  } else {
    page = context.pages().find((candidate) => (candidate.url() || "").includes("translate.google.com"));
    if (!page) page = context.pages()[0] || await context.newPage();
  }
  page.setDefaultTimeout(15000);
  await openTranslatorHome(page, args);

  let processed = 0;
  let skipped = 0;
  let technicalTerms = 0;
  let errors = 0;

  for (const entry of selected) {
    const key = `${entry.lineNumber}\t${entry.lemma}`;
    if (done.has(key)) {
      skipped += 1;
      console.log(`[Google oficial] ${entry.lineNumber}: ${entry.lemma} | ja processado`);
      continue;
    }

    const marksText = dictionaryMarksText(entry);
    const technicalReason = args.skipTechnicalDictionaryTerms ? technicalDictionaryReason(entry) : "";
    const scientificTechnical = technicalReason ? "sim" : "nao";
    const pureScientificTechnical = args.skipTechnicalDictionaryTerms && isPureScientificTechnical(entry);
    if (pureScientificTechnical) {
      technicalTerms += 1;
      const status = "termo_tecnico_cientifico";
      appendLine(importPath, [
        entry.lemma,
        entry.lemma,
        marksText,
        technicalReason,
        "sem_traducao",
      ].map(tsv).join("\t"));
      appendLine(progressPath, JSON.stringify({
        lineNumber: entry.lineNumber,
        lemma: entry.lemma,
        status,
        reason: technicalReason,
        dictionaryMarks: entry.dictionaryMarks,
        pureScientificTechnical,
        inputModel: entry.inputModel,
        capturedAt: new Date().toISOString(),
      }));
      appendLine(reportPath, [
        `Linha ${entry.lineNumber}: ${entry.lemma}`,
        `Status: ${status}`,
        `Marcas: ${marksText || "(nenhuma)"}`,
        `Motivo: ${technicalReason}`,
        `Importavel: ${entry.lemma} -> ${entry.lemma} | termo_tecnico`,
        "",
      ].join("\r\n"));
      console.log(`[Google oficial] ${entry.lineNumber}/${selected.length}: ${entry.lemma} -> ${entry.lemma} | termo_tecnico | direto`);
      continue;
    }

    const started = Date.now();
    let status = "ok";
    let visibleText = "";
    let extracted = {};

    try {
      let ready = await openTranslationForLemma(page, args, entry.lemma);
      if (!ready.ready) {
        console.log(`[Google oficial] ${entry.lineNumber}: navegacao direta lenta, tentando preencher manualmente`);
        await openTranslatorHome(page, args);
        if (args.preTranslateDelayMs > 0) {
          await page.waitForTimeout(args.preTranslateDelayMs);
        }
        const filled = await fillSource(page, entry.lemma);
        if (!filled) throw new Error("campo_origem_nao_encontrado_apos_retry");
        ready = await waitForUsefulResult(page, args.timeout);
      }
      visibleText = ready.text;
      if (!ready.ready) status = "carregamento_lento_ou_incompleto";
      if (isGoogleTranslationError(visibleText)) status = "erro_de_traducao_google";

      extracted = extractSimpleSections(visibleText);
      const domHints = await collectDomHints(page);
      extracted.mainContextTerms = domHints.mainContextTerms || [];
      extracted.mainClassText = findClassLabelInText(domHints.mainBlockText || "");
      if (!extracted.mainTranslation) status = status === "ok" ? "sem_traducao_principal_visivel" : status;
    } catch (error) {
      errors += 1;
      status = `erro:${error.message}`;
      visibleText = await page.locator("body").innerText({ timeout: 5000 }).catch(() => "");
      extracted = extractSimpleSections(visibleText);
      const domHints = await collectDomHints(page);
      extracted.mainContextTerms = domHints.mainContextTerms || [];
      extracted.mainClassText = findClassLabelInText(domHints.mainBlockText || "");
    }

    const elapsed = Date.now() - started;
    const rawRecord = {
      runId,
      lineNumber: entry.lineNumber,
      lemma: entry.lemma,
      dictionaryMarks: entry.dictionaryMarks,
      inputModel: entry.inputModel,
      scientificTechnical,
      technicalReason,
      pureScientificTechnical,
      commonDictionaryMark: hasCommonDictionaryMark(entry) ? "sim" : "nao",
      status,
      elapsedMs: elapsed,
      extracted,
      visibleText,
      capturedAt: new Date().toISOString(),
    };

    appendLine(rawPath, JSON.stringify(rawRecord));

    for (const row of extractTranslationRows(entry, extracted)) {
      appendLine(importPath, [
        row.source,
        row.target,
        marksText,
        technicalReason,
        row.translationStatus,
      ].map(tsv).join("\t"));
    }

    appendLine(progressPath, JSON.stringify({
      lineNumber: entry.lineNumber,
      lemma: entry.lemma,
      status,
      dictionaryMarks: entry.dictionaryMarks,
      inputModel: entry.inputModel,
      elapsedMs: elapsed,
      capturedAt: new Date().toISOString(),
    }));

    processed += 1;
    appendLine(reportPath, [
      `Linha ${entry.lineNumber}: ${entry.lemma}`,
      `Marcas: ${marksText || "(nenhuma)"}`,
      `Tecnico cientifico: ${scientificTechnical}`,
      `Tecnico puro direto: ${pureScientificTechnical ? "sim" : "nao"}`,
      `Tradução principal: ${extracted.mainTranslation || "(vazia)"}`,
      `Mostrar dicionário: ${extracted.hasDictionaryButton ? "apareceu, nao usado" : "não"}`,
      `Mais traduções: ${extracted.moreTranslationsText ? "sim" : "não"}`,
      `Definições: ${extracted.definitionsText ? "sim" : "não"}`,
      `Exemplos: ${extracted.examplesText ? "sim" : "não"}`,
      `Status: ${status}`,
      `Tempo: ${elapsed} ms`,
      "",
    ].join("\r\n"));
    console.log(`[Google oficial] ${entry.lineNumber}/${selected.length}: ${entry.lemma} -> ${extracted.mainTranslation || "(vazio)"} | tela_principal=sim | ${elapsed} ms | ${status}`);

    if (status !== "ok") {
      console.log(`[Google oficial] ${entry.lineNumber}: status ruim, seguindo para proxima palavra por URL limpa`);
    }

  }

  if (shouldCloseContext) {
    await context.close();
  }

  fs.appendFileSync(reportPath, [
    `Fim: ${new Date().toISOString()}`,
    `Processadas: ${processed}`,
    `Ignoradas por progresso: ${skipped}`,
    `Termos tecnicos gravados direto: ${technicalTerms}`,
    `Erros: ${errors}`,
    "",
    "Arquivos:",
    `- ${importPath}`,
    `- ${rawPath}`,
    `- ${progressPath}`,
    "",
  ].join("\r\n"), "utf8");

  console.log(`[Atlas] fim processadas=${processed} ignoradas=${skipped} termos_tecnicos=${technicalTerms} erros=${errors}`);
  console.log(`[Atlas] TSV=${importPath}`);
  console.log(`[Atlas] bruto=${rawPath}`);
}

main().catch((error) => {
  console.error(`[ERRO] ${error.stack || error.message}`);
  process.exit(1);
});
