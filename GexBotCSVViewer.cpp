#include "sierrachart.h"
#include <map>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cfloat>

SCDLLName("GEX_CSV_VIEWER")

// =========================
//        STRUCTURES
// =========================

struct ViewerData
{
    // Historical maps for forward fill - using SCDateTime directly
    std::map<SCDateTime, float> zeroMap;
    std::map<SCDateTime, float> posVolMap;
    std::map<SCDateTime, float> negVolMap;
    std::map<SCDateTime, float> posOiMap;
    std::map<SCDateTime, float> negOiMap;
    std::map<SCDateTime, float> netMap;
    std::map<SCDateTime, float> longMap;
    std::map<SCDateTime, float> shortMap;
    std::map<SCDateTime, float> majPosMap;
    std::map<SCDateTime, float> majNegMap;
    
    // Cache State
    std::string LastBasePath;
    std::string LastTicker;
    int LastDaysCount = -1;
    SCDateTime LastUpdate;
    std::set<std::string> CachedFilePaths;
    
    // Last known values for forward filling
    float lastZero = 0;
    float lastPosVol = 0;
    float lastNegVol = 0;
    float lastPosOi = 0;
    float lastNegOi = 0;
    float lastLong = 0;
    float lastShort = 0;
    float lastMajPos = 0;
    float lastMajNeg = 0;
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
            while (!field.empty() && (field.front() == ' ' || field.front() == '\t')) field.erase(0, 1);
            while (!field.empty() && (field.back() == ' ' || field.back() == '\r' || field.back() == '\n')) field.pop_back();

            values[count] = StringToDouble(field);
            count++;
            start = pos + 1;
        }
        pos++;
    }
    return count;
}

int LoadViewerCSV(SCStudyInterfaceRef sc, const std::string& fullPath, int tzOffsetHours, ViewerData* data)
{
    int rowCount = 0;
    int fileHandle = 0;
    SCString scPath(fullPath.c_str());
    
    // Open for reading (shared access)
    if (!sc.OpenFile(scPath, n_ACSIL::FILE_MODE_OPEN_EXISTING_FOR_SEQUENTIAL_READING, fileHandle))
        return 0;

    const int BUFFER_SIZE = 256 * 1024; 
    char* buffer = new char[BUFFER_SIZE];
    unsigned int bytesRead = 0;
    int totalRead = 0;

    while (sc.ReadFile(fileHandle, buffer + totalRead, BUFFER_SIZE - totalRead - 1, &bytesRead) && bytesRead > 0)
    {
        totalRead += bytesRead;
        if (totalRead >= BUFFER_SIZE - 1) break;
        bytesRead = 0;
    }
    buffer[totalRead] = '\0';
    sc.CloseFile(fileHandle);

    if (totalRead == 0)
    {
        delete[] buffer;
        return 0;
    }

    std::string content(buffer, totalRead);
    delete[] buffer;

    size_t lineStart = 0;
    bool isFirstLine = true;

    while (lineStart < content.length())
    {
        size_t lineEnd = content.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = content.length();

        std::string line = content.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;

        if (isFirstLine)
        {
            isFirstLine = false;
            if (line.find("timestamp") != std::string::npos || (!line.empty() && !isdigit(line[0]) && line[0] != '-'))
                continue;
        }

        // 0:timestamp, 1:spot, 2:zero_gamma, 3:major_pos_vol, 4:major_neg_vol,
        // 5:major_pos_oi, 6:major_neg_oi, 7:sum_gex_vol, 8:sum_gex_oi,
        // 9:delta_risk_reversal, 10:major_long_gamma, 11:major_short_gamma,
        // 12:major_positive, 13:major_negative, 14:net
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

        if (!std::isnan(zero) && zero != 0) { data->zeroMap[dt] = (float)zero; data->lastZero = (float)zero; }
        if (!std::isnan(posv) && posv != 0) { data->posVolMap[dt] = (float)posv; data->lastPosVol = (float)posv; }
        if (!std::isnan(negv) && negv != 0) { data->negVolMap[dt] = (float)negv; data->lastNegVol = (float)negv; }
        if (!std::isnan(posoi) && posoi != 0) { data->posOiMap[dt] = (float)posoi; data->lastPosOi = (float)posoi; }
        if (!std::isnan(negoi) && negoi != 0) { data->negOiMap[dt] = (float)negoi; data->lastNegOi = (float)negoi; }
        if (!std::isnan(lng) && lng != 0) { data->longMap[dt] = (float)lng; data->lastLong = (float)lng; }
        if (!std::isnan(sht) && sht != 0) { data->shortMap[dt] = (float)sht; data->lastShort = (float)sht; }
        if (!std::isnan(mjp) && mjp != 0) { data->majPosMap[dt] = (float)mjp; data->lastMajPos = (float)mjp; }
        if (!std::isnan(mjn) && mjn != 0) { data->majNegMap[dt] = (float)mjn; data->lastMajNeg = (float)mjn; }

        ++rowCount;
    }
    return rowCount;
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
        data->CachedFilePaths.clear(); 
        
        data->LastBasePath = baseFolder;
        data->LastTicker = ticker;
        data->LastDaysCount = days;
    }

    SCDateTime reference = sc.GetCurrentDateTime();
    
    // Always force reload of today's file to get updates
    // Load older files only if we haven't seen them
    for (int i = days - 1; i >= 0; --i)
    {
        SCDateTime date = SubtractDays(reference, i);
        std::string suffix = FormatDateSuffix(date);
        std::string path = baseFolder + "\\Tickers " + suffix + "\\" + ticker + ".csv";
        
        bool isToday = (i == 0);
        bool isAlreadyLoaded = data->CachedFilePaths.count(path) > 0;
        
        if (FileExists(path))
        {
            if (isToday || !isAlreadyLoaded)
            {
                int rows = LoadViewerCSV(sc, path, tzOffset, data);
                if (rows > 0) data->CachedFilePaths.insert(path);
            }
        }
    }
}

