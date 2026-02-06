#include "sierrachart.h"
#include <set>
#include <cfloat>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <limits>

#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <iomanip>
#define NOMINMAX
#include <windows.h>


SCDLLName("GEX_TERMINAL_API")

// =========================
//     ASYNC FETCH STATES
// =========================

enum FetchState
{
    FETCH_IDLE = 0,
    FETCH_MAJORS_SENT,
    FETCH_PROFILE_SENT,
    FETCH_STATE_CHECK_SENT,
    FETCH_GREEKS_SENT,
    FETCH_ERROR
};

// =========================
//        STRUCTURES
// =========================

struct MajorsData
{
    double mpos_vol = 0;
    double mneg_vol = 0;
    double mpos_oi = 0;
    double mneg_oi = 0;
    double zero_gamma = 0;
    double net_gex_vol = 0;
    double net_gex_oi = 0;
};

struct GreeksData
{
    double major_positive = 0;
    double major_negative = 0;
    double major_long_gamma = 0;
    double major_short_gamma = 0;
};

struct ProfileMetaData
{
    double zero_gamma = 0;
    double sum_gex_vol = 0;
    double sum_gex_oi = 0;
    double delta_risk_reversal = 0;
};

struct GammaData
{
    // Majors endpoint
    MajorsData Majors;

    // Profile endpoint meta
    ProfileMetaData ProfileMeta;

    // Greeks majors
    GreeksData Greeks;

    // Cache and state
    SCDateTime LastUpdate;
    std::string LastError;

    // Cache parameters
    std::string LastApiKey;
    std::string LastTicker;
    int LastRefreshInterval = -1;

    // Async HTTP state machine
    FetchState CurrentFetchState = FETCH_IDLE;
    bool StateEndpointAvailable = false;
    bool FetchCycleDataDirty = false;

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
    std::map<SCDateTime, float> spotMap;
    
    std::set<std::string> CachedFilePaths;
    SCDateTime LastRefreshTime;
    
    std::string LastBasePath;
    std::string LastTickerForFile;
    int LastDaysCount = -1;
    int LastTZOffset = -999;
    
    bool HistoricalDataLoaded = false;
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

std::string DoubleToString(double val, int decimals = 3)
{
    std::ostringstream oss;
    oss.precision(decimals);
    oss << std::fixed << val;
    return oss.str();
}

std::string UrlEncode(const std::string& str)
{
    std::ostringstream encoded;
    encoded.fill('0');
    encoded << std::hex;

    for (char c : str)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            encoded << c;
        else
            encoded << '%' << std::setw(2) << int((unsigned char)c);
    }

    return encoded.str();
}

// =========================
//        JSON PARSING
// =========================

std::string ExtractJsonValue(const std::string& json, const std::string& key)
{
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";

    pos++; // Skip ':'
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) 
        pos++;

    if (pos >= json.length()) return "";

    size_t start = pos;
    if (json[pos] == '"')
    {
        start++;
        pos = json.find('"', start);
        if (pos == std::string::npos) return "";
        return json.substr(start, pos - start);
    }
    else
    {
        while (pos < json.length() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']' && json[pos] != '\n')
            pos++;
        std::string val = json.substr(start, pos - start);
        // Trim
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r'))
            val.pop_back();
        return val;
    }
}

// =========================
//    RESPONSE PARSERS
// =========================

bool ParseMajorsResponse(const std::string& response, GammaData* data)
{
    if (response.empty() || response == "ERROR" || response == "HTTP_REQUEST_ERROR")
    {
        data->LastError = "Majors HTTP request failed or empty response";
        return false;
    }

    std::string errorMsg = ExtractJsonValue(response, "error");
    if (!errorMsg.empty())
    {
        data->LastError = "API error: " + errorMsg;
        return false;
    }

    data->Majors.mpos_vol = StringToDouble(ExtractJsonValue(response, "mpos_vol"));
    data->Majors.mneg_vol = StringToDouble(ExtractJsonValue(response, "mneg_vol"));
    data->Majors.mpos_oi = StringToDouble(ExtractJsonValue(response, "mpos_oi"));
    data->Majors.mneg_oi = StringToDouble(ExtractJsonValue(response, "mneg_oi"));
    data->Majors.zero_gamma = StringToDouble(ExtractJsonValue(response, "zero_gamma"));
    data->Majors.net_gex_vol = StringToDouble(ExtractJsonValue(response, "net_gex_vol"));
    data->Majors.net_gex_oi = StringToDouble(ExtractJsonValue(response, "net_gex_oi"));

    return true;
}

bool ParseProfileResponse(const std::string& response, GammaData* data)
{
    if (response.empty() || response == "ERROR" || response == "HTTP_REQUEST_ERROR")
    {
        data->LastError = "Profile HTTP request failed or empty response";
        return false;
    }

    std::string errorMsg = ExtractJsonValue(response, "error");
    if (!errorMsg.empty())
    {
        data->LastError = "API error: " + errorMsg;
        return false;
    }

    data->ProfileMeta.zero_gamma = StringToDouble(ExtractJsonValue(response, "zero_gamma"));
    data->ProfileMeta.sum_gex_vol = StringToDouble(ExtractJsonValue(response, "sum_gex_vol"));
    data->ProfileMeta.sum_gex_oi = StringToDouble(ExtractJsonValue(response, "sum_gex_oi"));
    data->ProfileMeta.delta_risk_reversal = StringToDouble(ExtractJsonValue(response, "delta_risk_reversal"));

    return true;
}

