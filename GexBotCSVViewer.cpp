#include "sierrachart.h"
#include <map>
#include <vector>
#include <string>
#include <sstream>
#include <fstream> 
#include <iomanip>
#include <cmath>
#include <cfloat>

SCDLLName("GEX_CSV_VIEWER")

// =========================
//        STRUCTURES
// =========================

struct ViewerData
{
    // Historical maps
    std::map<SCDateTime, float> zeroMap;
    std::map<SCDateTime, float> posVolMap;
    std::map<SCDateTime, float> negVolMap;
    std::map<SCDateTime, float> posOiMap;
    std::map<SCDateTime, float> negOiMap;
    std::map<SCDateTime, float> netVolMap;
    std::map<SCDateTime, float> netOiMap;
    std::map<SCDateTime, float> longMap;
    std::map<SCDateTime, float> shortMap;
    std::map<SCDateTime, float> majPosMap;
    std::map<SCDateTime, float> majNegMap;
    
    // Cache State
    std::string LastBasePath;
    std::string LastTicker;
    int LastDaysCount = -1;
    SCDateTime LastUpdate;
    
    // File Tracking for Incremental Updates
    std::map<std::string, std::streampos> FileOffsets; 
};

// =========================
//        UTILITIES
// =========================

double StringToDouble(const std::string& str)
{
    if (str.empty()) return 0.0;
    try {
        return std::stod(str);
    }
    catch (...) {
        return 0.0;
    }
}

std::string FormatDateSuffix(const SCDateTime& dt)
{
    int year = dt.GetYear();
    int month = dt.GetMonth();
    int day = dt.GetDay();
    
    char buffer[20];
    snprintf(buffer, sizeof(buffer), "%02d.%02d.%04d", month, day, year);
    return buffer;
}

SCDateTime SubtractDays(SCDateTime base, int days)
{
    double adjusted = base.GetAsDouble() - days;
    return SCDateTime(adjusted);
}

