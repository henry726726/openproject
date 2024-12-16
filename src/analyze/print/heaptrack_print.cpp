/*
    SPDX-FileCopyrightText: 2014-2016 Milian Wolff <mail@milianw.de>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

/**
 * @file heaptrack_print.cpp
 *
 * @brief Evaluate and print the collected heaptrack data.
 */

#include <boost/program_options.hpp>

#include "analyze/accumulatedtracedata.h"
#include "analyze/suppressions.h"

#include <future>
#include <iomanip>
#include <iostream>

#include <tsl/robin_set.h>

#include "util/config.h"

// 추가
#include <filesystem>
#include <chrono>
#include <iostream>
#include <iomanip> // for formatting output
//

using namespace std;
namespace po = boost::program_options;

namespace {

/**
 * Merged allocation information by instruction pointer outside of alloc funcs
 */
struct MergedAllocation : public AllocationData
{
    // individual backtraces
    std::vector<Allocation> traces;
    // location
    IpIndex ipIndex;
};

class formatBytes
{
public:
    formatBytes(int64_t bytes, int width = 0)
        : m_bytes(bytes)
        , m_width(width)
    {
    }

    friend ostream& operator<<(ostream& out, const formatBytes data);

private:
    int64_t m_bytes;
    int m_width;
};

template <typename Bytes>
ostream& writeBytes(ostream& out, Bytes bytes, int width, const char* unit)
{
    const auto unitLength = strlen(unit);
    if (width > static_cast<int>(unitLength)) {
        return out << fixed << setprecision(2) << setw(width - unitLength) << bytes << unit;
    } else {
        return out << fixed << setprecision(2) << bytes << *unit;
    }
}

ostream& operator<<(ostream& out, const formatBytes data)
{
    auto bytes = static_cast<double>(data.m_bytes);

    static const auto units = {"B", "KB", "MB", "GB", "TB"};
    auto unit = units.begin();
    size_t i = 0;
    while (i < units.size() - 1 && std::abs(bytes) > 1000.) {
        bytes /= 1000.;
        ++i;
        ++unit;
    }

    if (i == 0) {
        // no fractions for bytes
        return writeBytes(out, data.m_bytes, data.m_width, *unit);
    } else {
        return writeBytes(out, bytes, data.m_width, *unit);
    }
}

enum CostType
{
    Allocations,
    Temporary,
    Leaked,
    Peak
};

std::istream& operator>>(std::istream& in, CostType& type)
{
    std::string token;
    in >> token;
    if (token == "allocations")
        type = Allocations;
    else if (token == "temporary")
        type = Temporary;
    else if (token == "leaked")
        type = Leaked;
    else if (token == "peak")
        type = Peak;
    else
        in.setstate(std::ios_base::failbit);
    return in;
}

struct Printer final : public AccumulatedTraceData
{
    void finalize()
    {
        applyLeakSuppressions();
        filterAllocations();
        mergedAllocations = mergeAllocations(allocations);
    }

    void mergeAllocation(vector<MergedAllocation>* mergedAllocations, const Allocation& allocation) const
    {
        const auto trace = findTrace(allocation.traceIndex);
        const auto traceIp = findIp(trace.ipIndex);
        auto it = lower_bound(mergedAllocations->begin(), mergedAllocations->end(), traceIp,
                              [this](const MergedAllocation& allocation, const InstructionPointer traceIp) -> bool {
                                  // Compare meta data without taking the instruction pointer address into account.
                                  // This is useful since sometimes, esp. when we lack debug symbols, the same
                                  // function allocates memory at different IP addresses which is pretty useless
                                  // information most of the time
                                  // TODO: make this configurable, but on-by-default
                                  const auto allocationIp = findIp(allocation.ipIndex);
                                  return allocationIp.compareWithoutAddress(traceIp);
                              });
        if (it == mergedAllocations->end() || !findIp(it->ipIndex).equalWithoutAddress(traceIp)) {
            MergedAllocation merged;
            merged.ipIndex = trace.ipIndex;
            it = mergedAllocations->insert(it, merged);
        }
        it->traces.push_back(allocation);
    }