bool ParseStateCheckResponse(const std::string& response, GammaData* data)
{
    if (response.empty() || response == "ERROR" || response == "HTTP_REQUEST_ERROR")
        return false;

    if (response.find("\"error\"") != std::string::npos ||
        response.find("access denied") != std::string::npos ||
        response.find("Access Denied") != std::string::npos)
        return false;

    data->ProfileMeta.zero_gamma = StringToDouble(ExtractJsonValue(response, "zero_gamma"));
    data->ProfileMeta.sum_gex_vol = StringToDouble(ExtractJsonValue(response, "sum_gex_vol"));
    data->ProfileMeta.sum_gex_oi = StringToDouble(ExtractJsonValue(response, "sum_gex_oi"));
    data->ProfileMeta.delta_risk_reversal = StringToDouble(ExtractJsonValue(response, "delta_risk_reversal"));

    return true;
}

bool ParseGreeksResponse(const std::string& response, GammaData* data)
{
    if (response.empty() || response == "ERROR" || response == "HTTP_REQUEST_ERROR")
    {
        data->LastError = "Greeks HTTP request failed or empty response";
        return false;
    }

    std::string errorMsg = ExtractJsonValue(response, "error");
    if (!errorMsg.empty())
    {
        data->LastError = "API error: " + errorMsg;
        return false;
    }

    data->Greeks.major_positive = StringToDouble(ExtractJsonValue(response, "major_positive"));
    data->Greeks.major_negative = StringToDouble(ExtractJsonValue(response, "major_negative"));
    data->Greeks.major_long_gamma = StringToDouble(ExtractJsonValue(response, "major_long_gamma"));
    data->Greeks.major_short_gamma = StringToDouble(ExtractJsonValue(response, "major_short_gamma"));

    return true;
}

// =========================
//     CSV FILE I/O
// =========================

// Create directory tree recursively (Windows API)
void EnsureDirectoryExists(const std::string& filePath)
{
    size_t lastSlash = filePath.find_last_of("\\/");
    if (lastSlash == std::string::npos) return;

    std::string dirPath = filePath.substr(0, lastSlash);
    if (dirPath.empty()) return;

    std::wstring wDirPath(dirPath.begin(), dirPath.end());
    DWORD dwAttrib = GetFileAttributesW(wDirPath.c_str());
    if (dwAttrib != INVALID_FILE_ATTRIBUTES) return; // Already exists

    // Create directory tree
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

// Helper to check if a file exists
bool FileExists(const std::string& path)
{
    std::wstring wPath(path.begin(), path.end());
    DWORD dwAttrib = GetFileAttributesW(wPath.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

// CSV header line
static const char* CSV_HEADER = "timestamp,spot,zero_gamma,major_pos_vol,major_neg_vol,"
    "major_pos_oi,major_neg_oi,sum_gex_vol,sum_gex_oi,delta_risk_reversal,"
    "major_long_gamma,major_short_gamma,major_positive,major_negative,net\r\n";

// Write a single data row to a CSV file (append mode)
bool WriteToCsvFile(SCStudyInterfaceRef sc, GammaData* data, const std::string& csvPath)
{
    if (csvPath.empty()) return false;

    EnsureDirectoryExists(csvPath);

    // Check if file exists to decide whether to write header
    bool needsHeader = !FileExists(csvPath);

    // Open file for append
    int fileHandle = 0;
    SCString scPath(csvPath.c_str());
    if (!sc.OpenFile(scPath, n_ACSIL::FILE_MODE_OPEN_TO_APPEND, fileHandle))
    {
        // File doesn't exist yet - create it
        if (!sc.OpenFile(scPath, n_ACSIL::FILE_MODE_CREATE_AND_OPEN_FOR_READ_WRITE, fileHandle))
        {
            data->LastError = "Failed to open CSV file for writing";
            return false;
        }
        needsHeader = true;
    }

    unsigned int bytesWritten = 0;

    // Write header if new file
    if (needsHeader)
    {
        int headerLen = (int)strlen(CSV_HEADER);
        sc.WriteFile(fileHandle, CSV_HEADER, headerLen, &bytesWritten);
    }

    // Build data line
    double scDateTime = sc.CurrentSystemDateTime.GetAsDouble();
    double unixTimestamp = (scDateTime - 25569.0) * 86400.0;

    double spot = 0.0;
    if (sc.BaseData[SC_CLOSE].GetArraySize() > 0 && sc.Index >= 0)
        spot = sc.BaseData[SC_CLOSE][sc.Index];

    double netGex = data->Majors.mpos_vol - fabs(data->Majors.mneg_vol);
    double zeroGamma = data->ProfileMeta.zero_gamma != 0 ? data->ProfileMeta.zero_gamma : data->Majors.zero_gamma;
    double net = spot + netGex / 100.0;

    char line[1024];
    int lineLen = snprintf(line, sizeof(line),
        "%.1f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\r\n",
        unixTimestamp,
        spot,
        zeroGamma,
        data->Majors.mpos_vol,
        data->Majors.mneg_vol,
        data->Majors.mpos_oi,
        data->Majors.mneg_oi,
        data->ProfileMeta.sum_gex_vol,
        data->ProfileMeta.sum_gex_oi,
        data->ProfileMeta.delta_risk_reversal,
        data->Greeks.major_long_gamma,
        data->Greeks.major_short_gamma,
        data->Greeks.major_positive,
        data->Greeks.major_negative,
        net);

    sc.WriteFile(fileHandle, line, lineLen, &bytesWritten);
    sc.CloseFile(fileHandle);

    return true;
}

// Parse a single CSV line into an array of doubles
// Returns number of fields parsed
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
            while (!field.empty() && (field.front() == ' ' || field.front() == '\t'))
                field.erase(0, 1);
            while (!field.empty() && (field.back() == ' ' || field.back() == '\t' || field.back() == '\r' || field.back() == '\n'))
                field.pop_back();

            values[count] = StringToDouble(field);
            count++;
            start = pos + 1;
        }
        pos++;
    }

    return count;
}

