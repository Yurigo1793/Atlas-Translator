param(
    [string]$InputGlob = "datasets/MediaWiki/enwiktionary-*.xml",
    [string]$OutputDir = "datasets/MediaWiki_Split",
    [int]$PagesPerPart = 500000,
    [int]$Parts = 0,
    [switch]$Force,
    [switch]$DryRun,
    [switch]$SkipSpaceCheck
)

$ErrorActionPreference = "Stop"

$matches = @(Get-ChildItem -Path $InputGlob -File)
if ($matches.Count -eq 0) {
    throw "No input file matched: $InputGlob"
}
if ($matches.Count -gt 1) {
    $list = ($matches | ForEach-Object { "  - $($_.FullName)" }) -join [Environment]::NewLine
    throw "Input glob matched more than one file:$([Environment]::NewLine)$list"
}
if ($PagesPerPart -lt 1) {
    throw "PagesPerPart must be at least 1."
}
if ($Parts -lt 0) {
    throw "Parts must be 0 or greater."
}

$sourcePath = $matches[0].FullName
$outputPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDir)

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;

public sealed class BinaryLineReader : IDisposable
{
    private readonly FileStream stream;
    private readonly byte[] buffer;
    private readonly MemoryStream line;
    private int offset;
    private int count;

    public BinaryLineReader(string path)
    {
        stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1024 * 1024);
        buffer = new byte[1024 * 1024];
        line = new MemoryStream(16 * 1024);
    }

    public byte[] ReadLine()
    {
        line.SetLength(0);
        while (true)
        {
            if (offset >= count)
            {
                count = stream.Read(buffer, 0, buffer.Length);
                offset = 0;
                if (count == 0)
                {
                    return line.Length == 0 ? null : line.ToArray();
                }
            }

            int start = offset;
            while (offset < count && buffer[offset] != (byte)'\n')
            {
                offset++;
            }

            int length = offset - start;
            if (length > 0)
            {
                line.Write(buffer, start, length);
            }

            if (offset < count && buffer[offset] == (byte)'\n')
            {
                line.WriteByte(buffer[offset]);
                offset++;
                return line.ToArray();
            }
        }
    }

    public void Dispose()
    {
        line.Dispose();
        stream.Dispose();
    }
}

public sealed class SplitPlan
{
    public string Source;
    public string OutputDir;
    public int Parts;
    public long TotalPages;
    public long[] PagesPerPart;
    public byte[] Preamble;
    public byte[] NewLine;
}

public static class MediaWikiDumpSplitter
{
    private static readonly byte[] PageStart = Encoding.ASCII.GetBytes("<page>");
    private static readonly byte[] PageEnd = Encoding.ASCII.GetBytes("</page>");
    private static readonly byte[] MediaWikiEnd = Encoding.ASCII.GetBytes("</mediawiki>");
    private static readonly byte[] SiteInfoStart = Encoding.ASCII.GetBytes("<siteinfo");
    private static readonly byte[] SiteInfoEnd = Encoding.ASCII.GetBytes("</siteinfo>");

    public static int Run(string source, string outputDir, int pagesPerPart, int requestedParts, bool force, bool dryRun, bool skipSpaceCheck)
    {
        Stopwatch watch = Stopwatch.StartNew();
        SplitPlan plan = null;
        List<string> generated = new List<string>();
        long[] pagesWritten = new long[0];
        List<string> errors = new List<string>();

        try
        {
            outputDir = Path.GetFullPath(outputDir);
            Directory.CreateDirectory(outputDir);
            EnsureOutputReady(outputDir, force);

            if (!dryRun && !skipSpaceCheck)
            {
                EnsureMinimumSpaceBeforeScan(source, outputDir);
            }

            plan = ScanDump(source, outputDir, pagesPerPart, requestedParts);
            pagesWritten = new long[plan.Parts];

            Console.WriteLine("Input: " + plan.Source);
            Console.WriteLine("Pages detected: " + plan.TotalPages);
            if (requestedParts > 0)
            {
                Console.WriteLine("Mode: fixed part count (" + requestedParts + " parts)");
            }
            else
            {
                Console.WriteLine("Mode: fixed page count (" + pagesPerPart + " pages per part, last part gets the remainder)");
            }
            Console.WriteLine("Pages per part: " + string.Join(", ", plan.PagesPerPart));

            if (dryRun)
            {
                return 0;
            }

            if (!skipSpaceCheck)
            {
                EnsureEnoughSpace(plan);
            }

            SplitDump(plan, generated, pagesWritten);
            return 0;
        }
        catch (Exception ex)
        {
            errors.Add(ex.Message);
            Console.Error.WriteLine("ERROR: " + ex.Message);
            return 1;
        }
        finally
        {
            watch.Stop();
            if (plan != null && (generated.Count > 0 || errors.Count > 0))
            {
                try
                {
                    WriteReport(plan, generated, pagesWritten, watch.Elapsed.TotalSeconds, errors);
                }
                catch (Exception reportError)
                {
                    Console.Error.WriteLine("ERROR writing report: " + reportError.Message);
                }
            }
        }
    }