    // merge allocations so that different traces that point to the same
    // instruction pointer at the end where the allocation function is
    // called are combined
    vector<MergedAllocation> mergeAllocations(const vector<Allocation>& allocations) const
    {
        // TODO: merge deeper traces, i.e. A,B,C,D and A,B,C,F
        //       should be merged to A,B,C: D & F
        //       currently the below will only merge it to: A: B,C,D & B,C,F
        vector<MergedAllocation> ret;
        ret.reserve(allocations.size());
        for (const Allocation& allocation : allocations) {
            mergeAllocation(&ret, allocation);
        }
        for (MergedAllocation& merged : ret) {
            for (const Allocation& allocation : merged.traces) {
                merged.allocations += allocation.allocations;
                merged.leaked += allocation.leaked;
                merged.peak += allocation.peak;
                merged.temporary += allocation.temporary;
            }
        }
        return ret;
    }

    void filterAllocations()
    {
        if (filterBtFunction.empty()) {
            return;
        }
        allocations.erase(remove_if(allocations.begin(), allocations.end(),
                                    [&](const Allocation& allocation) -> bool {
                                        auto node = findTrace(allocation.traceIndex);
                                        while (node.ipIndex) {
                                            const auto& ip = findIp(node.ipIndex);
                                            if (isStopIndex(ip.frame.functionIndex)) {
                                                break;
                                            }
                                            auto matchFunction = [this](const Frame& frame) {
                                                return stringify(frame.functionIndex).find(filterBtFunction)
                                                    != string::npos;
                                            };
                                            if (matchFunction(ip.frame)) {
                                                return false;
                                            }
                                            for (const auto& inlined : ip.inlined) {
                                                if (matchFunction(inlined)) {
                                                    return false;
                                                }
                                            }
                                            node = findTrace(node.parentIndex);
                                        };
                                        return true;
                                    }),
                          allocations.end());
    }

    void printIndent(ostream& out, size_t indent, const char* indentString = "  ") const
    {
        while (indent--) {
            out << indentString;
        }
    }

    void printIp(const IpIndex ip, ostream& out, const size_t indent = 0) const
    {
        printIp(findIp(ip), out, indent);
    }

    void printIp(const InstructionPointer& ip, ostream& out, const size_t indent = 0, bool flameGraph = false) const
    {
        printIndent(out, indent);

        if (ip.frame.functionIndex) {
            out << prettyFunction(stringify(ip.frame.functionIndex));
        } else {
            out << "0x" << hex << ip.instructionPointer << dec;
        }

        if (flameGraph) {
            // only print the file name but nothing else
            auto printFile = [this, &out](FileIndex fileIndex) {
                const auto& file = stringify(fileIndex);
                auto idx = file.find_last_of('/') + 1;
                out << " (" << file.substr(idx) << ")";
            };
            if (ip.frame.fileIndex) {
                printFile(ip.frame.fileIndex);
            }
            out << ';';
            for (const auto& inlined : ip.inlined) {
                out << prettyFunction(stringify(inlined.functionIndex));
                printFile(inlined.fileIndex);
                out << ';';
            }
            return;
        }

        out << '\n';
        printIndent(out, indent + 1);

        if (ip.frame.fileIndex) {
            out << "at " << stringify(ip.frame.fileIndex) << ':' << ip.frame.line << '\n';
            printIndent(out, indent + 1);
        }

        if (ip.moduleIndex) {
            out << "in " << stringify(ip.moduleIndex);
        } else {
            out << "in ??";
        }
        out << '\n';

        for (const auto& inlined : ip.inlined) {
            printIndent(out, indent);
            out << prettyFunction(stringify(inlined.functionIndex)) << '\n';
            printIndent(out, indent + 1);
            out << "at " << stringify(inlined.fileIndex) << ':' << inlined.line << '\n';
        }
    }

    void printBacktrace(const TraceIndex traceIndex, ostream& out, const size_t indent = 0,
                        bool skipFirst = false) const
    {
        if (!traceIndex) {
            out << "  ??";
            return;
        }
        printBacktrace(findTrace(traceIndex), out, indent, skipFirst);
    }