// Load historical data from a single CSV file
int LoadSingleCSV(SCStudyInterfaceRef sc, const std::string& fullPath, int tzOffsetHours, GammaData* data)
{
    int rowCount = 0;

    int fileHandle = 0;
    SCString scPath(fullPath.c_str());
    if (!sc.OpenFile(scPath, n_ACSIL::FILE_MODE_OPEN_EXISTING_FOR_SEQUENTIAL_READING, fileHandle))
    {
        SCString msg;
        msg.Format("GEX_TERMINAL: Cannot open %s", fullPath.c_str());
        sc.AddMessageToLog(msg, 0);
        return 0;
    }

    // Read entire file into a buffer
    // CSV files for daily data are small (< 100KB typically)
    const int BUFFER_SIZE = 256 * 1024; // 256KB max
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

    // Parse line by line
    std::string content(buffer, totalRead);
    delete[] buffer;

    size_t lineStart = 0;
    bool isFirstLine = true;

    while (lineStart < content.length())
    {
        size_t lineEnd = content.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = content.length();

        std::string line = content.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;

        // Trim trailing \r
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();

        if (line.empty()) continue;

        // Skip header line
        if (isFirstLine)
        {
            isFirstLine = false;
            // Check if this is actually a header (starts with "timestamp" or non-numeric)
            if (line.find("timestamp") != std::string::npos || (!line.empty() && !isdigit(line[0]) && line[0] != '-'))
                continue;
        }

        // Parse CSV fields:
        // 0:timestamp, 1:spot, 2:zero_gamma, 3:major_pos_vol, 4:major_neg_vol,
        // 5:major_pos_oi, 6:major_neg_oi, 7:sum_gex_vol, 8:sum_gex_oi,
        // 9:delta_risk_reversal, 10:major_long_gamma, 11:major_short_gamma,
        // 12:major_positive, 13:major_negative, 14:net
        double values[15] = {0};
        int fieldCount = ParseCsvLine(line, values, 15);
        if (fieldCount < 5) continue; // Need at least timestamp + a few fields

        double ts = values[0];
        if (ts <= 0) continue;

        // Convert Unix timestamp to SCDateTime with timezone offset
        SCDateTime dt((ts + tzOffsetHours * 3600.0) / 86400.0 + 25569.0);

        double spot  = values[1];
        double zero  = values[2];
        double posv  = values[3];
        double negv  = values[4];
        double posoi = (fieldCount > 5) ? values[5] : 0;
        double negoi = (fieldCount > 6) ? values[6] : 0;
        // 7:sum_gex_vol, 8:sum_gex_oi, 9:delta_risk_reversal (skipped for maps)
        double lng   = (fieldCount > 10) ? values[10] : 0;
        double sht   = (fieldCount > 11) ? values[11] : 0;
        double mjp   = (fieldCount > 12) ? values[12] : 0;
        double mjn   = (fieldCount > 13) ? values[13] : 0;

        // Store in maps (map overwrites duplicates naturally)
        if (!std::isnan(spot) && spot != 0) data->spotMap[dt] = static_cast<float>(spot);
        if (!std::isnan(zero) && zero != 0) data->zeroMap[dt] = static_cast<float>(zero);
        if (!std::isnan(posv) && posv != 0) data->posVolMap[dt] = static_cast<float>(posv);
        if (!std::isnan(negv) && negv != 0) data->negVolMap[dt] = static_cast<float>(negv);
        if (!std::isnan(posoi) && posoi != 0) data->posOiMap[dt] = static_cast<float>(posoi);
        if (!std::isnan(negoi) && negoi != 0) data->negOiMap[dt] = static_cast<float>(negoi);
        if (!std::isnan(lng) && lng != 0) data->longMap[dt] = static_cast<float>(lng);
        if (!std::isnan(sht) && sht != 0) data->shortMap[dt] = static_cast<float>(sht);
        if (!std::isnan(mjp) && mjp != 0) data->majPosMap[dt] = static_cast<float>(mjp);
        if (!std::isnan(mjn) && mjn != 0) data->majNegMap[dt] = static_cast<float>(mjn);

        // Calculate net (spot + netGex / 100.0)
        if (!std::isnan(spot) && !std::isnan(posv) && !std::isnan(negv))
        {
            double netGex = posv - fabs(negv);
            data->netMap[dt] = static_cast<float>(spot + netGex / 100.0);
        }

        ++rowCount;
    }

    if (rowCount > 0)
    {
        SCString msg;
        msg.Format("GEX_TERMINAL: Loaded %d rows from %s", rowCount, fullPath.c_str());
        sc.AddMessageToLog(msg, 0);
    }

    return rowCount;
}

// =========================
//     DATE/TIME HELPERS
// =========================

// Helper to format date as MM.dd.yyyy
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

