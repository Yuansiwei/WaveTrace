using Microsoft.Diagnostics.Symbols;
using Microsoft.Diagnostics.Tracing;
using Microsoft.Diagnostics.Tracing.Etlx;
using Microsoft.Diagnostics.Tracing.Parsers.Kernel;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;

internal static class DiagSessionTop
{
    private sealed class Options
    {
        public string Input = string.Empty;
        public string Process = string.Empty;
        public string Csv = string.Empty;
        public string SymbolPath = string.Empty;
        public int Top = 100;
        public bool KeepExpanded;
    }

    private sealed class FunctionStat
    {
        public string Function = string.Empty;
        public string Module = string.Empty;
        public long SelfSamples;
        public long InclusiveSamples;
    }

    private sealed class StackStat
    {
        public string Stack = string.Empty;
        public long Samples;
    }

    private sealed class Totals
    {
        public long Samples;
        public long SamplesWithStack;
        public long SamplesWithoutStack;
        public readonly Dictionary<string, FunctionStat> Functions =
            new Dictionary<string, FunctionStat>(StringComparer.Ordinal);
        public readonly Dictionary<string, StackStat> Stacks =
            new Dictionary<string, StackStat>(StringComparer.Ordinal);
        public readonly Dictionary<int, long> ProcessSamples = new Dictionary<int, long>();
        public readonly Dictionary<int, string> ProcessNames = new Dictionary<int, string>();
    }

    private static int Main(string[] args)
    {
        try
        {
            Options options;
            if (!TryParse(args, out options)) return 2;

            string expandedDirectory = null;
            IReadOnlyList<string> etlFiles = ResolveEtlFiles(options.Input, out expandedDirectory);
            if (etlFiles.Count == 0)
            {
                Console.Error.WriteLine("No ETL file was found in: " + options.Input);
                return 3;
            }

            string symbolPath = BuildSymbolPath(options);
            var totals = new Totals();
            foreach (string etl in etlFiles)
            {
                Console.Error.WriteLine("[diagsession-top] reading " + etl);
                AnalyzeEtl(etl, options, symbolPath, totals);
            }

            PrintReport(options, totals);
            if (!string.IsNullOrEmpty(options.Csv)) WriteCsv(options.Csv, totals);

            if (expandedDirectory != null && !options.KeepExpanded)
            {
                TryDeleteDirectory(expandedDirectory);
            }
            return totals.Samples == 0 ? 4 : 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("diagsession-top failed: " + ex);
            return 1;
        }
    }

    private static bool TryParse(string[] args, out Options options)
    {
        options = new Options();
        if (args.Length == 0 || args[0] == "--help" || args[0] == "-h")
        {
            PrintUsage();
            return false;
        }
        options.Input = Path.GetFullPath(args[0]);
        for (int i = 1; i < args.Length; ++i)
        {
            string arg = args[i];
            if (arg == "--process" && i + 1 < args.Length) options.Process = args[++i];
            else if (arg == "--top" && i + 1 < args.Length)
            {
                int top;
                if (!int.TryParse(args[++i], NumberStyles.Integer, CultureInfo.InvariantCulture, out top) || top <= 0)
                    throw new ArgumentException("--top requires a positive integer");
                options.Top = top;
            }
            else if (arg == "--csv" && i + 1 < args.Length) options.Csv = Path.GetFullPath(args[++i]);
            else if (arg == "--symbol-path" && i + 1 < args.Length) options.SymbolPath = args[++i];
            else if (arg == "--keep-expanded") options.KeepExpanded = true;
            else throw new ArgumentException("Unknown or incomplete option: " + arg);
        }
        if (!File.Exists(options.Input)) throw new FileNotFoundException("Input file not found", options.Input);
        return true;
    }

    private static void PrintUsage()
    {
        Console.WriteLine("Usage:");
        Console.WriteLine("  DiagSessionTop.exe <report.diagsession|trace.etl> [options]");
        Console.WriteLine();
        Console.WriteLine("Options:");
        Console.WriteLine("  --process <name|pid>    Filter samples by process name or PID");
        Console.WriteLine("  --top <N>               Print the top N functions and stacks (default 100)");
        Console.WriteLine("  --csv <file>            Write function statistics as CSV");
        Console.WriteLine("  --symbol-path <path>    Extra _NT_SYMBOL_PATH-compatible symbol path");
        Console.WriteLine("  --keep-expanded         Keep the temporary extracted directory");
    }