bool FileExists(const std::string& path)
{
    std::wstring wPath(path.begin(), path.end());
    DWORD dwAttrib = GetFileAttributesW(wPath.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

// =========================
//     CSV PARSING
// =========================

int ParseCsvLine(const std::string& line, double* values, int maxFields)
{
    int count = 0;
    size_t start = 0;
    size_t pos = 0;

    while (pos <= line.length() && count < maxFields)
    {
        if (pos == line.length() || line[pos] == ',')
        {
            std::string field = line.substr(start, pos - start);
            
            // Trim whitespace
            size_t first = field.find_first_not_of(" \t");
            if (std::string::npos == first) {
                 field = "";
            } else {
                 size_t last = field.find_last_not_of(" \t\r\n");
                 field = field.substr(first, (last - first + 1));
            }

            values[count] = StringToDouble(field);
            count++;
            start = pos + 1;
        }
        pos++;
    }
    return count;
}

void LoadViewerCSV(const std::string& fullPath, int tzOffsetHours, ViewerData* data)
{
    // Use std::ifstream for flexible seeking
    std::ifstream file(fullPath);
    if (!file.is_open()) return;

    // Check if we have a previous offset for this file
    std::streampos offset = 0;
    if (data->FileOffsets.count(fullPath))
    {
        offset = data->FileOffsets[fullPath];
    }
    
    // Get current file size
    file.seekg(0, std::ios::end);
    std::streampos fileSize = file.tellg();
    
    // Logic: 
    // 1. If fileSize < offset, file was likely truncated/recreated -> Start from 0
    // 2. If offset == 0, read from start
    // 3. Otherwise seek to offset
    if (fileSize < offset) {
        offset = 0;
    }
    
    file.seekg(offset);

    std::string line;
    bool isFirstLine = (offset == 0);

    while (std::getline(file, line))
    {
        // Skip empty lines or header if we are starting from 0
        if (line.empty()) continue;
        
        // Remove potential carriage return
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (isFirstLine)
        {
            isFirstLine = false; 
            // Simple check for header: if it contains "timestamp" or starts with non-digit (and not a minus sign)
            if (line.find("timestamp") != std::string::npos || (!line.empty() && !isdigit(line[0]) && line[0] != '-'))
                continue;
        }

        double values[15] = {0};
        int fieldCount = ParseCsvLine(line, values, 15);
        if (fieldCount < 5) continue;

        double ts = values[0];
        if (ts <= 0) continue;

        SCDateTime dt((ts + tzOffsetHours * 3600.0) / 86400.0 + 25569.0);

        double spot  = values[1];
        double zero  = values[2];
        double posv  = values[3];
        double negv  = values[4];
        double posoi = (fieldCount > 5) ? values[5] : 0;
        double negoi = (fieldCount > 6) ? values[6] : 0;
        double lng   = (fieldCount > 10) ? values[10] : 0;
        double sht   = (fieldCount > 11) ? values[11] : 0;
        double mjp   = (fieldCount > 12) ? values[12] : 0;
        double mjn   = (fieldCount > 13) ? values[13] : 0;
        double netv  = (fieldCount > 7)  ? values[7] : 0;
        double netoi = (fieldCount > 8)  ? values[8] : 0;
        
        if (!std::isnan(zero) && zero != 0) { data->zeroMap[dt] = (float)zero; }
        if (!std::isnan(posv) && posv != 0) { data->posVolMap[dt] = (float)posv; }
        if (!std::isnan(negv) && negv != 0) { data->negVolMap[dt] = (float)negv; }
        if (!std::isnan(posoi) && posoi != 0) { data->posOiMap[dt] = (float)posoi; }
        if (!std::isnan(negoi) && negoi != 0) { data->negOiMap[dt] = (float)negoi; }
        if (!std::isnan(lng) && lng != 0) { data->longMap[dt] = (float)lng; }
        if (!std::isnan(sht) && sht != 0) { data->shortMap[dt] = (float)sht; }
        if (!std::isnan(mjp) && mjp != 0) { data->majPosMap[dt] = (float)mjp; }
        if (!std::isnan(mjn) && mjn != 0) { data->majNegMap[dt] = (float)mjn; }
        if (!std::isnan(netv) && netv != 0) { data->netVolMap[dt] = (float)netv; }
        if (!std::isnan(netoi) && netoi != 0) { data->netOiMap[dt] = (float)netoi; }
        
    }
    
    // Save new offset
    if (!file.eof() && !file.fail()) {
        data->FileOffsets[fullPath] = file.tellg();
    } else {
        // If we hit EOF (which getline does), clear flags and get position
        file.clear();
        data->FileOffsets[fullPath] = file.tellg();
    }
}

void ReloadFiles(SCStudyInterfaceRef sc, ViewerData* data, const std::string& baseFolder, const std::string& ticker, int days, int tzOffset)
{
    // If config changed, clear everything
    if (baseFolder != data->LastBasePath || ticker != data->LastTicker || days != data->LastDaysCount)
    {
        data->zeroMap.clear();
        data->posVolMap.clear();
        data->negVolMap.clear();
        data->posOiMap.clear();
        data->negOiMap.clear();
        data->longMap.clear();
        data->shortMap.clear();
        data->majPosMap.clear();
        data->majNegMap.clear();
        data->netVolMap.clear();
        data->netOiMap.clear();
        
        data->FileOffsets.clear(); // Reset offsets so we re-read revised files from scratch
        
        data->LastBasePath = baseFolder;
        data->LastTicker = ticker;
        data->LastDaysCount = days;
    }

    SCDateTime reference = sc.GetCurrentDateTime();
    
    // Iterate from oldest to newest day
    for (int i = days - 1; i >= 0; --i)
    {
        SCDateTime date = SubtractDays(reference, i);
        std::string suffix = FormatDateSuffix(date);
        std::string path = baseFolder + "\\Tickers " + suffix + "\\" + ticker + ".csv";
        
        if (FileExists(path))
        {
            // For older files (i > 0), simple optimization:
            // If we have already read this file to the end (offset > 0), we assume it doesn't change 
            // (historical days are closed). 
            // However, to be safe, we just let the incremental reader check. 
            // It will see size == offset and return immediately.
            LoadViewerCSV(path, tzOffset, data);
        }
    }
}

float GetValue(const std::map<SCDateTime, float>& map, SCDateTime targetTime)
{
    // Time Filter: Don't draw outside 09:30 - 16:00
    // This prevents flat lines extending overnight
    int time = targetTime.GetTime();
    if (time < HMS_TIME(9, 30, 0) || time > HMS_TIME(16, 0, 0))
        return -FLT_MAX;

    if (map.empty()) return -FLT_MAX;
    auto it = map.upper_bound(targetTime);
    if (it == map.begin()) return -FLT_MAX;
    --it;
    
    // Check if the data point is from a previous day (don't connect yesterday's close to today's open)
    if (it->first.GetDate() != targetTime.GetDate())
        return -FLT_MAX;

    // Forward fill indefinitely within the same day
    return it->second;
}


// =========================
//      MAIN STUDY
// =========================

SCSFExport scsf_GexBotCSVViewer(SCStudyInterfaceRef sc)
{
    // Mapped strictly to User Requirements (SG1 - SG9)
    SCSubgraphRef SG1_CallVol = sc.Subgraph[0];
    SCSubgraphRef SG2_PutVol = sc.Subgraph[1];
    SCSubgraphRef SG3_Zero = sc.Subgraph[2];
    SCSubgraphRef SG4_CallOI = sc.Subgraph[3];
    SCSubgraphRef SG5_PutOI = sc.Subgraph[4];
    SCSubgraphRef SG6_Long = sc.Subgraph[5];
    SCSubgraphRef SG7_Short = sc.Subgraph[6];
    SCSubgraphRef SG8_NetVol = sc.Subgraph[7];
    SCSubgraphRef SG9_NetOI = sc.Subgraph[8];

    SCInputRef TickerInput = sc.Input[0];
    SCInputRef CsvPathInput = sc.Input[1];
    SCInputRef RefreshInterval = sc.Input[2];
    SCInputRef DaysToLoad = sc.Input[3];
    SCInputRef TZOffset = sc.Input[4];

    if (sc.SetDefaults)
    {
        sc.GraphName = "GexBot CSV Viewer";
        sc.AutoLoop = 1; 
        sc.GraphRegion = 0;
        sc.ValueFormat = 2;

        SG1_CallVol.Name = "Major Call Gamma (Vol)"; SG1_CallVol.DrawStyle = DRAWSTYLE_DASH; SG1_CallVol.PrimaryColor = RGB(0, 159, 0); SG1_CallVol.LineWidth = 1;
        SG2_PutVol.Name = "Major Put Gamma (Vol)"; SG2_PutVol.DrawStyle = DRAWSTYLE_DASH; SG2_PutVol.PrimaryColor = RGB(174, 0, 0); SG2_PutVol.LineWidth = 1;
        SG3_Zero.Name = "Zero Gamma"; SG3_Zero.DrawStyle = DRAWSTYLE_DASH; SG3_Zero.PrimaryColor = RGB(252, 177, 3); SG3_Zero.LineWidth = 1;
        
        SG4_CallOI.Name = "Major Call Gamma (OI)"; SG4_CallOI.DrawStyle = DRAWSTYLE_DASH; SG4_CallOI.PrimaryColor = RGB(0, 255, 255); SG4_CallOI.LineWidth = 1;
        SG5_PutOI.Name = "Major Put Gamma (OI)"; SG5_PutOI.DrawStyle = DRAWSTYLE_DASH; SG5_PutOI.PrimaryColor = RGB(255, 165, 0); SG5_PutOI.LineWidth = 1;
        
        // Hidden by default - requires State package (shows 0 for Classic users, breaks Y-axis)
        SG6_Long.Name = "Major Long Gamma (State)"; SG6_Long.DrawStyle = DRAWSTYLE_HIDDEN; SG6_Long.PrimaryColor = RGB(0, 255, 255); SG6_Long.LineWidth = 2;
        SG7_Short.Name = "Major Short Gamma (State)"; SG7_Short.DrawStyle = DRAWSTYLE_HIDDEN; SG7_Short.PrimaryColor = RGB(174, 74, 213); SG7_Short.LineWidth = 2;
        
        // Hidden by default - different scale than price levels
        SG8_NetVol.Name = "Net GEX (Vol)"; SG8_NetVol.DrawStyle = DRAWSTYLE_HIDDEN; SG8_NetVol.PrimaryColor = RGB(200, 200, 200);
        SG9_NetOI.Name = "Net GEX (OI)"; SG9_NetOI.DrawStyle = DRAWSTYLE_HIDDEN; SG9_NetOI.PrimaryColor = RGB(150, 150, 150);
        
        TickerInput.Name = "Ticker"; TickerInput.SetString("ES_SPX");
        CsvPathInput.Name = "Local CSV Path"; CsvPathInput.SetString("C:\\GexBot\\Data");
        RefreshInterval.Name = "Refresh Interval (sec)"; RefreshInterval.SetInt(10);
        DaysToLoad.Name = "Days to Load"; DaysToLoad.SetInt(2);
        TZOffset.Name = "UTC Offset (hours)"; TZOffset.SetInt(0);

        return;
    }

    // Persistence
    ViewerData* data = (ViewerData*)sc.GetPersistentPointer(1);
    if (!data && !sc.LastCallToFunction)
    {
        data = new ViewerData();
        sc.SetPersistentPointer(1, data);
    }
    if (sc.LastCallToFunction)
    {
        if (data) { delete data; sc.SetPersistentPointer(1, nullptr); }
        return;
    }

    // Refresh Logic (Last Bar Only)
    // We try to reload data periodically
    if (sc.Index == sc.ArraySize - 1)
    {
        double timeSinceLast = (sc.CurrentSystemDateTime - data->LastUpdate).GetAsDouble() * 86400.0;
        
        // Initial load or periodic refresh
        if (data->LastUpdate.GetAsDouble() == 0.0 || timeSinceLast >= RefreshInterval.GetInt())
        {
            ReloadFiles(sc, data, CsvPathInput.GetString(), TickerInput.GetString(), DaysToLoad.GetInt(), TZOffset.GetInt());
            data->LastUpdate = sc.CurrentSystemDateTime;
        }
    }

    // Render Logic
    SCDateTime now = sc.BaseDateTimeIn[sc.Index];

    float vZero = GetValue(data->zeroMap, now);
    float vPosVol = GetValue(data->posVolMap, now);
    float vNegVol = GetValue(data->negVolMap, now);
    float vPosOi = GetValue(data->posOiMap, now);
    float vNegOi = GetValue(data->negOiMap, now);
    float vLong = GetValue(data->longMap, now);
    float vShort = GetValue(data->shortMap, now);
    float vNetVol = GetValue(data->netVolMap, now); 
    float vNetOi = GetValue(data->netOiMap, now);
    
    if (vPosVol != -FLT_MAX) SG1_CallVol[sc.Index] = vPosVol;
    if (vNegVol != -FLT_MAX) SG2_PutVol[sc.Index] = vNegVol;
    if (vZero != -FLT_MAX) SG3_Zero[sc.Index] = vZero;
    if (vPosOi != -FLT_MAX) SG4_CallOI[sc.Index] = vPosOi;
    if (vNegOi != -FLT_MAX) SG5_PutOI[sc.Index] = vNegOi;
    if (vLong != -FLT_MAX) SG6_Long[sc.Index] = vLong;
    if (vShort != -FLT_MAX) SG7_Short[sc.Index] = vShort;
    if (vNetVol != -FLT_MAX) SG8_NetVol[sc.Index] = vNetVol;
    if (vNetOi != -FLT_MAX) SG9_NetOI[sc.Index] = vNetOi;
    
}