    private static SplitPlan ScanDump(string source, string outputDir, int pagesPerPart, int requestedParts)
    {
        long totalPages = 0;
        bool inPreamble = true;
        byte[] newline = Encoding.ASCII.GetBytes("\n");
        MemoryStream preamble = new MemoryStream(1024 * 1024);

        using (BinaryLineReader reader = new BinaryLineReader(source))
        {
            byte[] line;
            while ((line = reader.ReadLine()) != null)
            {
                if (EndsWith(line, new byte[] { (byte)'\r', (byte)'\n' }))
                {
                    newline = new byte[] { (byte)'\r', (byte)'\n' };
                }
                else if (EndsWith(line, new byte[] { (byte)'\n' }))
                {
                    newline = new byte[] { (byte)'\n' };
                }

                if (inPreamble)
                {
                    if (IsExactTagLine(line, PageStart))
                    {
                        inPreamble = false;
                        totalPages++;
                        continue;
                    }

                    preamble.Write(line, 0, line.Length);
                    continue;
                }

                if (IsExactTagLine(line, PageStart))
                {
                    totalPages++;
                }

                if (IsExactTagLine(line, MediaWikiEnd))
                {
                    break;
                }
            }
        }

        if (inPreamble)
        {
            throw new Exception("No <page> block was found in the dump.");
        }

        byte[] preambleBytes = preamble.ToArray();
        if (IndexOf(preambleBytes, SiteInfoStart) < 0 || IndexOf(preambleBytes, SiteInfoEnd) < 0)
        {
            throw new Exception("Could not detect a complete <siteinfo> block before the first <page>.");
        }

        SplitPlan plan = new SplitPlan();
        plan.Source = Path.GetFullPath(source);
        plan.OutputDir = outputDir;
        plan.Parts = requestedParts > 0 ? requestedParts : CalculatePartCount(totalPages, pagesPerPart);
        plan.TotalPages = totalPages;
        plan.PagesPerPart = requestedParts > 0
            ? CalculateEvenPagesPerPart(totalPages, plan.Parts)
            : CalculateFixedPagesPerPart(totalPages, pagesPerPart, plan.Parts);
        plan.Preamble = preambleBytes;
        plan.NewLine = newline;
        return plan;
    }

