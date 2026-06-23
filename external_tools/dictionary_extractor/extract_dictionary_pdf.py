import argparse
import csv
import re
import sys
import time
import unicodedata
from pathlib import Path

import fitz


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PDF = ROOT / "external_tools" / "dicionario-pt.pdf"
DEFAULT_OUTPUT = ROOT / "external_tools" / "dictionary_extractor" / "output" / "dicionario_pt_extraido.tsv"
DEFAULT_ABBREVIATIONS = ROOT / "external_tools" / "dictionary_extractor" / "abreviaturas_dicionario.tsv"

LIGATURES = {
    "\ufb00": "ff",
    "\ufb01": "fi",
    "\ufb02": "fl",
    "\ufb03": "ffi",
    "\ufb04": "ffl",
}

PDF_ACCENTS = {
    "´a": "á", "´e": "é", "´i": "í", "´o": "ó", "´u": "ú", "´y": "ý", "´ı": "í",
    "´A": "Á", "´E": "É", "´I": "Í", "´O": "Ó", "´U": "Ú", "´Y": "Ý",
    "`a": "à", "`e": "è", "`i": "ì", "`o": "ò", "`u": "ù",
    "`A": "À", "`E": "È", "`I": "Ì", "`O": "Ò", "`U": "Ù",
    "ˆa": "â", "ˆe": "ê", "ˆi": "î", "ˆo": "ô", "ˆu": "û", "ˆı": "î",
    "ˆA": "Â", "ˆE": "Ê", "ˆI": "Î", "ˆO": "Ô", "ˆU": "Û",
    "˜a": "ã", "˜e": "ẽ", "˜i": "ĩ", "˜o": "õ", "˜u": "ũ", "˜ı": "ĩ",
    "˜A": "Ã", "˜E": "Ẽ", "˜I": "Ĩ", "˜O": "Õ", "˜U": "Ũ",
    "¸c": "ç", "¸C": "Ç",
}

CLASS_PATTERNS = [
    "m., f. e adj.", "m. e adj.", "f. e adj.", "adv. e prep.",
    "loc. interj.", "loc. prep.", "loc. adv.",
    "v. pron.", "v. t.", "v. i.", "v. p.",
    "m. pl.", "f. pl.", "art. def.", "pron. ind.",
    "interj.", "abrev.", "adj.", "adv.", "art.", "conj.",
    "num.", "pref.", "prep.", "pron.", "suf.", "m.", "f.", "v.",
]

CLASS_RE = re.compile(
    r"^(?P<class>"
    + "|".join(re.escape(item) for item in sorted(CLASS_PATTERNS, key=len, reverse=True))
    + r")(?=\s|$)",
    flags=re.IGNORECASE,
)


def clean_text(text: str) -> str:
    for src, dst in LIGATURES.items():
        text = text.replace(src, dst)
    for src, dst in PDF_ACCENTS.items():
        text = text.replace(src, dst)
    text = text.replace("\u00a0", " ")
    return re.sub(r"\s+", " ", text).strip()


def is_bold_span(span: dict) -> bool:
    font = span.get("font", "")
    flags = int(span.get("flags", 0))
    return bool(flags & 16) or "CMBX" in font or "SFBX" in font


def line_text(line: dict) -> str:
    return clean_text("".join(span.get("text", "") for span in line.get("spans", [])))


def leading_bold_text(line: dict) -> str:
    chunks: list[str] = []
    started = False
    for span in line.get("spans", []):
        text = span.get("text", "")
        if not text:
            continue
        if is_bold_span(span):
            chunks.append(text)
            started = True
            continue
        if started:
            break
        if text.isspace():
            continue
        break
    return clean_text("".join(chunks))


def should_skip_line(text: str) -> bool:
    if not text:
        return True
    if text.isdigit():
        return True
    if re.fullmatch(r"[ivxlcdm]+", text, flags=re.IGNORECASE):
        return True
    return False


def clean_lemma(raw_lemma: str) -> str:
    lemma = clean_text(raw_lemma)
    lemma = re.sub(r",\s*\([^)]*\)\s*$", "", lemma)
    lemma = re.sub(r",\s*\d+\s*$", "", lemma)
    lemma = lemma.strip(" \t\r\n,;:")
    if re.fullmatch(r"(?:[A-Za-z]\.\s*){1,}[A-Za-z]?\.?", lemma):
        lemma = lemma.strip()
        if not lemma.endswith("."):
            lemma += "."
        if lemma.lower() == "a. c.":
            return "a. C."
        return lemma
    return lemma.strip(".").strip()


def parse_class_and_definition(full_line: str, lemma_raw: str) -> tuple[str, str]:
    rest = full_line[len(lemma_raw):].strip() if full_line.startswith(lemma_raw) else full_line
    rest = re.sub(r"^\d+\s*", "", rest)
    rest = re.sub(r"^\([^)]*\)\s*", "", rest)
    match = CLASS_RE.match(rest)
    if not match:
        return "", rest
    return match.group("class").strip().lower(), rest[match.end():].strip()