float GetValue(const std::map<SCDateTime, float>& map, SCDateTime targetTime)
{
    if (map.empty()) return -FLT_MAX;
    auto it = map.upper_bound(targetTime);
    if (it == map.begin()) return -FLT_MAX;
    --it;
    
    // 5 minute forward fill tolerance for history
    double delta = fabs((targetTime - it->first).GetAsDouble() * 86400.0);
    if (delta <= 300.0) return it->second;
    
    // If we are live (last bar) and the map has recent data, maybe return last known?
    // For now stick to strict timestamp matching
    return -FLT_MAX;
}

// =========================
//      MAIN STUDY
// =========================

SCSFExport scsf_GexBotCSVViewer(SCStudyInterfaceRef sc)
{
    // Mapped strictly to User Requirements (SG1 - SG9)
    // Note: Code uses 0-based index, so SG1 is Subgraph[0]
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
        
        SG6_Long.Name = "Major Long Gamma"; SG6_Long.DrawStyle = DRAWSTYLE_DASH; SG6_Long.PrimaryColor = RGB(0, 255, 255); SG6_Long.LineWidth = 2;
        SG7_Short.Name = "Major Short Gamma"; SG7_Short.DrawStyle = DRAWSTYLE_DASH; SG7_Short.PrimaryColor = RGB(174, 74, 213); SG7_Short.LineWidth = 2;
        
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
    if (sc.Index == sc.ArraySize - 1)
    {
        double timeSinceLast = (sc.CurrentSystemDateTime - data->LastUpdate).GetAsDouble() * 86400.0;
        if (timeSinceLast >= RefreshInterval.GetInt())
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
    float vNetVol = GetValue(data->netMap, now); 
    // float vNetOi = GetValue(data->netOiMap, now); // Not parsed currently, can be added if needed

    if (vPosVol != -FLT_MAX) SG1_CallVol[sc.Index] = vPosVol;
    if (vNegVol != -FLT_MAX) SG2_PutVol[sc.Index] = vNegVol;
    if (vZero != -FLT_MAX) SG3_Zero[sc.Index] = vZero;
    if (vPosOi != -FLT_MAX) SG4_CallOI[sc.Index] = vPosOi;
    if (vNegOi != -FLT_MAX) SG5_PutOI[sc.Index] = vNegOi;
    if (vLong != -FLT_MAX) SG6_Long[sc.Index] = vLong;
    if (vShort != -FLT_MAX) SG7_Short[sc.Index] = vShort;
    if (vNetVol != -FLT_MAX) SG8_NetVol[sc.Index] = vNetVol;
    // SG9_NetOI is waiting for parsing implementation if required
}