    private static void SplitDump(SplitPlan plan, List<string> generated, long[] pagesWritten)
    {
        int partIndex = 0;
        long currentTarget = plan.PagesPerPart[partIndex];
        FileStream output = OpenPart(plan, partIndex, generated);
        bool insidePage = false;

        try
        {
            using (BinaryLineReader reader = new BinaryLineReader(plan.Source))
            {
                byte[] line;
                while ((line = reader.ReadLine()) != null)
                {
                    if (IsExactTagLine(line, PageStart))
                    {
                        if (insidePage)
                        {
                            throw new Exception("Nested <page> detected.");
                        }

                        insidePage = true;
                        output.Write(line, 0, line.Length);
                        continue;
                    }

                    if (!insidePage)
                    {
                        if (IsExactTagLine(line, MediaWikiEnd))
                        {
                            break;
                        }
                        continue;
                    }

                    output.Write(line, 0, line.Length);

                    if (!IsExactTagLine(line, PageEnd))
                    {
                        continue;
                    }

                    pagesWritten[partIndex]++;
                    insidePage = false;

                    if (pagesWritten[partIndex] >= currentTarget && partIndex < plan.Parts - 1)
                    {
                        ClosePart(output, plan);
                        output = null;
                        partIndex++;
                        currentTarget = plan.PagesPerPart[partIndex];
                        output = OpenPart(plan, partIndex, generated);
                    }
                }
            }

            if (insidePage)
            {
                throw new Exception("Reached end of file inside an unfinished <page> block.");
            }

            while (partIndex < plan.Parts - 1)
            {
                ClosePart(output, plan);
                output = null;
                partIndex++;
                output = OpenPart(plan, partIndex, generated);
            }
        }
        finally
        {
            if (output != null)
            {
                ClosePart(output, plan);
            }
        }
    }

