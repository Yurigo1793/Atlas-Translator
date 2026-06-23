param(
    [string]$InputGlob = "datasets/MediaWiki_Split_500k/enwiktionary_part_*.xml",
    [int]$MaxPages = 0,
    [int]$SampleLimit = 80
)

$ErrorActionPreference = "Stop"

$files = @(Get-ChildItem -Path $InputGlob -File | Sort-Object Name)
if ($files.Count -eq 0) {
    throw "No input files matched: $InputGlob"
}

Add-Type -ReferencedAssemblies System.Xml -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using System.Xml;

public sealed class TranslationAnalyzer
{
    private static readonly Regex Heading = new Regex(@"^(={2,6})\s*([^=]+?)\s*\1\s*$", RegexOptions.Compiled);
    private static readonly Regex Template = new Regex(@"\{\{\s*([^|}\s]+)\s*(?:\|([^}]*))?", RegexOptions.Compiled | RegexOptions.IgnoreCase);
    private static readonly Regex TranslationTemplate = new Regex(@"\{\{\s*(t(?:\+|-check|-simple)?)\s*\|\s*([^|}]+)\s*\|\s*([^|}]+)", RegexOptions.Compiled | RegexOptions.IgnoreCase);

    private readonly int sampleLimit;
    private readonly Dictionary<string, long> templateCounts = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, long> targetLanguageCounts = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, long> sourceLanguageCounts = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, long> translationBlockSourceCounts = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, long> translationTemplateSourceCounts = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, long> headingCounts = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, long> qualifierParamCounts = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
    private readonly List<string> examples = new List<string>();
    private readonly List<string> complexExamples = new List<string>();
    private readonly List<string> nonTemplateTranslationLines = new List<string>();

    private long pages;
    private long mainPages;
    private long translationBlocks;
    private long translationLines;
    private long explicitTranslationTemplates;
    private long translationLinesWithExplicitTemplate;
    private long translationLinesWithoutExplicitTemplate;
    private long linesWithMultipleTemplates;
    private long templatesWithExtraParams;
    private long templatesWithRomanization;
    private long templatesWithGender;
    private long templatesWithQualifiers;

    public TranslationAnalyzer(int sampleLimit)
    {
        this.sampleLimit = sampleLimit;
    }

    public void AnalyzeFile(string path, int maxPages)
    {
        XmlReaderSettings settings = new XmlReaderSettings();
        settings.DtdProcessing = DtdProcessing.Ignore;
        using (XmlReader reader = XmlReader.Create(path, settings))
        {
            string title = "";
            string ns = "";
            string text = "";
            bool inPage = false;

            while (reader.Read())
            {
                if (reader.NodeType == XmlNodeType.Element)
                {
                    if (reader.LocalName == "page")
                    {
                        inPage = true;
                        title = "";
                        ns = "";
                        text = "";
                    }
                    else if (inPage && reader.LocalName == "title")
                    {
                        title = reader.ReadElementContentAsString();
                    }
                    else if (inPage && reader.LocalName == "ns")
                    {
                        ns = reader.ReadElementContentAsString();
                    }
                    else if (inPage && reader.LocalName == "text")
                    {
                        text = reader.ReadElementContentAsString();
                    }
                }
                else if (reader.NodeType == XmlNodeType.EndElement && reader.LocalName == "page")
                {
                    inPage = false;
                    pages++;
                    if (ns == "0" && title.IndexOf(':') < 0 && text.Length > 0)
                    {
                        mainPages++;
                        AnalyzePage(title, text);
                    }
                    if (maxPages > 0 && pages >= maxPages)
                    {
                        return;
                    }
                }
            }
        }
    }

    public void Print()
    {
        Console.WriteLine("Pages read: " + pages);
        Console.WriteLine("Main pages: " + mainPages);
        Console.WriteLine("Translations blocks: " + translationBlocks);
        Console.WriteLine("Translation bullet lines: " + translationLines);
        Console.WriteLine("Lines with explicit t/t+/t-check/t-simple: " + translationLinesWithExplicitTemplate);
        Console.WriteLine("Lines without explicit t-template: " + translationLinesWithoutExplicitTemplate);
        Console.WriteLine("Explicit translation templates: " + explicitTranslationTemplates);
        Console.WriteLine("Lines with multiple templates: " + linesWithMultipleTemplates);
        Console.WriteLine("Templates with extra params: " + templatesWithExtraParams);
        Console.WriteLine("Templates with gender params: " + templatesWithGender);
        Console.WriteLine("Templates with romanization/transliteration params: " + templatesWithRomanization);
        Console.WriteLine("Templates with qualifier/gloss params: " + templatesWithQualifiers);
        PrintTop("Top source language headings", sourceLanguageCounts, 25);
        PrintTop("Top source languages with Translations blocks", translationBlockSourceCounts, 25);
        PrintTop("Top source languages with explicit translation templates", translationTemplateSourceCounts, 25);
        PrintTop("Top target language codes", targetLanguageCounts, 40);
        PrintTop("Top template names in translation bullets", templateCounts, 40);
        PrintTop("Top non-language headings seen", headingCounts, 30);
        PrintTop("Top extra parameter keys on t-templates", qualifierParamCounts, 30);
        PrintList("Examples", examples);
        PrintList("Complex template examples", complexExamples);
        PrintList("Lines without explicit t-template", nonTemplateTranslationLines);
    }

