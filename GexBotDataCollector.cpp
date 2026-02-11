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
    SCInputRef DebugLoggingInput = sc.Input[7];
    
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

        DebugLoggingInput.Name = "Enable Debug Logging";
        DebugLoggingInput.SetYesNo(0);

        return;
    }

    // Only process on the very last bar to avoid rewriting history during full recalcs
    // unless we strictly want to log historical bars. 
    // Given the request "collecting data... stop recording after hours", this implies real-time logging.
    if (sc.Index < sc.ArraySize - 1) 
        return;

    // Time Filter - Use REAL TIME for filtering and timestamps
    SCDateTime CurrentDateTime = sc.CurrentSystemDateTime;
    int CurrentTime = CurrentDateTime.GetTime();    
    int StartTime = StartTimeInput.GetTime();
    int EndTime = EndTimeInput.GetTime();

    if (CurrentTime < StartTime || CurrentTime > EndTime)
    {
        // Debug: Log why we are skipping (only once per minute to avoid spam)
        if (CurrentDateTime.GetSecond() == 0) 
        {
             SCString msg;
             msg.Format("GexCollector: Outside trading hours. Current=%d, Start=%d, End=%d", 
                 CurrentTime, StartTime, EndTime);
             // sc.AddMessageToLog(msg, 0); // Uncomment to debug
        }
        return;
    }
    
    // Debug: Log when we ARE recording late in the day (after 15:55)
    if (CurrentTime > HMS_TIME(15, 55, 0))
    {
         SCString msg;
         msg.Format("GexCollector: Recording active! Current=%d, EndLimit=%d", CurrentTime, EndTime);
         sc.AddMessageToLog(msg, 0);
    }

    // Day Filter: Exclude Weekends (Sunday=0, Saturday=6)
    int dayOfWeek = CurrentDateTime.GetDayOfWeek();
    if (dayOfWeek == SATURDAY || dayOfWeek == SUNDAY)
        return;

    // Rate Limiting (Throttle writes)
    SCDateTime LastWrite = SCDateTime(0.0);
    // We'll use a persistent int for "Last Write Time" to avoid string parsing overhead
    int& LastWriteTimeSec = sc.GetPersistentInt(2);
    int NowSec = CurrentDateTime.GetTimeInSeconds();
    
    if (NowSec - LastWriteTimeSec < WriteIntervalSecInput.GetInt())
        return;

    // Access Source Arrays
    int ChartID = ChartIDInput.GetInt();
    int StudyID = StudyIDInput.GetInt();
    if (ChartID == 0) ChartID = sc.ChartNumber;
    bool debugLog = DebugLoggingInput.GetYesNo() != 0;

    // =============================================================
    // ACTUAL Subgraph Mapping from GexBotTerminalAPI.cpp (scsf_GexBotAPI)
    // The source DLL defines subgraphs in THIS order:
    //   [0]  LongGamma        = Greeks.major_long_gamma
    //   [1]  ShortGamma       = Greeks.major_short_gamma
    //   [2]  MajorPositive    = Greeks.major_positive
    //   [3]  MajorNegative    = Greeks.major_negative
    //   [4]  Zero             = ProfileMeta.zero_gamma / Majors.zero_gamma
    //   [5]  MajorPosVol      = Majors.mpos_vol   (Call Wall by Volume)
    //   [6]  MajorNegVol      = Majors.mneg_vol   (Put Wall by Volume)
    //   [7]  MajorPosOi       = Majors.mpos_oi    (Call Wall by OI)
    //   [8]  MajorNegOi       = Majors.mneg_oi    (Put Wall by OI)
    //   [9]  GreekMajorPos    = Greeks.major_positive (duplicate of [2])
    //   [10] GreekMajorNeg    = Greeks.major_negative (duplicate of [3])
    //   [11] MajorLongGamma   = Greeks.major_long_gamma (duplicate of [0])
    //   [12] MajorShortGamma  = Greeks.major_short_gamma (duplicate of [1])
    // =============================================================

    SCFloatArray SG_LongGamma, SG_ShortGamma, SG_MajorPos, SG_MajorNeg;
    SCFloatArray SG_Zero, SG_CallVol, SG_PutVol, SG_CallOI, SG_PutOI;
    
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 0, SG_LongGamma);   // [0] Long Gamma
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 1, SG_ShortGamma);  // [1] Short Gamma
    // [2] MajorPositive - skip (Greek, not needed in CSV)
    // [3] MajorNegative - skip (Greek, not needed in CSV)
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 4, SG_Zero);        // [4] Zero Gamma
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 5, SG_CallVol);     // [5] Call Wall Vol
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 6, SG_PutVol);      // [6] Put Wall Vol
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 7, SG_CallOI);      // [7] Call Wall OI
    sc.GetStudyArrayFromChartUsingID(ChartID, StudyID, 8, SG_PutOI);       // [8] Put Wall OI

    // Reading values at current index
    // Use ArraySize-1 to get the latest value from the source study regardless of alignment
    int SrcIndex = SG_CallVol.GetArraySize() - 1;
    if (SrcIndex < 0) return;

    double CallVol  = SG_CallVol[SrcIndex];
    double PutVol   = SG_PutVol[SrcIndex];
    double Zero     = SG_Zero[SrcIndex];
    double CallOI   = SG_CallOI[SrcIndex];
    double PutOI    = SG_PutOI[SrcIndex];
    double Long     = SG_LongGamma[SrcIndex];
    double Short    = SG_ShortGamma[SrcIndex];
    // Net GEX is not a direct subgraph from the API study — it's in the CSV only
    // The API study writes net values via UpdateMapsAndWriteCSV, not via subgraph
    // So we skip NetVol/NetOI here — the Collector should only record what is visible as SGs
    double NetVol = 0.0;  // Not available as a subgraph
    double NetOI  = 0.0;  // Not available as a subgraph
    
    // Get Spot Price - Try SC_LAST (Last Trade) first, fallback to SC_OPEN if invalid
    double Spot = sc.BaseData[SC_LAST][sc.Index];
    
    // Fallback chain for abnormal chart types
    if (Spot < 500.0 && sc.BaseData[SC_OPEN][sc.Index] > 500.0)
        Spot = sc.BaseData[SC_OPEN][sc.Index];
    if (Spot < 500.0 && sc.BaseData[SC_HIGH][sc.Index] > 500.0)
        Spot = sc.BaseData[SC_HIGH][sc.Index];

    // Debug logging (controlled by input toggle)
    if (Spot < 500.0) 
    {
        SCString debugMsg;
        debugMsg.Format("GexCollector WARNING: Suspicious Spot=%.2f Open=%.2f High=%.2f Close=%.2f",
            Spot, sc.BaseData[SC_OPEN][sc.Index], sc.BaseData[SC_HIGH][sc.Index], sc.BaseData[SC_CLOSE][sc.Index]);
        sc.AddMessageToLog(debugMsg, 1);
    }
    else if (debugLog)
    {
         SCString debugMsg;
         debugMsg.Format("GexCollector: Spot=%.2f Z=%.2f CallV=%.2f Long=%.2f", Spot, Zero, CallVol, Long);
         sc.AddMessageToLog(debugMsg, 0);
    }

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
        const char* header = "timestamp,spot,zero_gamma,major_pos_vol,major_neg_vol,major_pos_oi,major_neg_oi,sum_gex_vol,sum_gex_oi,delta_risk_reversal,major_long_gamma,major_short_gamma,major_positive,major_negative,net\r\n";
        sc.WriteFile(fileHandle, header, (int)strlen(header), &bytesWritten);
    }

    // Format Line
    // Unix Timestamp
    double unixTs = (CurrentDateTime.GetAsDouble() - 25569.0) * 86400.0;
    
    // Write match for GexBotTerminalAPI/Viewer format:
    // timestamp, spot, zero, pos_vol, neg_vol, pos_oi, neg_oi, net_vol, net_oi, delta_rr, long, short, maj_pos, maj_neg, net
    // We only have SGs for the first set. We will fill others with 0.
    
    char line[512];
    snprintf(line, sizeof(line), "%.1f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\r\n",
        unixTs, Spot, Zero, CallVol, PutVol, CallOI, PutOI, NetVol, NetOI, 0.0, Long, Short, 0.0, 0.0, 0.0);

    sc.WriteFile(fileHandle, line, (int)strlen(line), &bytesWritten);
    sc.CloseFile(fileHandle);

    LastWriteTimeSec = NowSec;
}