    private static IReadOnlyList<string> ResolveEtlFiles(string input, out string expandedDirectory)
    {
        expandedDirectory = null;
        if (string.Equals(Path.GetExtension(input), ".etl", StringComparison.OrdinalIgnoreCase))
            return new[] { input };

        expandedDirectory = Path.Combine(
            Path.GetTempPath(),
            "diagsession_top_" + Path.GetFileNameWithoutExtension(input) + "_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(expandedDirectory);
        ZipFile.ExtractToDirectory(input, expandedDirectory);

        return Directory.GetFiles(expandedDirectory, "*.etl", SearchOption.AllDirectories)
            .OrderByDescending(path => new FileInfo(path).Length)
            .ToArray();
    }

    private static string BuildSymbolPath(Options options)
    {
        var parts = new List<string>();
        if (!string.IsNullOrWhiteSpace(options.SymbolPath)) parts.Add(options.SymbolPath);
        string environment = Environment.GetEnvironmentVariable("_NT_SYMBOL_PATH");
        if (!string.IsNullOrWhiteSpace(environment)) parts.Add(environment);
        parts.Add(Path.GetDirectoryName(options.Input));
        parts.Add(Environment.CurrentDirectory);
        return string.Join(";", parts.Where(p => !string.IsNullOrWhiteSpace(p)).Distinct(StringComparer.OrdinalIgnoreCase));
    }

    private static void AnalyzeEtl(string etl, Options options, string symbolPath, Totals totals)
    {
        string etlx = etl + ".etlx";
        try
        {
            using (TraceLog log = TraceLog.OpenOrConvert(etl))
            {
                ResolveSymbols(log, symbolPath, options);
                foreach (TraceEvent data in log.Events)
                {
                    var sample = data as SampledProfileTraceData;
                    if (sample == null) continue;

                    int pid = sample.ProcessID;
                    string processName = sample.ProcessName ?? string.Empty;
                    totals.ProcessNames[pid] = processName;
                    long processCount;
                    totals.ProcessSamples.TryGetValue(pid, out processCount);
                    totals.ProcessSamples[pid] = processCount + 1;
                    if (!MatchesProcess(options.Process, pid, processName)) continue;

                    ++totals.Samples;
                    TraceCallStack stack = sample.CallStack();
                    if (stack == null)
                    {
                        ++totals.SamplesWithoutStack;
                        continue;
                    }
                    ++totals.SamplesWithStack;
                    AddStack(stack, totals);
                }
            }
        }
        finally
        {
            if (File.Exists(etlx)) File.Delete(etlx);
        }
    }

    private static void ResolveSymbols(TraceLog log, string symbolPath, Options options)
    {
        if (string.IsNullOrWhiteSpace(symbolPath)) return;
        using (var reader = new SymbolReader(TextWriter.Null, symbolPath))
        {
            for (int i = 0; i < log.ModuleFiles.Count; ++i)
            {
                TraceModuleFile module = log.ModuleFiles[(ModuleFileIndex)i];
                try
                {
                    log.CodeAddresses.LookupSymbolsForModule(reader, module);
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine("[diagsession-top] symbol warning " + module.Name + ": " + ex.Message);
                }
            }
        }
    }

    private static bool MatchesProcess(string filter, int pid, string name)
    {
        if (string.IsNullOrEmpty(filter)) return true;
        int requestedPid;
        if (int.TryParse(filter, NumberStyles.Integer, CultureInfo.InvariantCulture, out requestedPid))
            return requestedPid == pid;
        string cleanName = Path.GetFileNameWithoutExtension(name ?? string.Empty);
        string cleanFilter = Path.GetFileNameWithoutExtension(filter);
        return string.Equals(cleanName, cleanFilter, StringComparison.OrdinalIgnoreCase) ||
               cleanName.IndexOf(cleanFilter, StringComparison.OrdinalIgnoreCase) >= 0;
    }

    private static void AddStack(TraceCallStack stack, Totals totals)
    {
        var frames = new List<string>();
        var inclusiveSeen = new HashSet<string>(StringComparer.Ordinal);
        bool leaf = true;
        for (TraceCallStack current = stack; current != null; current = current.Caller)
        {
            TraceCodeAddress address = current.CodeAddress;
            string module = address.ModuleName ?? "(unknown-module)";
            string function = address.FullMethodName;
            if (string.IsNullOrWhiteSpace(function))
                function = module + "!0x" + address.Address.ToString("x", CultureInfo.InvariantCulture);
            string key = module + "\0" + function;

            FunctionStat stat;
            if (!totals.Functions.TryGetValue(key, out stat))
            {
                stat = new FunctionStat { Function = function, Module = module };
                totals.Functions.Add(key, stat);
            }
            if (leaf) stat.SelfSamples++;
            if (inclusiveSeen.Add(key)) stat.InclusiveSamples++;
            leaf = false;
            frames.Add(function);
        }

        string collapsed = string.Join(" <- ", frames);
        StackStat stackStat;
        if (!totals.Stacks.TryGetValue(collapsed, out stackStat))
        {
            stackStat = new StackStat { Stack = collapsed };
            totals.Stacks.Add(collapsed, stackStat);
        }
        stackStat.Samples++;
    }

    private static void PrintReport(Options options, Totals totals)
    {
        Console.WriteLine("samples={0} with_stack={1} without_stack={2}",
            totals.Samples, totals.SamplesWithStack, totals.SamplesWithoutStack);
        if (totals.Samples == 0)
        {
            Console.WriteLine("No samples matched --process. Available processes:");
            foreach (var pair in totals.ProcessSamples.OrderByDescending(p => p.Value).Take(options.Top))
                Console.WriteLine("  pid={0} samples={1} name={2}", pair.Key, pair.Value, totals.ProcessNames[pair.Key]);
            return;
        }

        Console.WriteLine();
        Console.WriteLine("Top functions by self samples:");
        Console.WriteLine("{0,10} {1,9} {2,10} {3}", "self", "self%", "inclusive", "function");
        foreach (FunctionStat stat in totals.Functions.Values
                     .OrderByDescending(s => s.SelfSamples)
                     .ThenByDescending(s => s.InclusiveSamples)
                     .Take(options.Top))
        {
            double percent = 100.0 * stat.SelfSamples / totals.Samples;
            Console.WriteLine("{0,10} {1,8:F2}% {2,10} {3}",
                stat.SelfSamples, percent, stat.InclusiveSamples, DisplayFunction(stat));
        }

        Console.WriteLine();
        Console.WriteLine("Top sampled stacks:");
        foreach (StackStat stat in totals.Stacks.Values.OrderByDescending(s => s.Samples).Take(options.Top))
            Console.WriteLine("{0,10} {1}", stat.Samples, stat.Stack);
    }

    private static string DisplayFunction(FunctionStat stat)
    {
        string prefix = stat.Module + "!";
        return stat.Function.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)
            ? stat.Function
            : prefix + stat.Function;
    }