    private void AnalyzePage(string title, string text)
    {
        string currentLang = "";
        bool inTranslations = false;
        foreach (string rawLine in text.Split('\n'))
        {
            string line = rawLine.Trim();
            Match heading = Heading.Match(line);
            if (heading.Success)
            {
                int level = heading.Groups[1].Value.Length;
                string headingText = heading.Groups[2].Value.Trim();
                Add(headingCounts, headingText);
                if (level == 2)
                {
                    currentLang = headingText;
                    Add(sourceLanguageCounts, currentLang);
                    inTranslations = false;
                }
                else if (currentLang.Length > 0 && level >= 4 && level <= 5 && headingText.Equals("Translations", StringComparison.OrdinalIgnoreCase))
                {
                    inTranslations = true;
                    translationBlocks++;
                    Add(translationBlockSourceCounts, currentLang);
                }
                else if (level >= 2)
                {
                    inTranslations = false;
                }
                continue;
            }

            if (!inTranslations || !line.StartsWith("*"))
            {
                continue;
            }

            translationLines++;
            int lineTemplateCount = 0;
            foreach (Match match in Template.Matches(line))
            {
                lineTemplateCount++;
                string name = match.Groups[1].Value.Trim().ToLowerInvariant();
                Add(templateCounts, name);
            }
            if (lineTemplateCount > 1)
            {
                linesWithMultipleTemplates++;
            }

            MatchCollection translationMatches = TranslationTemplate.Matches(line);
            if (translationMatches.Count == 0)
            {
                translationLinesWithoutExplicitTemplate++;
                AddSample(nonTemplateTranslationLines, title + " | " + currentLang + " | " + line);
                continue;
            }

            translationLinesWithExplicitTemplate++;
            foreach (Match match in translationMatches)
            {
                explicitTranslationTemplates++;
                Add(translationTemplateSourceCounts, currentLang);
                string code = match.Groups[2].Value.Trim().ToLowerInvariant();
                Add(targetLanguageCounts, code);

                string fullTemplate = ExtractTemplateFrom(line, match.Index);
                string[] pieces = fullTemplate.Trim('{', '}').Split('|');
                if (pieces.Length > 3)
                {
                    templatesWithExtraParams++;
                    bool gender = false;
                    bool romanization = false;
                    bool qualifier = false;
                    for (int i = 3; i < pieces.Length; i++)
                    {
                        string param = pieces[i].Trim();
                        int eq = param.IndexOf('=');
                        if (eq > 0)
                        {
                            string key = param.Substring(0, eq).Trim().ToLowerInvariant();
                            Add(qualifierParamCounts, key);
                            if (key == "tr" || key == "ts" || key == "sc") romanization = true;
                            if (key == "gloss" || key == "lit" || key == "alt" || key == "q" || key == "qq") qualifier = true;
                        }
                        else if (param == "m" || param == "f" || param == "n" || param == "c" || param == "p" || param == "s")
                        {
                            gender = true;
                        }
                    }
                    if (gender) templatesWithGender++;
                    if (romanization) templatesWithRomanization++;
                    if (qualifier) templatesWithQualifiers++;
                    AddSample(complexExamples, title + " | " + currentLang + " | " + fullTemplate);
                }
                AddSample(examples, title + " | " + currentLang + " -> " + code + " | " + fullTemplate);
            }
        }
    }

    private static string ExtractTemplateFrom(string line, int start)
    {
        int depth = 0;
        for (int i = start; i < line.Length - 1; i++)
        {
            if (line[i] == '{' && line[i + 1] == '{')
            {
                depth++;
                i++;
                continue;
            }
            if (line[i] == '}' && line[i + 1] == '}')
            {
                depth--;
                i++;
                if (depth <= 0)
                {
                    return line.Substring(start, i - start + 1);
                }
            }
        }
        return line.Substring(start);
    }

    private void AddSample(List<string> samples, string value)
    {
        if (samples.Count < sampleLimit)
        {
            samples.Add(value);
        }
    }

    private static void Add(Dictionary<string, long> counts, string key)
    {
        if (String.IsNullOrWhiteSpace(key)) return;
        if (!counts.ContainsKey(key)) counts[key] = 0;
        counts[key]++;
    }

    private static void PrintTop(string title, Dictionary<string, long> counts, int limit)
    {
        Console.WriteLine();
        Console.WriteLine(title + ":");
        int emitted = 0;
        foreach (KeyValuePair<string, long> item in Top(counts))
        {
            Console.WriteLine(item.Value + "\t" + item.Key);
            emitted++;
            if (emitted >= limit) break;
        }
    }

    private static IEnumerable<KeyValuePair<string, long>> Top(Dictionary<string, long> counts)
    {
        List<KeyValuePair<string, long>> items = new List<KeyValuePair<string, long>>(counts);
        items.Sort((a, b) => {
            int compare = b.Value.CompareTo(a.Value);
            return compare != 0 ? compare : StringComparer.OrdinalIgnoreCase.Compare(a.Key, b.Key);
        });
        return items;
    }

    private static void PrintList(string title, List<string> samples)
    {
        Console.WriteLine();
        Console.WriteLine(title + ":");
        foreach (string sample in samples)
        {
            Console.WriteLine(sample);
        }
    }
}
'@

$analyzer = [TranslationAnalyzer]::new($SampleLimit)
$remaining = $MaxPages
foreach ($file in $files) {
    if ($MaxPages -gt 0 -and $remaining -le 0) {
        break
    }
    $limitForFile = if ($MaxPages -gt 0) { $remaining } else { 0 }
    $analyzer.AnalyzeFile($file.FullName, $limitForFile)
    if ($MaxPages -gt 0) {
        $remaining = $MaxPages - ($MaxPages - $remaining + [Math]::Min($remaining, 0))
    }
}
$analyzer.Print()