def join_continuation(previous: str, current: str) -> str:
    current = current.strip()
    if not previous:
        return current
    if previous.endswith("-") and current and current[0].islower():
        return previous[:-1] + current
    return previous + " " + current


def text_blocks_by_column(page: fitz.Page) -> list[dict]:
    data = page.get_text("dict")
    blocks = [block for block in data.get("blocks", []) if block.get("type") == 0]
    page_mid = page.rect.width / 2

    def sort_key(block: dict) -> tuple[int, float]:
        x0 = block["bbox"][0]
        column = 0 if x0 < page_mid else 1
        return column, block["bbox"][1]

    return sorted(blocks, key=sort_key)


def extract_entries(pdf_path: Path, start_page: int = 4, max_pages: int | None = None) -> list[dict]:
    doc = fitz.open(pdf_path)
    start_index = max(0, start_page - 1)
    end_index = doc.page_count if max_pages is None else min(doc.page_count, start_index + max_pages)
    entries: list[dict] = []
    current: dict | None = None

    def close_current() -> None:
        nonlocal current
        if current is None:
            return
        current["definicao"] = clean_text(current.get("definicao", ""))
        if current["lema"] or current["classe"] or current["definicao"]:
            entries.append(current)
        current = None

    for page_index in range(start_index, end_index):
        page = doc.load_page(page_index)
        for block in text_blocks_by_column(page):
            if block["bbox"][1] < 45:
                continue
            for line in block.get("lines", []):
                text = line_text(line)
                if should_skip_line(text):
                    continue
                lemma_raw = leading_bold_text(line)
                if clean_text(lemma_raw) == "*":
                    if current is not None:
                        current["definicao"] = join_continuation(current.get("definicao", ""), text)
                    continue
                if lemma_raw and len(lemma_raw) <= 90:
                    close_current()
                    classe, definition = parse_class_and_definition(text, lemma_raw)
                    current = {
                        "lema": clean_lemma(lemma_raw),
                        "classe": classe,
                        "definicao": definition,
                        "pagina_pdf": page_index + 1,
                    }
                    continue
                if current is not None:
                    if not current.get("classe"):
                        classe, definition = parse_class_and_definition(text, "")
                        if classe:
                            current["classe"] = classe
                            current["definicao"] = join_continuation(current.get("definicao", ""), definition)
                            continue
                    current["definicao"] = join_continuation(current.get("definicao", ""), text)

    close_current()
    return entries


def write_tsv(entries: list[dict], output_path: Path, header: bool = False) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        if header:
            writer.writerow(["lema", "classe", "definicao"])
        for item in entries:
            writer.writerow([item.get("lema", ""), item.get("classe", ""), item.get("definicao", "")])


def normalize_abbreviation_text(value: str) -> str:
    text = value or ""
    for src, dst in LIGATURES.items():
        text = text.replace(src, dst)
    text = unicodedata.normalize("NFD", text)
    text = "".join(char for char in text if unicodedata.category(char) != "Mn")
    text = text.lower()
    return re.sub(r"\s+", " ", text).strip()


def abbreviation_key(value: str) -> str:
    return normalize_abbreviation_text(value).replace(".", "").strip()


def load_abbreviations(path: Path) -> list[str]:
    abbreviations: list[str] = []
    if not path.exists():
        return abbreviations
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.reader(handle, delimiter="\t")
        for row in reader:
            if row and row[0].strip():
                abbreviations.append(row[0].strip())
    return abbreviations


def abbreviation_pattern(abbreviation: str) -> re.Pattern[str] | None:
    tokens = re.findall(r"[a-z0-9º]+", normalize_abbreviation_text(abbreviation))
    if not tokens:
        return None
    body = r"\s*\.\s*".join(re.escape(token) for token in tokens)
    return re.compile(rf"(^|[^a-z0-9])({body}\.?)(?=$|[^a-z0-9])", flags=re.IGNORECASE)


def abbreviation_patterns(abbreviations: list[str]) -> list[tuple[str, re.Pattern[str]]]:
    patterns: list[tuple[str, re.Pattern[str]]] = []
    for abbreviation in abbreviations:
        pattern = abbreviation_pattern(abbreviation)
        if pattern is not None:
            patterns.append((abbreviation, pattern))
    return patterns


def ranges_overlap(first: tuple[int, int], second: tuple[int, int]) -> bool:
    return first[0] < second[1] and second[0] < first[1]