    void printBacktrace(TraceNode node, ostream& out, const size_t indent = 0, bool skipFirst = false) const
    {
        tsl::robin_set<TraceIndex> recursionGuard;
        while (node.ipIndex) {
            const auto& ip = findIp(node.ipIndex);
            if (!skipFirst) {
                printIp(ip, out, indent);
            }
            skipFirst = false;

            if (isStopIndex(ip.frame.functionIndex)) {
                break;
            }

            if (!recursionGuard.insert(node.parentIndex).second) {
                cerr << "Trace recursion detected - corrupt data file? " << node.parentIndex.index << endl;
                break;
            }
            node = findTrace(node.parentIndex);
        };
    }

    /**
     * recursive top-down printer in the format
     *
     * func1;func2 (file);func2 (file);
     */
    void printFlamegraph(TraceNode node, ostream& out) const
    {
        if (!node.ipIndex) {
            return;
        }

        const auto& ip = findIp(node.ipIndex);

        if (!isStopIndex(ip.frame.functionIndex)) {
            printFlamegraph(findTrace(node.parentIndex), out);
        }
        printIp(ip, out, 0, true);
    }

    template <typename T, typename LabelPrinter, typename SubLabelPrinter>
    void printAllocations(T AllocationData::*member, LabelPrinter label, SubLabelPrinter sublabel)
    {
        if (mergeBacktraces) {
            printMerged(member, label, sublabel);
        } else {
            printUnmerged(member, label);
        }
    }

    template <typename T, typename LabelPrinter, typename SubLabelPrinter>
    void printMerged(T AllocationData::*member, LabelPrinter label, SubLabelPrinter sublabel)
    {
        auto sortOrder = [member](const AllocationData& l, const AllocationData& r) {
            return std::abs(l.*member) > std::abs(r.*member);
        };
        sort(mergedAllocations.begin(), mergedAllocations.end(), sortOrder);
        for (size_t i = 0; i < min(peakLimit, mergedAllocations.size()); ++i) {
            auto& allocation = mergedAllocations[i];
            if (!(allocation.*member)) {
                break;
            }
            label(allocation);
            printIp(allocation.ipIndex, cout);

            if (!allocation.ipIndex) {
                continue;
            }

            sort(allocation.traces.begin(), allocation.traces.end(), sortOrder);
            int64_t handled = 0;
            for (size_t j = 0; j < min(subPeakLimit, allocation.traces.size()); ++j) {
                const auto& trace = allocation.traces[j];
                if (!(trace.*member)) {
                    break;
                }
                sublabel(trace);
                handled += trace.*member;
                printBacktrace(trace.traceIndex, cout, 2, true);
            }
            if (allocation.traces.size() > subPeakLimit) {
                cout << "  and ";
                if (member == &AllocationData::allocations) {
                    cout << (allocation.*member - handled);
                } else {
                    cout << formatBytes(allocation.*member - handled);
                }
                cout << " from " << (allocation.traces.size() - subPeakLimit) << " other places\n";
            }
            cout << '\n';
        }
    }

    template <typename T, typename LabelPrinter>
    void printUnmerged(T AllocationData::*member, LabelPrinter label)
    {
        sort(allocations.begin(), allocations.end(),
             [member](const Allocation& l, const Allocation& r) { return std::abs(l.*member) > std::abs(r.*member); });
        for (size_t i = 0; i < min(peakLimit, allocations.size()); ++i) {
            const auto& allocation = allocations[i];
            if (!(allocation.*member)) {
                break;
            }
            label(allocation);
            printBacktrace(allocation.traceIndex, cout, 1);
            cout << '\n';
        }
        cout << endl;
    }

    void writeMassifHeader(const char* command)
    {
        // write massif header
        massifOut << "desc: heaptrack\n"
                  << "cmd: " << command << '\n'
                  << "time_unit: s\n";
    }

    void writeMassifSnapshot(size_t timeStamp, bool isLast)
    {
        if (!lastMassifPeak) {
            lastMassifPeak = totalCost.leaked;
            massifAllocations = allocations;
        }
        massifOut << "#-----------\n"
                  << "snapshot=" << massifSnapshotId << '\n'
                  << "#-----------\n"
                  << "time=" << (0.001 * timeStamp) << '\n'
                  << "mem_heap_B=" << lastMassifPeak << '\n'
                  << "mem_heap_extra_B=0\n"
                  << "mem_stacks_B=0\n";

        if (massifDetailedFreq && (isLast || !(massifSnapshotId % massifDetailedFreq))) {
            massifOut << "heap_tree=detailed\n";
            const size_t threshold = double(lastMassifPeak) * massifThreshold * 0.01;
            writeMassifBacktrace(massifAllocations, lastMassifPeak, threshold, IpIndex());
        } else {
            massifOut << "heap_tree=empty\n";
        }

        ++massifSnapshotId;
        lastMassifPeak = 0;
    }

