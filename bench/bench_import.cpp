// Synchronous CSV benchmark. Paths supplied to the library are UTF-8.
#include "csvp/pipeline.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <psapi.h>
#endif

namespace {
using namespace csvp;
struct RunSnapshot {
    uint64_t imported = 0, skipped = 0, errors = 0;
    size_t detail_bytes = 0;
    bool truncated = false;
    size_t retained = 0;
    uint64_t written = 0;
    std::string error_log;
    double ms = 0;
};
struct MemoryPeak { uint64_t working_set = 0, private_commit = 0; };
MemoryPeak processMemoryPeak() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        throw std::runtime_error("GetProcessMemoryInfo failed");
    return {static_cast<uint64_t>(counters.PeakWorkingSetSize),
            static_cast<uint64_t>(counters.PeakPagefileUsage)};
#else
    return {};
#endif
}
std::vector<FieldRule> defaultRules() {
    return {{"id",FieldType::Int,true},{"name",FieldType::String,true},
            {"age",FieldType::Int,true},{"score",FieldType::Double,true},
            {"active",FieldType::Bool,false}};
}
std::vector<std::string> arguments(int argc, char* argv[]) {
#ifdef _WIN32
    (void)argc; (void)argv;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) throw std::runtime_error("Cannot read command line");
    std::vector<std::string> result;
    try {
        for (int i=0; i<count; ++i) {
            const int n = WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,nullptr,0,nullptr,nullptr);
            if (n<=0) throw std::runtime_error("Invalid Unicode command-line argument");
            std::string text(static_cast<size_t>(n),'\0');
            if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,text.data(),n,nullptr,nullptr))
                throw std::runtime_error("Cannot encode command-line argument");
            text.pop_back();
            result.push_back(std::move(text));
        }
    } catch (...) { LocalFree(wide); throw; }
    LocalFree(wide);
    return result;
#else
    return std::vector<std::string>(argv,argv+argc);
