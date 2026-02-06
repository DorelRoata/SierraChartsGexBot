#include "sierrachart.h"
#include <string>
#include <sstream>
#include <vector>
#include <iomanip>
#include <windows.h>

SCDLLName("GEX_DATA_COLLECTOR")

// =========================
//     FILE I/O HELPERS
// =========================

void EnsureDirectoryExists(const std::string& filePath)
{
    size_t lastSlash = filePath.find_last_of("\\/");
    if (lastSlash == std::string::npos) return;

    std::string dirPath = filePath.substr(0, lastSlash);
    if (dirPath.empty()) return;

    std::wstring wDirPath(dirPath.begin(), dirPath.end());
    DWORD dwAttrib = GetFileAttributesW(wDirPath.c_str());
    if (dwAttrib != INVALID_FILE_ATTRIBUTES) return; 

    size_t pos = 0;
    while ((pos = dirPath.find_first_of("\\/", pos + 1)) != std::string::npos)
    {
        std::string subDir = dirPath.substr(0, pos);
        std::wstring wSubDir(subDir.begin(), subDir.end());
        CreateDirectoryW(wSubDir.c_str(), NULL);
    }
    std::wstring wFullDir(dirPath.begin(), dirPath.end());
    CreateDirectoryW(wFullDir.c_str(), NULL);
}