    void writeMassifBacktrace(const vector<Allocation>& allocations, size_t heapSize, size_t threshold,
                              const IpIndex& location, size_t depth = 0)
    {
        int64_t skippedLeaked = 0;
        size_t numAllocs = 0;
        size_t skipped = 0;
        auto mergedAllocations = mergeAllocations(allocations);
        sort(mergedAllocations.begin(), mergedAllocations.end(),
             [](const MergedAllocation& l, const MergedAllocation& r) { return l.leaked > r.leaked; });

        const auto ip = findIp(location);

        // skip anything below main
        const bool shouldStop = isStopIndex(ip.frame.functionIndex);
        if (!shouldStop) {
            for (auto& merged : mergedAllocations) {
                if (merged.leaked < 0) {
                    // list is sorted, so we can bail out now - these entries are
                    // uninteresting for massif
                    break;
                }

                // skip items below threshold
                if (static_cast<size_t>(merged.leaked) >= threshold) {
                    ++numAllocs;
                    // skip the first level of the backtrace, otherwise we'd endlessly
                    // recurse
                    for (auto& alloc : merged.traces) {
                        alloc.traceIndex = findTrace(alloc.traceIndex).parentIndex;
                    }
                } else {
                    ++skipped;
                    skippedLeaked += merged.leaked;
                }
            }
        }

        // TODO: write inlined frames out to massif files
        printIndent(massifOut, depth, " ");
        massifOut << 'n' << (numAllocs + (skipped ? 1 : 0)) << ": " << heapSize;
        if (!depth) {
            massifOut << " (heap allocation functions) malloc/new/new[], "
                         "--alloc-fns, etc.\n";
        } else {
            massifOut << " 0x" << hex << ip.instructionPointer << dec << ": ";
            if (ip.frame.functionIndex) {
                massifOut << stringify(ip.frame.functionIndex);
            } else {
                massifOut << "???";
            }

            massifOut << " (";
            if (ip.frame.fileIndex) {
                massifOut << stringify(ip.frame.fileIndex) << ':' << ip.frame.line;
            } else if (ip.moduleIndex) {
                massifOut << stringify(ip.moduleIndex);
            } else {
                massifOut << "???";
            }
            massifOut << ")\n";
        }

        auto writeSkipped = [&] {
            if (skipped) {
                printIndent(massifOut, depth, " ");
                massifOut << " n0: " << skippedLeaked << " in " << skipped << " places, all below massif's threshold ("
                          << massifThreshold << ")\n";
                skipped = 0;
            }
        };

        if (!shouldStop) {
            for (const auto& merged : mergedAllocations) {
                if (merged.leaked > 0 && static_cast<size_t>(merged.leaked) >= threshold) {
                    if (skippedLeaked > merged.leaked) {
                        // manually inject this entry to keep the output sorted
                        writeSkipped();
                    }
                    writeMassifBacktrace(merged.traces, merged.leaked, threshold, merged.ipIndex, depth + 1);
                }
            }
            writeSkipped();
        }
    }

    void handleAllocation(const AllocationInfo& info, const AllocationInfoIndex /*index*/) override
    {
        if (printHistogram) {
            ++sizeHistogram[info.size];
        }

        if (totalCost.leaked > 0 && static_cast<size_t>(totalCost.leaked) > lastMassifPeak && massifOut.is_open()) {
            massifAllocations = allocations;
            lastMassifPeak = totalCost.leaked;
        }
    }

    void handleTimeStamp(int64_t /*oldStamp*/, int64_t newStamp, bool isFinalTimeStamp, ParsePass pass) override
    {
        if (pass != ParsePass::FirstPass) {
            return;
        }
        if (massifOut.is_open()) {
            writeMassifSnapshot(newStamp, isFinalTimeStamp);
        }
    }