    private static void WriteCsv(string path, Totals totals)
    {
        string directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
        using (var writer = new StreamWriter(path, false, new UTF8Encoding(false)))
        {
            writer.WriteLine("self_samples,inclusive_samples,self_percent,module,function");
            foreach (FunctionStat stat in totals.Functions.Values
                         .OrderByDescending(s => s.SelfSamples)
                         .ThenByDescending(s => s.InclusiveSamples))
            {
                double percent = totals.Samples == 0 ? 0 : 100.0 * stat.SelfSamples / totals.Samples;
                writer.WriteLine(string.Join(",", new[] {
                    stat.SelfSamples.ToString(CultureInfo.InvariantCulture),
                    stat.InclusiveSamples.ToString(CultureInfo.InvariantCulture),
                    percent.ToString("F6", CultureInfo.InvariantCulture),
                    Csv(stat.Module),
                    Csv(stat.Function)
                }));
            }
        }
        Console.Error.WriteLine("[diagsession-top] wrote " + path);
    }

    private static string Csv(string value)
    {
        value = value ?? string.Empty;
        return "\"" + value.Replace("\"", "\"\"") + "\"";
    }

    private static void TryDeleteDirectory(string path)
    {
        try { Directory.Delete(path, true); }
        catch (Exception ex) { Console.Error.WriteLine("[diagsession-top] cleanup warning: " + ex.Message); }
    }
}