bool FileExists(const std::string& path)
{
    std::wstring wPath(path.begin(), path.end());
    DWORD dwAttrib = GetFileAttributesW(wPath.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

// =========================
//      MAIN STUDY
// =========================

SCSFExport scsf_GexBotDataCollector(SCStudyInterfaceRef sc)
{
    // Inputs
    SCInputRef ChartIDInput = sc.Input[0];
    SCInputRef StudyIDInput = sc.Input[1];
    SCInputRef TickerInput = sc.Input[2];
    SCInputRef OutputPathInput = sc.Input[3];
    SCInputRef StartTimeInput = sc.Input[4];
    SCInputRef EndTimeInput = sc.Input[5];
    SCInputRef WriteIntervalSecInput = sc.Input[6];
    
    // Internal Persistence
    SCString& LastLogTime = sc.GetPersistentSCString(1);

    if (sc.SetDefaults)
    {
        sc.GraphName = "GexBot Data Collector";
        sc.AutoLoop = 1; 
        sc.GraphRegion = 0;
        
        ChartIDInput.Name = "Source Chart ID (0 for this chart)";
        ChartIDInput.SetInt(0);

        StudyIDInput.Name = "Source Study ID";
        StudyIDInput.SetInt(0);

        TickerInput.Name = "Ticker Name (for filename)";
        TickerInput.SetString("ES_SPX");

        OutputPathInput.Name = "Output Directory";
        OutputPathInput.SetString("C:\\GexBot\\Data");

        StartTimeInput.Name = "Market Start Time";
        StartTimeInput.SetTime(HMS_TIME(9, 30, 0));

        EndTimeInput.Name = "Market End Time";
        EndTimeInput.SetTime(HMS_TIME(16, 0, 0));
        
        WriteIntervalSecInput.Name = "Write Interval (Seconds)";
        WriteIntervalSecInput.SetInt(10);

        return;
    }

    // Only process on the very last bar to avoid rewriting history during full recalcs
    // unless we strictly want to log historical bars. 
    // Given the request "collecting data... stop recording after hours", this implies real-time logging.
    if (sc.Index < sc.ArraySize - 1) 
        return;

    // Time Filter
    SCDateTime CurrentDateTime = sc.BaseDateTimeIn[sc.Index];
    int CurrentTime = CurrentDateTime.GetTime();    
    int StartTime = StartTimeInput.GetTime();
    int EndTime = EndTimeInput.GetTime();

    if (CurrentTime < StartTime || CurrentTime > EndTime)
        return;

    // Day Filter: Exclude Weekends (Sunday=0, Saturday=6)
    int dayOfWeek = CurrentDateTime.GetDayOfWeek();
    if (dayOfWeek == DA_SATURDAY || dayOfWeek == DA_SUNDAY)
        return;

    // Rate Limiting (Throttle writes)
    SCDateTime LastWrite = SCDateTime(0.0);
    if (LastLogTime.GetLength() > 0)
    {
        // Simple string parsing or keep double in persistent memory
        // Using persistent vars simplifies things
         // We will just use the SCDateTime comparison logic
    }
    
    // We'll use a persistent int for "Last Write Time" to avoid string parsing overhead
    int& LastWriteTimeSec = sc.GetPersistentInt(2);
    int NowSec = CurrentDateTime.GetTimeInSeconds();
    
    if (NowSec - LastWriteTimeSec < WriteIntervalSecInput.GetInt())
        return;

    // Access Source Arrays
    int ChartID = ChartIDInput.GetInt();
    int StudyID = StudyIDInput.GetInt();
    if (ChartID == 0) ChartID = sc.ChartNumber;

    // Mapped via "Proposed Subgraph Mapping" in README
    // SG1 -> Index 0: Major Call Gamma by Volume
    // SG2 -> Index 1: Major Put Gamma by Volume
    // SG3 -> Index 2: Zero Gamma
    // SG4 -> Index 3: Major Call Gamma by OI
    // SG5 -> Index 4: Major Put Gamma by OI
    // SG6 -> Index 5: Major Long Gamma
    // SG7 -> Index 6: Major Short Gamma
    // SG8 -> Index 7: Net GEX Volume
    // SG9 -> Index 8: Net GEX OI

    SCFloatArray SG_CallVol, SG_PutVol, SG_Zero, SG_CallOI, SG_PutOI, SG_Long, SG_Short, SG_NetVol, SG_NetOI;
    
    // Get Arrays
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 0, SG_CallVol);
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 1, SG_PutVol);
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 2, SG_Zero);
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 3, SG_CallOI);
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 4, SG_PutOI);
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 5, SG_Long);
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 6, SG_Short);
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 7, SG_NetVol);
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 8, SG_NetOI);

    // Reading values at current index
    // Note: The source study array must be aligned with this chart's index or we grab the last value.
    // Use ArraySize-1 to get the latest value from the source study regardless of alignment
    int SrcIndex = SG_CallVol.GetArraySize() - 1;
    if (SrcIndex < 0) return;

    float vMakeFloat(float v) { return v; } // Helper if needed, but direct access is float

    double CallVol = SG_CallVol[SrcIndex];
    double PutVol = SG_PutVol[SrcIndex];
    double Zero = SG_Zero[SrcIndex];
    double CallOI = SG_CallOI[SrcIndex];
    double PutOI = SG_PutOI[SrcIndex];
    double Long = SG_Long[SrcIndex];
    double Short = SG_Short[SrcIndex];
    double NetVol = SG_NetVol[SrcIndex];
    double NetOI = SG_NetOI[SrcIndex];
    
    double Spot = sc.BaseData[SC_CLOSE][sc.Index];

    // CSV Output
    std::string ticker = TickerInput.GetString();
    std::string dir = OutputPathInput.GetString();
    if (ticker.empty() || dir.empty()) return;

    // Filename: Tickers MM.DD.YYYY/Ticker.csv
    SCDateTime Today = sc.CurrentSystemDateTime;
    int year = Today.GetYear();
    int month = Today.GetMonth();
    int day = Today.GetDay();
    char dateBuf[64];
    snprintf(dateBuf, sizeof(dateBuf), "Tickers %02d.%02d.%04d", month, day, year);
    
    std::string fullDir = dir + "\\" + dateBuf;
    std::string filePath = fullDir + "\\" + ticker + ".csv";

    EnsureDirectoryExists(filePath);
    
    bool needsHeader = !FileExists(filePath);
    int fileHandle = 0;
    SCString scPath(filePath.c_str());
    
    if (!sc.OpenFile(scPath, n_ACSIL::FILE_MODE_OPEN_TO_APPEND, fileHandle))
    {
        if (!sc.OpenFile(scPath, n_ACSIL::FILE_MODE_CREATE_AND_OPEN_FOR_READ_WRITE, fileHandle))
            return;
        needsHeader = true;
    }

    unsigned int bytesWritten = 0;
    if (needsHeader)
    {
        const char* header = "timestamp,spot,zero_gamma,call_vol,put_vol,call_oi,put_oi,net_vol,net_oi,long_gamma,short_gamma\r\n";
        sc.WriteFile(fileHandle, header, (int)strlen(header), &bytesWritten);
    }

    // Format Line
    // Unix Timestamp
    double unixTs = (CurrentDateTime.GetAsDouble() - 25569.0) * 86400.0;
    
    char line[512];
    snprintf(line, sizeof(line), "%.1f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\r\n",
        unixTs, Spot, Zero, CallVol, PutVol, CallOI, PutOI, NetVol, NetOI, Long, Short);

    sc.WriteFile(fileHandle, line, (int)strlen(line), &bytesWritten);
    sc.CloseFile(fileHandle);

    LastWriteTimeSec = NowSec;
}