    private static FileStream OpenPart(SplitPlan plan, int partIndex, List<string> generated)
    {
        string path = OutputPath(plan.OutputDir, partIndex + 1, plan.Parts);
        FileStream output = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.Read, 1024 * 1024);
        output.Write(plan.Preamble, 0, plan.Preamble.Length);
        generated.Add(path);
        return output;
    }

    private static void ClosePart(FileStream output, SplitPlan plan)
    {
        byte[] close = Encoding.ASCII.GetBytes("</mediawiki>");
        output.Write(close, 0, close.Length);
        output.Write(plan.NewLine, 0, plan.NewLine.Length);
        output.Dispose();
    }

    private static void EnsureOutputReady(string outputDir, bool force)
    {
        List<string> existing = new List<string>();
        foreach (string path in Directory.GetFiles(outputDir, "enwiktionary_part_*.xml"))
        {
            existing.Add(path);
        }

        string reportPath = Path.Combine(outputDir, "MediaWiki_Split_Report.txt");
        if (File.Exists(reportPath))
        {
            existing.Add(reportPath);
        }

        if (existing.Count > 0 && !force)
        {
            throw new Exception("Output files already exist. Use -Force to overwrite:" + Environment.NewLine + string.Join(Environment.NewLine, existing));
        }

        if (existing.Count > 0 && force)
        {
            foreach (string path in existing)
            {
                File.Delete(path);
            }
        }
    }

    private static void EnsureMinimumSpaceBeforeScan(string source, string outputDir)
    {
        long needed = new FileInfo(source).Length + 4096;
        long free = new DriveInfo(Path.GetPathRoot(outputDir)).AvailableFreeSpace;
        if (free < needed)
        {
            throw new Exception("Not enough free disk space to create split files. The source is " + FormatBytes(needed) + ", but only " + FormatBytes(free) + " is available at " + outputDir + ".");
        }
    }

    private static void EnsureEnoughSpace(SplitPlan plan)
    {
        long needed = new FileInfo(plan.Source).Length + ((long)plan.Preamble.Length * plan.Parts) + 4096;
        long free = new DriveInfo(Path.GetPathRoot(plan.OutputDir)).AvailableFreeSpace;
        if (free < needed)
        {
            throw new Exception("Not enough free disk space for the split output. Need about " + FormatBytes(needed) + ", available " + FormatBytes(free) + " at " + plan.OutputDir + ".");
        }
    }

    private static void WriteReport(SplitPlan plan, List<string> generated, long[] pagesWritten, double elapsedSeconds, List<string> errors)
    {
        StringBuilder report = new StringBuilder();
        report.AppendLine("MediaWiki Split Report");
        report.AppendLine();
        report.AppendLine("Arquivo original: " + plan.Source);
        report.AppendLine("Tamanho original: " + FormatBytes(new FileInfo(plan.Source).Length) + " (" + new FileInfo(plan.Source).Length + " bytes)");
        report.AppendLine("Total de paginas detectadas: " + plan.TotalPages);
        report.AppendLine("Total de partes geradas: " + generated.Count);
        report.AppendLine();
        report.AppendLine("Partes:");

        for (int i = 0; i < generated.Count; i++)
        {
            FileInfo info = new FileInfo(generated[i]);
            long pages = i < pagesWritten.Length ? pagesWritten[i] : 0;
            report.AppendLine("- " + info.Name + ": " + pages + " paginas, " + FormatBytes(info.Length) + " (" + info.Length + " bytes)");
        }

        report.AppendLine();
        report.AppendLine("Tempo total: " + elapsedSeconds.ToString("0.00") + " segundos");
        report.AppendLine();
        report.AppendLine("Erros:");
        if (errors.Count == 0)
        {
            report.AppendLine("- nenhum");
        }
        else
        {
            foreach (string error in errors)
            {
                report.AppendLine("- " + error);
            }
        }

        string reportPath = Path.Combine(plan.OutputDir, "MediaWiki_Split_Report.txt");
        File.WriteAllText(reportPath, report.ToString(), new UTF8Encoding(false));
        Console.WriteLine("Report: " + reportPath);
    }

    private static int CalculatePartCount(long totalPages, int pagesPerPart)
    {
        if (totalPages == 0)
        {
            return 1;
        }
        return (int)((totalPages + pagesPerPart - 1) / pagesPerPart);
    }

    private static long[] CalculateFixedPagesPerPart(long totalPages, int pagesPerPart, int parts)
    {
        long[] result = new long[parts];
        long remaining = totalPages;
        for (int i = 0; i < parts; i++)
        {
            result[i] = Math.Min((long)pagesPerPart, remaining);
            remaining -= result[i];
        }
        return result;
    }

    private static long[] CalculateEvenPagesPerPart(long totalPages, int parts)
    {
        long[] result = new long[parts];
        long baseCount = totalPages / parts;
        long remainder = totalPages % parts;
        for (int i = 0; i < parts; i++)
        {
            result[i] = baseCount + (i < remainder ? 1 : 0);
        }
        return result;
    }

    private static string OutputPath(string outputDir, int partNumber, int parts)
    {
        return Path.Combine(outputDir, "enwiktionary_part_" + partNumber.ToString("00") + "_of_" + parts.ToString("00") + ".xml");
    }

    private static bool IsExactTagLine(byte[] line, byte[] tag)
    {
        int start = 0;
        int end = line.Length - 1;

        while (start <= end && (line[start] == (byte)' ' || line[start] == (byte)'\t'))
        {
            start++;
        }

        while (end >= start && (line[end] == (byte)' ' || line[end] == (byte)'\t' || line[end] == (byte)'\r' || line[end] == (byte)'\n'))
        {
            end--;
        }

        int length = end - start + 1;
        if (length != tag.Length)
        {
            return false;
        }

        for (int i = 0; i < tag.Length; i++)
        {
            if (line[start + i] != tag[i])
            {
                return false;
            }
        }
        return true;
    }

    private static bool EndsWith(byte[] value, byte[] suffix)
    {
        if (value.Length < suffix.Length)
        {
            return false;
        }
        for (int i = 0; i < suffix.Length; i++)
        {
            if (value[value.Length - suffix.Length + i] != suffix[i])
            {
                return false;
            }
        }
        return true;
    }

    private static int IndexOf(byte[] value, byte[] pattern)
    {
        for (int i = 0; i <= value.Length - pattern.Length; i++)
        {
            bool found = true;
            for (int j = 0; j < pattern.Length; j++)
            {
                if (value[i + j] != pattern[j])
                {
                    found = false;
                    break;
                }
            }
            if (found)
            {
                return i;
            }
        }
        return -1;
    }

    private static string FormatBytes(long size)
    {
        string[] units = new string[] { "B", "KB", "MB", "GB", "TB" };
        double value = size;
        int unit = 0;
        while (value >= 1024 && unit < units.Length - 1)
        {
            value /= 1024;
            unit++;
        }
        return value.ToString("0.00") + " " + units[unit];
    }
}
'@

exit ([MediaWikiDumpSplitter]::Run(
    $sourcePath,
    $outputPath,
    $PagesPerPart,
    $Parts,
    [bool]$Force,
    [bool]$DryRun,
    [bool]$SkipSpaceCheck
))