    void handleDebuggee(const char* command) override
    {
        cout << "Debuggee command was: " << command << endl;
        if (massifOut.is_open()) {
            writeMassifHeader(command);
        }
    }

    bool printHistogram = false;
    bool mergeBacktraces = true;

    vector<MergedAllocation> mergedAllocations;

    std::map<uint64_t, uint64_t> sizeHistogram;

    uint64_t massifSnapshotId = 0;
    uint64_t lastMassifPeak = 0;
    vector<Allocation> massifAllocations;
    ofstream massifOut;
    double massifThreshold = 1;
    uint64_t massifDetailedFreq = 1;

    string filterBtFunction;
    size_t peakLimit = 10;
    size_t subPeakLimit = 5;
    //

    // cleanup function
    void showRemainingDays(const std::string& directory, const std::chrono::hours& max_age) {
    namespace fs = std::filesystem;
    auto now = std::chrono::system_clock::now();

    try {
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (fs::is_regular_file(entry)) {
                auto last_write_time = fs::last_write_time(entry);
                auto file_age = std::chrono::duration_cast<std::chrono::hours>(now - last_write_time);
                auto remaining_time = max_age - file_age;

                std::cout << "File: " << entry.path().filename().string();
                if (remaining_time.count() > 0) {
                    auto remaining_days = std::chrono::duration_cast<std::chrono::days>(remaining_time).count();
                    std::cout << " - Remaining days before deletion: " << remaining_days << " day(s)" << "\n";
                } else {
                    std::cout << " - Marked for deletion (already expired)\n";
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error accessing files: " << e.what() << "\n";
    }
    }

    

    // Cleanup function
    void cleanupOldFiles(const std::string& directory, const std::chrono::hours& max_age) {
        namespace fs = std::filesystem;
        auto now = std::chrono::system_clock::now();

        try {
         for (const auto& entry : fs::directory_iterator(directory)) {
             if (fs::is_regular_file(entry)) {
                 auto last_write_time = fs::last_write_time(entry);
                    auto file_age = std::chrono::duration_cast<std::chrono::hours>(now - last_write_time);
                    if (file_age > max_age) {
                    fs::remove(entry);
                    std::cout << "Deleted old file: " << entry.path() << "\n";
                    }
                }
            }
        } catch (const std::exception& e) {
         std::cerr << "Error cleaning up files: " << e.what() << "\n";
    }
    }

    //

};
}

int main(int argc, char** argv)
{
    po::options_description desc("Options", 120, 60);
    // clang-format off
    desc.add_options()
        ("file,f", po::value<string>(),
            "The heaptrack data file to print.")
        ("diff,d", po::value<string>()->default_value({}),
            "Find the differences to this file.")
        ("shorten-templates,t", po::value<bool>()->default_value(true)->implicit_value(true),
            "Shorten template identifiers.")
        ("merge-backtraces,m", po::value<bool>()->default_value(true)->implicit_value(true),
            "Merge backtraces.\nNOTE: the merged peak consumption is not correct.")
        ("print-peaks,p", po::value<bool>()->default_value(true)->implicit_value(true),
            "Print backtraces to top allocators, sorted by peak consumption.")
        ("print-allocators,a", po::value<bool>()->default_value(true)->implicit_value(true),
            "Print backtraces to top allocators, sorted by number of calls to allocation functions.")
        ("print-temporary,T", po::value<bool>()->default_value(true)->implicit_value(true),
            "Print backtraces to top allocators, sorted by number of temporary allocations.")
        ("print-leaks,l", po::value<bool>()->default_value(false)->implicit_value(true),
            "Print backtraces to leaked memory allocations.")
        ("peak-limit,n", po::value<size_t>()->default_value(10)->implicit_value(10),
            "Limit the number of reported peaks.")
        ("sub-peak-limit,s", po::value<size_t>()->default_value(5)->implicit_value(5),
            "Limit the number of reported backtraces of merged peak locations.")
        ("print-histogram,H", po::value<string>()->default_value(string()),
            "Path to output file where an allocation size histogram will be written to.")
        ("flamegraph-cost-type", po::value<CostType>()->default_value(Allocations),
            "The cost type to use when generating a flamegraph. Possible options are:\n"
            "  - allocations: number of allocations\n"
            "  - temporary: number of temporary allocations\n"
            "  - leaked: bytes not deallocated at the end\n"
            "  - peak: bytes consumed at highest total memory consumption")
        ("print-flamegraph,F", po::value<string>()->default_value(string()),
            "Path to output file where a flame-graph compatible stack file will be written to.\n"
            "To visualize the resulting file, use flamegraph.pl from "
            "https://github.com/brendangregg/FlameGraph:\n"
            "  heaptrack_print heaptrack.someapp.PID.gz -F stacks.txt\n"
            "  # optionally pass --reverse to flamegraph.pl\n"
            "  flamegraph.pl --title \"heaptrack: allocations\" --colors mem \\\n"
            "    --countname allocations < stacks.txt > heaptrack.someapp.PID.svg\n"
            "  [firefox|chromium] heaptrack.someapp.PID.svg\n")
        ("print-massif,M", po::value<string>()->default_value(string()),
            "Path to output file where a massif compatible data file will be written to.")
        ("massif-threshold", po::value<double>()->default_value(1.),
            "Percentage of current memory usage, below which allocations are aggregated into a 'below threshold' entry.\n"
            "This is only used in the massif output file so far.\n")
        ("massif-detailed-freq", po::value<size_t>()->default_value(2),
            "Frequency of detailed snapshots in the massif output file. Increase  this to reduce the file size.\n"
            "You can set the value to zero to disable detailed snapshots.\n")
        ("filter-bt-function", po::value<string>()->default_value(string()),
            "Only print allocations where the backtrace contains the given function.")
        ("suppressions", po::value<string>()->default_value(string()),
            "Load list of leak suppressions from the specified file. Specify one suppression per line, and start each line with 'leak:', i.e. use the LSAN suppression file format.")
        ("disable-embedded-suppressions",
            "Ignore suppression definitions that are embedded into the heaptrack data file. By default, heaptrack will copy the suppressions"
            "optionally defined via a `const char *__lsan_default_suppressions()` symbol in the debuggee application. These are then always "
            "applied when analyzing the data, unless this feature is explicitly disabled using this command line option.")
        ("disable-builtin-suppressions",
            "Ignore suppression definitions that are built into heaptrack. By default, heaptrack will suppress certain "
            "known leaks from common system libraries.")
        ("print-suppressions", po::value<bool>()->default_value(false)->implicit_value(true),
            "Show statistics for matched suppressions.")
        ("help,h", "Show this help message.")
        ("version,v", "Displays version information.");
    // clang-format on
    po::positional_options_description p;
    p.add("file", -1);

    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
        if (vm.count("help")) {
            cout << "heaptrack_print - analyze heaptrack data files.\n"
                 << "\n"
                 << "heaptrack is a heap memory profiler which records information\n"
                 << "about calls to heap allocation functions such as malloc, "
                    "operator new etc. pp.\n"
                 << "This print utility can then be used to analyze the generated "
                    "data files.\n\n"
                 << desc << endl;
            return 0;
        } else if (vm.count("version")) {
            cout << "heaptrack_print " << HEAPTRACK_VERSION_STRING << endl;
            return 0;
        }
        po::notify(vm);
    } catch (const po::error& error) {
        cerr << "ERROR: " << error.what() << endl << endl << desc << endl;
        return 1;
    }