// Forward fill: find the most recent value <= targetTime with 5 minute tolerance
float GetValueAtTime(const std::map<SCDateTime, float>& map, SCDateTime targetTime)
{
    if (map.empty()) return -FLT_MAX;

    auto it = map.upper_bound(targetTime);
    if (it == map.begin()) return -FLT_MAX;

    --it;
    
    double delta = fabs((targetTime - it->first).GetAsDouble() * 86400.0);
    if (delta <= 300.0)
        return it->second;
    
    return -FLT_MAX;
}

// =========================
//  HISTORICAL DATA LOADING
// =========================

void LoadRecentGammaFiles(SCStudyInterfaceRef sc, GammaData* data, const std::string& baseFolder, const std::string& ticker, int days, int tzOffset, int refreshSeconds)
{
    // Clear if parameters changed
    if (baseFolder != data->LastBasePath || ticker != data->LastTickerForFile || days != data->LastDaysCount || tzOffset != data->LastTZOffset)
    {
        data->CachedFilePaths.clear();
        data->zeroMap.clear();
        data->posVolMap.clear();
        data->negVolMap.clear();
        data->posOiMap.clear();
        data->negOiMap.clear();
        data->netMap.clear();
        data->longMap.clear();
        data->shortMap.clear();
        data->majPosMap.clear();
        data->majNegMap.clear();
        data->spotMap.clear();

        data->LastBasePath = baseFolder;
        data->LastTickerForFile = ticker;
        data->LastDaysCount = days;
        data->LastTZOffset = tzOffset;
    }

    SCDateTime reference = sc.GetCurrentDateTime();

    int totalRowsLoaded = 0;
    for (int i = days - 1; i >= 0; --i)
    {
        SCDateTime date = SubtractDays(reference, i);
        std::string suffix = FormatDateSuffix(date);
        std::string path = baseFolder + "\\Tickers " + suffix + "\\" + ticker + ".csv";

        bool isToday = (i == 0);
        bool isAlreadyLoaded = data->CachedFilePaths.count(path) > 0;
        bool refreshDue = (sc.CurrentSystemDateTime - data->LastRefreshTime).GetAsDouble() > (refreshSeconds / 86400.0);

        if (FileExists(path))
        {
            if (isToday && refreshDue)
            {
                int rows = LoadSingleCSV(sc, path, tzOffset, data);
                data->LastRefreshTime = sc.CurrentSystemDateTime;
                totalRowsLoaded += rows;
            }
            else if (!isToday && !isAlreadyLoaded)
            {
                int rows = LoadSingleCSV(sc, path, tzOffset, data);
                if (rows > 0)
                {
                    data->CachedFilePaths.insert(path);
                    totalRowsLoaded += rows;
                }
            }
        }
        else
        {
            if (i < 3)
            {
                SCString msg;
                msg.Format("GEX_TERMINAL: File not found: %s", path.c_str());
                sc.AddMessageToLog(msg, 0);
            }
        }
    }
    
    if (totalRowsLoaded > 0)
    {
        SCString msg;
        msg.Format("GEX_TERMINAL: Total loaded: %d rows, Zero points: %d", totalRowsLoaded, (int)data->zeroMap.size());
        sc.AddMessageToLog(msg, 0);
    }
}

// =========================
//   UPDATE MAPS + WRITE
// =========================

void UpdateMapsAndWriteCSV(SCStudyInterfaceRef sc, GammaData* data,
    const std::string& ticker, const std::string& writePath)
{
    SCDateTime scDateTime = sc.CurrentSystemDateTime;

    float zeroGamma = (data->ProfileMeta.zero_gamma != 0)
        ? static_cast<float>(data->ProfileMeta.zero_gamma)
        : static_cast<float>(data->Majors.zero_gamma);
    
    if (zeroGamma != 0) data->zeroMap[scDateTime] = zeroGamma;
    if (data->Majors.mpos_vol != 0) data->posVolMap[scDateTime] = static_cast<float>(data->Majors.mpos_vol);
    if (data->Majors.mneg_vol != 0) data->negVolMap[scDateTime] = static_cast<float>(data->Majors.mneg_vol);
    if (data->Majors.mpos_oi != 0) data->posOiMap[scDateTime] = static_cast<float>(data->Majors.mpos_oi);
    if (data->Majors.mneg_oi != 0) data->negOiMap[scDateTime] = static_cast<float>(data->Majors.mneg_oi);
    if (data->Greeks.major_long_gamma != 0) data->longMap[scDateTime] = static_cast<float>(data->Greeks.major_long_gamma);
    if (data->Greeks.major_short_gamma != 0) data->shortMap[scDateTime] = static_cast<float>(data->Greeks.major_short_gamma);
    if (data->Greeks.major_positive != 0) data->majPosMap[scDateTime] = static_cast<float>(data->Greeks.major_positive);
    if (data->Greeks.major_negative != 0) data->majNegMap[scDateTime] = static_cast<float>(data->Greeks.major_negative);
    
    // Calculate net
    if (sc.BaseData[SC_CLOSE].GetArraySize() > 0 && sc.Index >= 0)
    {
        float spot = static_cast<float>(sc.BaseData[SC_CLOSE][sc.Index]);
        if (spot != 0 && data->Majors.mpos_vol != 0 && data->Majors.mneg_vol != 0)
        {
            float netGex = static_cast<float>(data->Majors.mpos_vol - fabs(data->Majors.mneg_vol));
            data->netMap[scDateTime] = spot + netGex / 100.0f;
        }
    }
    
    // Write to CSV file
    if (!writePath.empty() && !ticker.empty())
    {
        SCDateTime today = sc.GetCurrentDateTime();
        int year = today.GetYear();
        int month = today.GetMonth();
        int day = today.GetDay();
        char dateStr[32];
        snprintf(dateStr, sizeof(dateStr), "%02d.%02d.%04d", month, day, year);
        std::string todayCsvPath = writePath + "\\Tickers " + dateStr + "\\" + ticker + ".csv";
        
        WriteToCsvFile(sc, data, todayCsvPath);
    }
}