#endif
}
void validatePaths(const std::filesystem::path& input,const std::filesystem::path& report) {
    if (!std::filesystem::is_regular_file(input))
        throw std::runtime_error("Input CSV does not exist or is not a regular file");
    const auto source=std::filesystem::weakly_canonical(input);
    const auto output=std::filesystem::weakly_canonical(report);
    bool same=source==output;
#ifdef _WIN32
    same=same || CompareStringOrdinal(source.c_str(),-1,output.c_str(),-1,TRUE)==CSTR_EQUAL;
#endif
    if (same || (std::filesystem::exists(output) && std::filesystem::equivalent(source,output)))
        throw std::invalid_argument("Report path must not refer to the input CSV");
    if (!output.parent_path().empty() && !std::filesystem::is_directory(output.parent_path()))
        throw std::runtime_error("Report parent directory does not exist");
}
void writeReport(const std::filesystem::path& path,const std::string& input,int repeat,
                 const std::vector<RunSnapshot>& runs,const std::vector<RowError>& last_details,
                 double average,double rate) {
    std::ofstream out(path,std::ios::out|std::ios::trunc);
    out.exceptions(std::ios::failbit|std::ios::badbit);
    out<<"CSV Import Pipeline Benchmark Report\nFile: "<<input<<"\nRepeats: "<<repeat
       <<"\nAvg time: "<<average<<" ms\nThroughput: "<<rate<<" rows/sec\n\n"
       <<"run rows_imported rows_skipped total_error_rows retained_errors detail_bytes details_truncated time_ms archived_errors error_log\n";
    for(size_t i=0;i<runs.size();++i) {
        const auto& r=runs[i];
        out<<i+1<<' '<<r.imported<<' '<<r.skipped<<' '<<r.errors<<' '<<r.retained
           <<' '<<r.detail_bytes<<' '<<(r.truncated?"yes":"no")<<' '<<r.ms
           <<' '<<r.written<<' '<<r.error_log<<'\n';
    }
    out<<"\nError row samples (last run; bounded during import):\n";
    if(!runs.empty()) for(const auto& row:last_details) {
        out<<"line "<<row.line_number<<": "<<row.raw_line;
        if(row.details_truncated) out<<" [details truncated]";
        out<<'\n';
        for(const auto& cell:row.cell_errors)
            out<<"  ["<<cell.column_index<<' '<<cell.column_name<<"] code="<<static_cast<int>(cell.code)
               <<" value='"<<cell.raw_value<<"' "<<cell.message<<'\n';
    }
    out.close();
}
void runBench(const std::string& input,int repeat,const std::filesystem::path& report,
              const std::filesystem::path& error_log_base) {
    ImportPipeline::Config cfg;
    cfg.parser.has_header=true;
    cfg.skip_bad_rows=true;
    cfg.error_limit=200;
    cfg.max_error_bytes=4*1024*1024;
    // The supplied Part C data has five short fields. Bound physical-line growth.
    cfg.parser.max_line_bytes=1024*1024;
    std::vector<RunSnapshot> runs;
    runs.reserve(static_cast<size_t>(repeat));
    double totalMs=0;
    long double totalRows=0;
    std::vector<RowError> last_details;
    std::vector<std::filesystem::path> error_paths;
    for(int r=0;r<repeat;++r) {
        const auto path=repeat==1?error_log_base:
            error_log_base.parent_path()/std::filesystem::u8path(
                error_log_base.stem().u8string()+".run-"+std::to_string(r+1)+".jsonl");
        validatePaths(std::filesystem::u8path(input),path);
        const auto log_normal=std::filesystem::weakly_canonical(path);
        const auto report_normal=std::filesystem::weakly_canonical(report);
        bool same_output=log_normal==report_normal;
#ifdef _WIN32
        same_output=same_output ||
            CompareStringOrdinal(log_normal.c_str(),-1,report_normal.c_str(),-1,TRUE)==CSTR_EQUAL;
#endif
        if(same_output ||
           (std::filesystem::exists(path)&&std::filesystem::exists(report)&&
            std::filesystem::equivalent(path,report)))
            throw std::invalid_argument("Error log must not refer to the report file");
        if(std::filesystem::exists(path))
            throw std::invalid_argument("Error log already exists; choose a new output path");
        error_paths.push_back(path);
    }
    for(int r=0;r<repeat;++r) {
        cfg.error_log_path=error_paths[static_cast<size_t>(r)].u8string();
        ImportPipeline pipeline(cfg,defaultRules());
        const auto start=std::chrono::steady_clock::now();
        auto result=pipeline.importSync(input.c_str());
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(!result.success || result.cancelled || !result.error_log_complete ||
           result.errors_written!=result.error_count || result.rows_skipped!=result.error_count)
            throw std::runtime_error("CSV import failed or error archive is incomplete");
        totalMs+=ms; totalRows+=result.rows_imported;
        RunSnapshot snapshot;
        snapshot.imported=result.rows_imported; snapshot.skipped=result.rows_skipped;
        snapshot.errors=result.error_count; snapshot.detail_bytes=result.error_bytes;
        snapshot.truncated=result.errors_truncated; snapshot.ms=ms;
        snapshot.retained=result.errors.size();
        snapshot.written=result.errors_written;
        snapshot.error_log=result.error_log_path;
        // Keep only the last in-memory preview, even with many benchmark repeats.
        last_details=std::move(result.errors);
        std::cout<<"  Run "<<r+1<<": "<<snapshot.imported<<" rows in "<<ms<<" ms ("
                 <<snapshot.skipped<<" skipped) ["<<snapshot.errors<<" error rows, "
                 <<snapshot.retained<<" retained, "<<snapshot.written<<" archived]"<<std::endl;
        runs.push_back(std::move(snapshot));
    }
    const double average=totalMs/repeat;
    const double rate=totalMs>0?static_cast<double>(totalRows/(totalMs/1000)):0;
    std::cout<<"\n--- Summary ---\nFile: "<<input<<"\nRepeats: "<<repeat
             <<"\nAvg time: "<<average<<" ms\nThroughput: "<<rate<<" rows/sec\n";
    // Re-check now that logs exist: case/alias spellings must never let report
    // truncation overwrite a completed archive.
    validatePaths(std::filesystem::u8path(input),report);
    if(std::filesystem::exists(report)) for(const auto& error_path:error_paths)
        if(std::filesystem::equivalent(report,error_path))
            throw std::invalid_argument("Report path refers to an error archive");
    writeReport(report,input,repeat,runs,last_details,average,rate);
    std::cout<<"Report written to: "<<report.u8string()<<'\n';
    // All logs and the report are closed. These are process lifetime OS peaks.
    const auto memory=processMemoryPeak();
#ifdef _WIN32
    std::cout<<"Memory API: GetProcessMemoryInfo\nPeakWorkingSetBytes: "<<memory.working_set
             <<"\nPeakPrivateCommitBytes: "<<memory.private_commit
             <<"\nMemoryLimitBytes: 50000000\n";
    if(memory.working_set>=50000000 || memory.private_commit>=50000000)
        throw std::runtime_error("Process memory peak exceeds the 50 MB Part C limit");
#else
    (void)memory;
    std::cout<<"Memory API: unavailable; this run is not a memory-limit proof\n";
#endif
}
}
int main(int argc,char* argv[]) {
    try {
        const auto args=arguments(argc,argv);
        if(args.size()<2 || args.size()>5) {
            std::cerr<<"Usage: csvp_bench <csv_file> [repeat_count] [report_path] [error_log_path]\n";
            return 1;
        }
        const int repeat=args.size()>=3?Row::parseInt(args[2]):3;
        if(repeat<1 || repeat>10000) throw std::invalid_argument("repeat_count must be between 1 and 10000");
        const auto input=std::filesystem::u8path(args[1]);
        const auto report=std::filesystem::u8path(args.size()>=4?args[3]:"bench_report.txt");
        const auto error_log=args.size()>=5?std::filesystem::u8path(args[4]):
            report.parent_path()/std::filesystem::u8path(report.stem().u8string()+".errors.jsonl");
        validatePaths(input,report);
        std::cout<<"CSV Import Pipeline Benchmark\n=============================\n\n";
        runBench(args[1],repeat,report,error_log);
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"Benchmark failed: "<<error.what()<<'\n';
        return 1;
    }
}