    if (!vm.count("file")) {
        // NOTE: stay backwards compatible to old boost 1.41 available in RHEL 6
        //       otherwise, we could simplify this by setting the file option
        //       as ->required() using the new 1.42 boost API
        cerr << "ERROR: the option '--file' is required but missing\n\n" << desc << endl;
        return 1;
    }

    Printer data;

    const auto inputFile = vm["file"].as<string>();
    const auto diffFile = vm["diff"].as<string>();
    data.shortenTemplates = vm["shorten-templates"].as<bool>();
    data.mergeBacktraces = vm["merge-backtraces"].as<bool>();
    data.filterBtFunction = vm["filter-bt-function"].as<string>();
    data.peakLimit = vm["peak-limit"].as<size_t>();
    data.subPeakLimit = vm["sub-peak-limit"].as<size_t>();
    const string printHistogram = vm["print-histogram"].as<string>();
    data.printHistogram = !printHistogram.empty();
    const string printFlamegraph = vm["print-flamegraph"].as<string>();
    const auto flamegraphCostType = vm["flamegraph-cost-type"].as<CostType>();
    const string printMassif = vm["print-massif"].as<string>();
    if (!printMassif.empty()) {
        data.massifOut.open(printMassif, ios_base::out);
        if (!data.massifOut.is_open()) {
            cerr << "Failed to open massif output file \"" << printMassif << "\"." << endl;
            return 1;
        }
        data.massifThreshold = vm["massif-threshold"].as<double>();
        data.massifDetailedFreq = vm["massif-detailed-freq"].as<size_t>();
    }
    const bool printLeaks = vm["print-leaks"].as<bool>();
    const bool printPeaks = vm["print-peaks"].as<bool>();
    const bool printAllocs = vm["print-allocators"].as<bool>();
    const bool printTemporary = vm["print-temporary"].as<bool>();
    const auto printSuppressions = vm["print-suppressions"].as<bool>();
    const auto suppressionsFile = vm["suppressions"].as<string>();