// =========================
//        INDICATOR
// =========================

SCSFExport scsf_GexBotAPI(SCStudyInterfaceRef sc)
{
    SCSubgraphRef LongGamma = sc.Subgraph[0];
    SCSubgraphRef ShortGamma = sc.Subgraph[1];
    SCSubgraphRef MajorPositive = sc.Subgraph[2];
    SCSubgraphRef MajorNegative = sc.Subgraph[3];
    SCSubgraphRef Zero = sc.Subgraph[4];
    SCSubgraphRef MajorPosVol = sc.Subgraph[5];
    SCSubgraphRef MajorNegVol = sc.Subgraph[6];
    SCSubgraphRef MajorPosOi = sc.Subgraph[7];
    SCSubgraphRef MajorNegOi = sc.Subgraph[8];
    SCSubgraphRef GreekMajorPos = sc.Subgraph[9];
    SCSubgraphRef GreekMajorNeg = sc.Subgraph[10];
    SCSubgraphRef MajorLongGamma = sc.Subgraph[11];
    SCSubgraphRef MajorShortGamma = sc.Subgraph[12];

    SCInputRef ApiKeyInput = sc.Input[0];
    SCInputRef TickerInput = sc.Input[1];
    SCInputRef MultiplierInput = sc.Input[2];
    SCInputRef RefreshIntervalInput = sc.Input[3];
    SCInputRef CsvReadPathInput = sc.Input[4];
    SCInputRef CsvWritePathInput = sc.Input[5];
    SCInputRef DaysToLoadInput = sc.Input[6];
    SCInputRef TZOffsetInput = sc.Input[7];
    SCInputRef ShowMajorPosVolInput = sc.Input[8];
    SCInputRef ShowMajorNegVolInput = sc.Input[9];
    SCInputRef ShowZeroGammaInput = sc.Input[10];
    SCInputRef ShowMajorPosOiInput = sc.Input[11];
    SCInputRef ShowMajorNegOiInput = sc.Input[12];
    SCInputRef ShowGreekMajorPosInput = sc.Input[13];
    SCInputRef ShowGreekMajorNegInput = sc.Input[14];
    SCInputRef ShowMajorLongGammaInput = sc.Input[15];
    SCInputRef ShowMajorShortGammaInput = sc.Input[16];

    if (sc.SetDefaults)
    {
        sc.GraphName = "GEX BOT API";
        sc.AutoLoop = 1;
        sc.GraphRegion = 0;
        sc.ValueFormat = 2;
        // sc.MakeHTTPRequest automatically triggers a study callback when the response arrives.
        // No need for UpdateAlways - normal bar/tick updates handle refresh timing.

        // Subgraphs
        LongGamma.Name = "Long Gamma"; LongGamma.DrawStyle = DRAWSTYLE_DASH; LongGamma.PrimaryColor = RGB(0, 255, 255); LongGamma.LineWidth = 2;
        ShortGamma.Name = "Short Gamma"; ShortGamma.DrawStyle = DRAWSTYLE_DASH; ShortGamma.PrimaryColor = RGB(174, 74, 213); ShortGamma.LineWidth = 2;
        MajorPositive.Name = "Major Positive"; MajorPositive.DrawStyle = DRAWSTYLE_DASH; MajorPositive.PrimaryColor = RGB(0, 255, 0); MajorPositive.LineWidth = 1;
        MajorNegative.Name = "Major Negative"; MajorNegative.DrawStyle = DRAWSTYLE_DASH; MajorNegative.PrimaryColor = RGB(255, 0, 0); MajorNegative.LineWidth = 1;
        Zero.Name = "Zero Gamma"; Zero.DrawStyle = DRAWSTYLE_DASH; Zero.PrimaryColor = RGB(252, 177, 3); Zero.LineWidth = 1;
        MajorPosVol.Name = "Major + Vol"; MajorPosVol.DrawStyle = DRAWSTYLE_DASH; MajorPosVol.PrimaryColor = RGB(0, 159, 0); MajorPosVol.LineWidth = 1;
        MajorNegVol.Name = "Major - Vol"; MajorNegVol.DrawStyle = DRAWSTYLE_DASH; MajorNegVol.PrimaryColor = RGB(174, 0, 0); MajorNegVol.LineWidth = 1;
        MajorPosOi.Name = "Major + OI"; MajorPosOi.DrawStyle = DRAWSTYLE_DASH; MajorPosOi.PrimaryColor = RGB(0, 255, 255); MajorPosOi.LineWidth = 1;
        MajorNegOi.Name = "Major - OI"; MajorNegOi.DrawStyle = DRAWSTYLE_DASH; MajorNegOi.PrimaryColor = RGB(255, 165, 0); MajorNegOi.LineWidth = 1;
        GreekMajorPos.Name = "Greek Major +"; GreekMajorPos.DrawStyle = DRAWSTYLE_DASH; GreekMajorPos.PrimaryColor = RGB(0, 255, 0); GreekMajorPos.LineWidth = 1;
        GreekMajorNeg.Name = "Greek Major -"; GreekMajorNeg.DrawStyle = DRAWSTYLE_DASH; GreekMajorNeg.PrimaryColor = RGB(255, 0, 0); GreekMajorNeg.LineWidth = 1;
        MajorLongGamma.Name = "Major Long Gamma"; MajorLongGamma.DrawStyle = DRAWSTYLE_DASH; MajorLongGamma.PrimaryColor = RGB(0, 255, 255); MajorLongGamma.LineWidth = 1;
        MajorShortGamma.Name = "Major Short Gamma"; MajorShortGamma.DrawStyle = DRAWSTYLE_DASH; MajorShortGamma.PrimaryColor = RGB(255, 165, 0); MajorShortGamma.LineWidth = 1;

        // Inputs
        ApiKeyInput.Name = "API Key"; ApiKeyInput.SetString("YOUR_API_KEY");
        TickerInput.Name = "Ticker"; TickerInput.SetString("ES_SPX");
        MultiplierInput.Name = "Multiplier"; MultiplierInput.SetFloat(1.0);
        MultiplierInput.SetFloatLimits(0.001f, 1000.0f);
        RefreshIntervalInput.Name = "Refresh (seconds)"; RefreshIntervalInput.SetInt(10);
        RefreshIntervalInput.SetIntLimits(1, 3600);
        CsvReadPathInput.Name = "CSV Read Path (e.g. C:\\GexBot\\Data)"; CsvReadPathInput.SetString("");
        CsvWritePathInput.Name = "CSV Write Path (e.g. C:\\GexBot\\Data)"; CsvWritePathInput.SetString("");
        DaysToLoadInput.Name = "Days to Load"; DaysToLoadInput.SetInt(15);
        DaysToLoadInput.SetIntLimits(1, 30);
        TZOffsetInput.Name = "UTC Offset (hours)"; TZOffsetInput.SetInt(0);
        TZOffsetInput.SetIntLimits(-12, 12);

        ShowMajorPosVolInput.Name = "Show Major + Vol"; ShowMajorPosVolInput.SetYesNo(1);
        ShowMajorNegVolInput.Name = "Show Major - Vol"; ShowMajorNegVolInput.SetYesNo(1);
        ShowZeroGammaInput.Name = "Show Zero Gamma"; ShowZeroGammaInput.SetYesNo(1);
        ShowMajorPosOiInput.Name = "Show Major + OI"; ShowMajorPosOiInput.SetYesNo(0);
        ShowMajorNegOiInput.Name = "Show Major - OI"; ShowMajorNegOiInput.SetYesNo(0);
        ShowGreekMajorPosInput.Name = "Show Greek Major +"; ShowGreekMajorPosInput.SetYesNo(1);
        ShowGreekMajorNegInput.Name = "Show Greek Major -"; ShowGreekMajorNegInput.SetYesNo(1);
        ShowMajorLongGammaInput.Name = "Show Major Long Gamma"; ShowMajorLongGammaInput.SetYesNo(0);
        ShowMajorShortGammaInput.Name = "Show Major Short Gamma"; ShowMajorShortGammaInput.SetYesNo(0);

        return;
    }

    if (sc.LastCallToFunction)
    {
        GammaData* data = static_cast<GammaData*>(sc.GetPersistentPointer(1));
        if (data)
        {
            delete data;
            sc.SetPersistentPointer(1, nullptr);
        }
        return;
    }

    GammaData* data = static_cast<GammaData*>(sc.GetPersistentPointer(1));
    if (!data)
    {
        data = new GammaData();
        sc.SetPersistentPointer(1, data);
    }

    // Get parameters (lightweight - just reading input refs)
    float multiplier = MultiplierInput.GetFloat();
    bool isLastBar = (sc.Index == sc.ArraySize - 1);

    // ========================================
    //   LAST BAR ONLY: CSV loading + async HTTP
    // ========================================
    if (isLastBar)
    {
        std::string ticker = TickerInput.GetString();
        int refreshInterval = RefreshIntervalInput.GetInt();
        std::string apiKey = ApiKeyInput.GetString();
        std::string writePath = CsvWritePathInput.GetString();

        // Load historical data once at startup or if parameters change
        std::string readPath = CsvReadPathInput.GetString();
        int daysToLoad = DaysToLoadInput.GetInt();
        int tzOffset = TZOffsetInput.GetInt();
        
        bool paramsChanged = (readPath != data->LastBasePath || 
                             ticker != data->LastTickerForFile || 
                             daysToLoad != data->LastDaysCount || 
                             tzOffset != data->LastTZOffset);
        
        if (!readPath.empty() && !ticker.empty() && (paramsChanged || !data->HistoricalDataLoaded))
        {
            LoadRecentGammaFiles(sc, data, readPath, ticker, daysToLoad, tzOffset, refreshInterval);
            data->HistoricalDataLoaded = true;
        }

        // ------ STATE: IDLE ------
        if (data->CurrentFetchState == FETCH_IDLE)
        {
            bool needsRefresh = false;

            if (apiKey.empty() || apiKey == "YOUR_API_KEY")
            {
                // No valid API key, skip
            }
            else if (apiKey != data->LastApiKey || ticker != data->LastTicker || refreshInterval != data->LastRefreshInterval)
            {
                needsRefresh = true;
            }
            else
            {
                double timeSinceUpdate = (sc.CurrentSystemDateTime - data->LastUpdate).GetAsDouble() * 86400.0;
                if (timeSinceUpdate >= refreshInterval)
                    needsRefresh = true;
            }

            if (needsRefresh)
            {
                SCString url;
                url.Format("https://api.gexbot.com/%s/classic/zero/majors?key=%s",
                    UrlEncode(ticker).c_str(), UrlEncode(apiKey).c_str());
                
                if (sc.MakeHTTPRequest(url))
                {
                    data->CurrentFetchState = FETCH_MAJORS_SENT;
                    data->FetchCycleDataDirty = false;
                }
                else
                {
                    data->LastError = "Failed to send majors request";
                }
            }
        }

        // ------ STATE: MAJORS_SENT ------
        else if (data->CurrentFetchState == FETCH_MAJORS_SENT && sc.HTTPResponse != "")
        {
            std::string response = sc.HTTPResponse.GetChars();
            if (ParseMajorsResponse(response, data))
                data->FetchCycleDataDirty = true;

            SCString url;
            url.Format("https://api.gexbot.com/%s/classic/zero?key=%s",
                UrlEncode(ticker).c_str(), UrlEncode(apiKey).c_str());
            
            if (sc.MakeHTTPRequest(url))
                data->CurrentFetchState = FETCH_PROFILE_SENT;
            else
            {
                data->LastError = "Failed to send profile request";
                data->CurrentFetchState = FETCH_ERROR;
            }
        }

        // ------ STATE: PROFILE_SENT ------
        else if (data->CurrentFetchState == FETCH_PROFILE_SENT && sc.HTTPResponse != "")
        {
            std::string response = sc.HTTPResponse.GetChars();
            if (ParseProfileResponse(response, data))
                data->FetchCycleDataDirty = true;

            SCString url;
            url.Format("https://api.gexbot.com/%s/state/zero?key=%s",
                UrlEncode(ticker).c_str(), UrlEncode(apiKey).c_str());
            
            if (sc.MakeHTTPRequest(url))
                data->CurrentFetchState = FETCH_STATE_CHECK_SENT;
            else
            {
                data->LastError = "Failed to send state check request";
                data->CurrentFetchState = FETCH_ERROR;
            }
        }

        // ------ STATE: STATE_CHECK_SENT ------
        else if (data->CurrentFetchState == FETCH_STATE_CHECK_SENT && sc.HTTPResponse != "")
        {
            std::string response = sc.HTTPResponse.GetChars();
            data->StateEndpointAvailable = ParseStateCheckResponse(response, data);
            
            if (data->StateEndpointAvailable)
            {
                data->FetchCycleDataDirty = true;

                SCString url;
                url.Format("https://api.gexbot.com/%s/state/GEX_zero?key=%s",
                    UrlEncode(ticker).c_str(), UrlEncode(apiKey).c_str());
                
                if (sc.MakeHTTPRequest(url))
                    data->CurrentFetchState = FETCH_GREEKS_SENT;
                else
                {
                    data->LastError = "Failed to send greeks request";
                    data->CurrentFetchState = FETCH_ERROR;
                }
            }
            else
            {
                // State not available - finish with classic only
                data->LastApiKey = apiKey;
                data->LastTicker = ticker;
                data->LastRefreshInterval = refreshInterval;
                data->LastUpdate = sc.CurrentSystemDateTime;
                data->LastError = "OK (classic only)";
                data->CurrentFetchState = FETCH_IDLE;

                if (data->FetchCycleDataDirty)
                    UpdateMapsAndWriteCSV(sc, data, ticker, writePath);
            }
        }

        // ------ STATE: GREEKS_SENT ------
        else if (data->CurrentFetchState == FETCH_GREEKS_SENT && sc.HTTPResponse != "")
        {
            std::string response = sc.HTTPResponse.GetChars();
            if (ParseGreeksResponse(response, data))
                data->FetchCycleDataDirty = true;

            data->LastApiKey = apiKey;
            data->LastTicker = ticker;
            data->LastRefreshInterval = refreshInterval;
            data->LastUpdate = sc.CurrentSystemDateTime;
            data->LastError = "OK";
            data->CurrentFetchState = FETCH_IDLE;

            if (data->FetchCycleDataDirty)
                UpdateMapsAndWriteCSV(sc, data, ticker, writePath);
        }

        // ------ STATE: ERROR ------
        else if (data->CurrentFetchState == FETCH_ERROR)
        {
            double timeSinceUpdate = (sc.CurrentSystemDateTime - data->LastUpdate).GetAsDouble() * 86400.0;
            if (timeSinceUpdate >= refreshInterval)
                data->CurrentFetchState = FETCH_IDLE;
        }
    }

    // ========================================
    //   SUBGRAPH RENDERING (all bars)
    // ========================================

    SCDateTime now = sc.BaseDateTimeIn[sc.Index];
    
    // Debug: log map sizes once on full recalculation
    if (sc.Index == 0 && sc.IsFullRecalculation && !data->zeroMap.empty())
    {
        SCString msg;
        msg.Format("GEX_TERMINAL: Maps loaded - Zero: %d, PosVol: %d, Long: %d", 
                   (int)data->zeroMap.size(), (int)data->posVolMap.size(), (int)data->longMap.size());
        sc.AddMessageToLog(msg, 0);
    }
    
    auto getVal = [&](const std::map<SCDateTime, float>& map) -> float {
        return GetValueAtTime(map, now);
    };
    
    float zeroVal = getVal(data->zeroMap);
    float posVolVal = getVal(data->posVolMap);
    float negVolVal = getVal(data->negVolMap);
    float posOiVal = getVal(data->posOiMap);
    float negOiVal = getVal(data->negOiMap);
    float netVal = getVal(data->netMap);
    float longVal = getVal(data->longMap);
    float shortVal = getVal(data->shortMap);
    float majPosVal = getVal(data->majPosMap);
    float majNegVal = getVal(data->majNegMap);

    float zeroGamma = (zeroVal != -FLT_MAX) ? zeroVal : ((data->ProfileMeta.zero_gamma != 0) ? static_cast<float>(data->ProfileMeta.zero_gamma) : static_cast<float>(data->Majors.zero_gamma));
    LongGamma[sc.Index] = multiplier * ((longVal != -FLT_MAX) ? longVal : static_cast<float>(data->Greeks.major_long_gamma));
    ShortGamma[sc.Index] = multiplier * ((shortVal != -FLT_MAX) ? shortVal : static_cast<float>(data->Greeks.major_short_gamma));
    MajorPositive[sc.Index] = multiplier * ((majPosVal != -FLT_MAX) ? majPosVal : static_cast<float>(data->Greeks.major_positive));
    MajorNegative[sc.Index] = multiplier * ((majNegVal != -FLT_MAX) ? majNegVal : static_cast<float>(data->Greeks.major_negative));
    Zero[sc.Index] = multiplier * zeroGamma;
    MajorPosVol[sc.Index] = ShowMajorPosVolInput.GetYesNo() ? multiplier * ((posVolVal != -FLT_MAX) ? posVolVal : static_cast<float>(data->Majors.mpos_vol)) : 0;
    MajorNegVol[sc.Index] = ShowMajorNegVolInput.GetYesNo() ? multiplier * ((negVolVal != -FLT_MAX) ? negVolVal : static_cast<float>(data->Majors.mneg_vol)) : 0;
    MajorPosOi[sc.Index] = ShowMajorPosOiInput.GetYesNo() ? multiplier * ((posOiVal != -FLT_MAX) ? posOiVal : static_cast<float>(data->Majors.mpos_oi)) : 0;
    MajorNegOi[sc.Index] = ShowMajorNegOiInput.GetYesNo() ? multiplier * ((negOiVal != -FLT_MAX) ? negOiVal : static_cast<float>(data->Majors.mneg_oi)) : 0;
    GreekMajorPos[sc.Index] = ShowGreekMajorPosInput.GetYesNo() ? multiplier * ((majPosVal != -FLT_MAX) ? majPosVal : static_cast<float>(data->Greeks.major_positive)) : 0;
    GreekMajorNeg[sc.Index] = ShowGreekMajorNegInput.GetYesNo() ? multiplier * ((majNegVal != -FLT_MAX) ? majNegVal : static_cast<float>(data->Greeks.major_negative)) : 0;
    MajorLongGamma[sc.Index] = ShowMajorLongGammaInput.GetYesNo() ? multiplier * ((longVal != -FLT_MAX) ? longVal : static_cast<float>(data->Greeks.major_long_gamma)) : 0;
    MajorShortGamma[sc.Index] = ShowMajorShortGammaInput.GetYesNo() ? multiplier * ((shortVal != -FLT_MAX) ? shortVal : static_cast<float>(data->Greeks.major_short_gamma)) : 0;

    // Hide subgraphs if not shown
    LongGamma.LineStyle = (data->Greeks.major_long_gamma != 0) ? LINESTYLE_SOLID : LINESTYLE_UNSET;
    ShortGamma.LineStyle = (data->Greeks.major_short_gamma != 0) ? LINESTYLE_SOLID : LINESTYLE_UNSET;
    MajorPositive.LineStyle = (data->Greeks.major_positive != 0) ? LINESTYLE_SOLID : LINESTYLE_UNSET;
    MajorNegative.LineStyle = (data->Greeks.major_negative != 0) ? LINESTYLE_SOLID : LINESTYLE_UNSET;
    Zero.LineStyle = ShowZeroGammaInput.GetYesNo() && zeroGamma != 0 ? LINESTYLE_SOLID : LINESTYLE_UNSET;
    MajorPosVol.LineStyle = ShowMajorPosVolInput.GetYesNo() && data->Majors.mpos_vol != 0 ? LINESTYLE_SOLID : LINESTYLE_UNSET;
    MajorNegVol.LineStyle = ShowMajorNegVolInput.GetYesNo() && data->Majors.mneg_vol != 0 ? LINESTYLE_SOLID : LINESTYLE_UNSET;
    MajorPosOi.LineStyle = ShowMajorPosOiInput.GetYesNo() && data->Majors.mpos_oi != 0 ? LINESTYLE_SOLID : LINESTYLE_UNSET;
    MajorNegOi.LineStyle = ShowMajorNegOiInput.GetYesNo() && data->Majors.mneg_oi != 0 ? LINESTYLE_SOLID : LINESTYLE_UNSET;
    GreekMajorPos.LineStyle = ShowGreekMajorPosInput.GetYesNo() && data->Greeks.major_positive != 0 ? LINESTYLE_SOLID : LINESTYLE_UNSET;
    GreekMajorNeg.LineStyle = ShowGreekMajorNegInput.GetYesNo() && data->Greeks.major_negative != 0 ? LINESTYLE_SOLID : LINESTYLE_UNSET;
    MajorLongGamma.LineStyle = ShowMajorLongGammaInput.GetYesNo() && data->Greeks.major_long_gamma != 0 ? LINESTYLE_SOLID : LINESTYLE_UNSET;
    MajorShortGamma.LineStyle = ShowMajorShortGammaInput.GetYesNo() && data->Greeks.major_short_gamma != 0 ? LINESTYLE_SOLID : LINESTYLE_UNSET;
}