def entry_marks(entry: dict, patterns: list[tuple[str, re.Pattern[str]]]) -> list[str]:
    marks: list[str] = []
    seen: set[str] = set()

    def add(mark: str) -> None:
        clean = mark.strip()
        if not clean:
            return
        key = abbreviation_key(clean)
        if key in seen:
            return
        seen.add(key)
        marks.append(clean)

    add(entry.get("classe", ""))

    text = normalize_abbreviation_text(entry.get("definicao", ""))
    hits: list[tuple[int, int, int, str]] = []
    for abbreviation, pattern in patterns:
        for match in pattern.finditer(text):
            start = match.start(2)
            end = match.end(2)
            hits.append((start, end, end - start, abbreviation))

    hits.sort(key=lambda item: (item[0], -item[2]))
    accepted: list[tuple[int, int]] = []
    for start, end, _length, abbreviation in hits:
        current = (start, end)
        if any(ranges_overlap(current, existing) for existing in accepted):
            continue
        accepted.append(current)
        add(abbreviation)

    return marks


def write_marks_tsv(
    entries: list[dict],
    output_path: Path,
    abbreviations_path: Path,
    header: bool = False,
) -> dict:
    abbreviations = load_abbreviations(abbreviations_path)
    patterns = abbreviation_patterns(abbreviations)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        if header:
            writer.writerow(["lema", "marcas"])
        with_extra_marks = 0
        without_marks = 0
        for entry in entries:
            marks = entry_marks(entry, patterns)
            if len(marks) > (1 if entry.get("classe") else 0):
                with_extra_marks += 1
            if not marks:
                without_marks += 1
            writer.writerow([entry.get("lema", ""), " | ".join(marks)])
    return {
        "path": output_path,
        "abbreviations": len(abbreviations),
        "with_extra_marks": with_extra_marks,
        "without_marks": without_marks,
    }


def write_report(
    entries: list[dict],
    output_path: Path,
    pdf_path: Path,
    elapsed: float,
    marks_info: dict | None = None,
) -> None:
    report_path = output_path.with_name(output_path.stem + "_relatorio.txt")
    with report_path.open("w", encoding="utf-8") as handle:
        handle.write("ATLAS - EXTRACAO DE DICIONARIO PDF\n\n")
        handle.write(f"PDF: {pdf_path}\n")
        handle.write(f"TSV: {output_path}\n")
        handle.write(f"Entradas extraidas: {len(entries)}\n")
        handle.write(f"Tempo total: {elapsed:.2f}s\n\n")
        if marks_info:
            handle.write("Marcas / abreviacoes:\n")
            handle.write(f"TSV: {marks_info['path']}\n")
            handle.write(f"Abreviacoes de referencia: {marks_info['abbreviations']}\n")
            handle.write(f"Linhas com marcas alem da classe: {marks_info['with_extra_marks']}\n")
            handle.write(f"Linhas sem marcas: {marks_info['without_marks']}\n\n")
        handle.write("Exemplos:\n")
        for item in entries[:30]:
            handle.write(f"{item['lema']} | {item['classe']} | {item['definicao'][:180]}\n")
    print(f"[Atlas] relatorio: {report_path}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Extrai dicionario PDF em duas colunas para TSV.")
    parser.add_argument("--pdf", default=str(DEFAULT_PDF))
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT))
    parser.add_argument("--start-page", type=int, default=4)
    parser.add_argument("--max-pages", type=int, default=None)
    parser.add_argument("--header", action="store_true")
    parser.add_argument("--abbreviations", default=str(DEFAULT_ABBREVIATIONS))
    parser.add_argument("--marks-output", default=None)
    parser.add_argument("--no-marks", action="store_true")
    args = parser.parse_args()

    started = time.perf_counter()
    pdf_path = Path(args.pdf).resolve()
    output_path = Path(args.output).resolve()
    print(f"[Atlas] extraindo: {pdf_path}", flush=True)
    entries = extract_entries(pdf_path, args.start_page, args.max_pages)
    write_tsv(entries, output_path, args.header)
    marks_info = None
    if not args.no_marks:
        marks_output = Path(args.marks_output).resolve() if args.marks_output else output_path.with_name(
            output_path.stem + "_marcas_abreviacoes.tsv"
        )
        marks_info = write_marks_tsv(
            entries,
            marks_output,
            Path(args.abbreviations).resolve(),
            args.header,
        )
    elapsed = time.perf_counter() - started
    print(f"[Atlas] entradas={len(entries)}", flush=True)
    print(f"[Atlas] saida={output_path}", flush=True)
    if marks_info:
        print(f"[Atlas] marcas={marks_info['path']}", flush=True)
        print(
            f"[Atlas] marcas_com_contexto={marks_info['with_extra_marks']} sem_marcas={marks_info['without_marks']}",
            flush=True,
        )
    print(f"[Atlas] tempo={elapsed:.2f}s", flush=True)
    write_report(entries, output_path, pdf_path, elapsed, marks_info)
    return 0


if __name__ == "__main__":
    sys.exit(main())