    data.filterParameters.disableEmbeddedSuppressions = vm.count("disable-embedded-suppressions");
    data.filterParameters.disableBuiltinSuppressions = vm.count("disable-builtin-suppressions");
    bool suppressionsOk = false;
    data.filterParameters.suppressions = parseSuppressions(suppressionsFile, &suppressionsOk);
    if (!suppressionsOk) {
        return 1;
    }

    cout << "reading file \"" << inputFile << "\" - please wait, this might take some time..." << endl;

    if (!diffFile.empty()) {
        cout << "reading diff file \"" << diffFile << "\" - please wait, this might take some time..." << endl;
        Printer diffData;
        auto diffRead = async(launch::async, [&diffData, diffFile]() { return diffData.read(diffFile, false); });

        if (!data.read(inputFile, false) || !diffRead.get()) {
            return 1;
        }

        data.diff(diffData);
    } else if (!data.read(inputFile, false)) {
        return 1;
    }

    data.finalize();

    cout << "finished reading file, now analyzing data:\n" << endl;

    if (printAllocs) {
        // sort by amount of allocations
        cout << "MOST CALLS TO ALLOCATION FUNCTIONS\n";
        data.printAllocations(
            &AllocationData::allocations,
            [](const AllocationData& data) {
                cout << data.allocations << " calls to allocation functions with " << formatBytes(data.peak)
                     << " peak consumption from\n";
            },
            [](const AllocationData& data) {
                cout << data.allocations << " calls with " << formatBytes(data.peak) << " peak consumption from:\n";
            });
        cout << endl;
    }

    if (printPeaks) {
        cout << "PEAK MEMORY CONSUMERS\n";
        data.printAllocations(
            &AllocationData::peak,
            [](const AllocationData& data) {
                cout << formatBytes(data.peak) << " peak memory consumed over " << data.allocations << " calls from\n";
            },
            [](const AllocationData& data) {
                cout << formatBytes(data.peak) << " consumed over " << data.allocations << " calls from:\n";
            });
        cout << endl;
    }

    if (printLeaks) {
        // sort by amount of leaks
        cout << "MEMORY LEAKS\n";
        data.printAllocations(
            &AllocationData::leaked,
            [](const AllocationData& data) {
                cout << formatBytes(data.leaked) << " leaked over " << data.allocations << " calls from\n";
            },
            [](const AllocationData& data) {
                cout << formatBytes(data.leaked) << " leaked over " << data.allocations << " calls from:\n";
            });
        cout << endl;
    }

    if (printTemporary) {
        // sort by amount of temporary allocations
        cout << "MOST TEMPORARY ALLOCATIONS\n";
        data.printAllocations(
            &AllocationData::temporary,
            [](const AllocationData& data) {
                cout << data.temporary << " temporary allocations of " << data.allocations << " allocations in total ("
                     << fixed << setprecision(2) << (float(data.temporary) * 100.f / data.allocations) << "%) from\n";
            },
            [](const AllocationData& data) {
                cout << data.temporary << " temporary allocations of " << data.allocations << " allocations in total ("
                     << fixed << setprecision(2) << (float(data.temporary) * 100.f / data.allocations) << "%) from:\n";
            });
        cout << endl;
    }

    const double totalTimeS = data.totalTime ? (1000. "/ data.totalTime)" : 1.;
    cout << "total runtime: " << fixed << (data.totalTime / 1000.) << "s.\n"
         << "calls to allocation functions: " << data.totalCost.allocations << " ("
         << int64_t(data.totalCost.allocations * totalTimeS) << "/s)\n"
         << "temporary memory allocations: " << data.totalCost.temporary << " ("
         << int64_t(data.totalCost.temporary * totalTimeS) << "/s)\n"
         << "peak heap memory consumption: " << formatBytes(data.totalCost.peak) << '\n'
         << "peak RSS (including heaptrack overhead): " << formatBytes(data.peakRSS * data.systemInfo.pageSize) << '\n'
         << "total memory leaked: " << formatBytes(data.totalCost.leaked) << '\n';
    //

   // Directory to clean
    const std::string tempDir = "/home/ubuntu/test_cleanup";
    // Max file age (7 days)
    const std::chrono::hours maxFileAge = std::chrono::hours(24 * 7);

    cout << "Starting directory cleanup process.\n";

    // Show remaining days for each file
    cout << "Checking remaining days for files in: " << tempDir << "\n";
    showRemainingDays(tempDir, maxFileAge);
    cout << "Finished checking remaining days.\n";

    // Cleanup old files (older than 7 days)
    cout << "Cleaning up old files in: " << tempDir << "\n";
    cleanupOldFiles(tempDir, maxFileAge);
    cout << "Cleanup complete.\n";

    cout << "DEBUG: Starting showRemainingDays\n";
    showRemainingDays(tempDir, maxFileAge);
    cout << "DEBUG: Finished showRemainingDays\n";

    cout << "DEBUG: Starting cleanupOldFiles\n";
    cleanupOldFiles(tempDir, maxFileAge);
    cout << "DEBUG: Finished cleanupOldFiles\n";

    if (std::filesystem::exists(tempDir) && !std::filesystem::is_empty(tempDir)) {
    cout << "Files found in " << tempDir << ". Starting operations.\n";
    showRemainingDays(tempDir, maxFileAge);
    cleanupOldFiles(tempDir, maxFileAge);
    } else {
     cout << "No files found in " << tempDir << ". Skipping operations.\n";
    }



    //

    if (data.totalLeakedSuppressed) {
        cout << "suppressed leaks: " << formatBytes(data.totalLeakedSuppressed) << '\n';

        if (printSuppressions) {
            cout << "Suppressions used:\n";
            cout << setw(16) << "matches" << ' ' << setw(16) << "leaked"
                 << " pattern\n";
            for (const auto& suppression : data.suppressions) {
                if (!suppression.matches) {
                    continue;
                }
                cout << setw(16) << suppression.matches << ' ' << formatBytes(suppression.leaked, 16) << ' '
                     << suppression.pattern << '\n';
            }
        }
    }

    if (!printHistogram.empty()) {
        ofstream histogram(printHistogram, ios_base::out);
        if (!histogram.is_open()) {
            cerr << "Failed to open histogram output file \"" << printHistogram << "\"." << endl;
        } else {
            for (auto entry : data.sizeHistogram) {
                histogram << entry.first << '\t' << entry.second << '\n';
            }
        }
    }

    if (!printFlamegraph.empty()) {
        ofstream flamegraph(printFlamegraph, ios_base::out);
        if (!flamegraph.is_open()) {
            cerr << "Failed to open flamegraph output file \"" << printFlamegraph << "\"." << endl;
        } else {
            for (const auto& allocation : data.allocations) {
                if (!allocation.traceIndex) {
                    flamegraph << "??";
                } else {
                    data.printFlamegraph(data.findTrace(allocation.traceIndex), flamegraph);
                }
                flamegraph << ' ';
                switch (flamegraphCostType) {
                case Allocations:
                    flamegraph << allocation.allocations;
                    break;
                case Temporary:
                    flamegraph << allocation.temporary;
                    break;
                case Peak:
                    flamegraph << allocation.peak;
                    break;
                case Leaked:
                    flamegraph << allocation.leaked;
                    break;
                }
                flamegraph << '\n';
            }
        }
    }
    

    return 0;
}



