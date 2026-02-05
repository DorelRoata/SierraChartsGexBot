#include "sierrachart.h"
// #include "sqlite3.h" // Removed to use amalgamation
#include "sqlite3.cpp" // Include implementation directly for single-file build
#include <set>
#include <cfloat>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <oleauto.h>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <sstream>
#include <iomanip>
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

#pragma comment(lib, "winhttp.lib")

SCDLLName("GEX_TERMINAL_API")

// =========================
//        STRUCTURES
// =========================

struct StrikeData
{
    double Strike;
    double Value;
    std::vector<double> Priors;
};

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
    // Données du profil
    std::vector<StrikeData> ProfileData;
    double MaxAbsValue = 1.0;

    // Majors endpoint
    MajorsData Majors;

    // Profile endpoint meta
    ProfileMetaData ProfileMeta;

    // Greeks majors
    GreeksData Greeks;

    // Cache et état
    SCDateTime LastUpdate;
    std::string LastError;
    bool IsProcessing = false;

    // Paramètres de cache
    std::string LastApiKey;
    std::string LastTicker;
    int LastRefreshInterval = -1;

    // Base de données SQLite
    sqlite3* Database = nullptr;
    std::string DatabasePath;

    // Maps historiques pour forward fill (comme Quantower) - utilisant SCDateTime directement
    std::map<SCDateTime, float> zeroMap;          // timestamp -> zero_gamma
    std::map<SCDateTime, float> posVolMap;       // timestamp -> major_pos_vol
    std::map<SCDateTime, float> negVolMap;       // timestamp -> major_neg_vol
    std::map<SCDateTime, float> posOiMap;        // timestamp -> major_pos_oi
    std::map<SCDateTime, float> negOiMap;        // timestamp -> major_neg_oi
    std::map<SCDateTime, float> netMap;          // timestamp -> net (calculated)
    std::map<SCDateTime, float> longMap;        // timestamp -> major_long_gamma
    std::map<SCDateTime, float> shortMap;       // timestamp -> major_short_gamma
    std::map<SCDateTime, float> majPosMap;       // timestamp -> major_positive
    std::map<SCDateTime, float> majNegMap;       // timestamp -> major_negative
    std::map<SCDateTime, float> spotMap;         // timestamp -> spot
    
    std::set<std::string> CachedDBPaths;
    SCDateTime LastRefreshTime;
    
    std::string LastBasePath;
    std::string LastTickerForDB;  // Séparé de LastTicker pour éviter conflit
    int LastDaysCount = -1;
    int LastTZOffset = -999;
    
    bool HistoricalDataLoaded = false;
};

// =========================
//        UTILITAIRES
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
//        HTTP CLIENT
// =========================

std::string HttpGet(const std::string& url, int timeoutSeconds = 30)
{
    std::string result;
    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hRequest = nullptr;

    try
    {
        // Parse URL
        std::string protocol, host, path;
        size_t protocolEnd = url.find("://");
        if (protocolEnd == std::string::npos) return "";

        protocol = url.substr(0, protocolEnd);
        size_t hostStart = protocolEnd + 3;
        size_t pathStart = url.find('/', hostStart);
        
        if (pathStart == std::string::npos)
        {
            host = url.substr(hostStart);
            path = "/";
        }
        else
        {
            host = url.substr(hostStart, pathStart - hostStart);
            path = url.substr(pathStart);
        }

        // Validate protocol
        if (protocol != "http" && protocol != "https") return "";

        // Initialize WinHTTP
        hSession = WinHttpOpen(L"GEX_TERMINAL_API/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);

        if (!hSession) return "";

        // Set timeout
        DWORD timeout = timeoutSeconds * 1000;
        WinHttpSetTimeouts(hSession, timeout, timeout, timeout, timeout);

        // Connect
        std::wstring wHost(host.begin(), host.end());
        hConnect = WinHttpConnect(hSession, wHost.c_str(),
            (protocol == "https") ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);

        if (!hConnect)
        {
            WinHttpCloseHandle(hSession);
            return "";
        }

        // Create request
        std::wstring wPath(path.begin(), path.end());
        hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(),
            NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            (protocol == "https") ? WINHTTP_FLAG_SECURE : 0);

        if (!hRequest)
        {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return "";
        }

        // Send request
        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return "";
        }

        // Receive response
        if (!WinHttpReceiveResponse(hRequest, NULL))
        {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return "";
        }

        // Check HTTP status code
        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX))
        {
            if (statusCode != 200)
            {
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return "";
            }
        }

        // Read data
        DWORD bytesAvailable = 0;
        DWORD bytesRead = 0;
        char buffer[8192]; // Increased buffer size

        do
        {
            if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable))
                break;

            if (bytesAvailable == 0) break;

            DWORD toRead = (bytesAvailable < sizeof(buffer)) ? bytesAvailable : sizeof(buffer);
            if (!WinHttpReadData(hRequest, buffer, toRead, &bytesRead))
                break;

            if (bytesRead > 0)
                result.append(buffer, bytesRead);
        } while (bytesRead > 0);
    }
    catch (...)
    {
        result.clear();
    }

    // Cleanup
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);

    return result;
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

std::vector<std::string> ExtractJsonArray(const std::string& json, const std::string& key)
{
    std::vector<std::string> result;
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return result;

    pos = json.find('[', pos);
    if (pos == std::string::npos) return result;

    size_t start = pos + 1;
    size_t depth = 1;
    pos++;

    while (pos < json.length() && depth > 0)
    {
        if (json[pos] == '[') depth++;
        else if (json[pos] == ']') depth--;
        pos++;
    }

    if (depth == 0)
    {
        std::string arrayContent = json.substr(start, pos - start - 1);

        // Parse array elements
        depth = 0;
        size_t elemStart = 0;
        for (size_t i = 0; i < arrayContent.length(); i++)
        {
            if (arrayContent[i] == '[') depth++;
            else if (arrayContent[i] == ']') depth--;
            else if (arrayContent[i] == ',' && depth == 0)
            {
                std::string elem = arrayContent.substr(elemStart, i - elemStart);
                // Trim
                while (!elem.empty() && (elem.front() == ' ' || elem.front() == '\t'))
                    elem.erase(0, 1);
                while (!elem.empty() && (elem.back() == ' ' || elem.back() == '\t'))
                    elem.pop_back();
                if (!elem.empty()) result.push_back(elem);
                elemStart = i + 1;
            }
        }
        // Last element
        if (elemStart < arrayContent.length())
        {
            std::string elem = arrayContent.substr(elemStart);
            while (!elem.empty() && (elem.front() == ' ' || elem.front() == '\t'))
                elem.erase(0, 1);
            while (!elem.empty() && (elem.back() == ' ' || elem.back() == '\t'))
                elem.pop_back();
            if (!elem.empty()) result.push_back(elem);
        }
    }

    return result;
}

std::vector<double> ParsePriorsArray(const std::string& arrayStr)
{
    std::vector<double> result;
    if (arrayStr.empty() || arrayStr[0] != '[') return result;

    size_t start = 1;
    size_t pos = 1;
    while (pos < arrayStr.length() && arrayStr[pos] != ']')
    {
        if (arrayStr[pos] == ',')
        {
            std::string val = arrayStr.substr(start, pos - start);
            result.push_back(StringToDouble(val));
            start = pos + 1;
        }
        pos++;
    }
    if (start < pos)
    {
        std::string val = arrayStr.substr(start, pos - start);
        result.push_back(StringToDouble(val));
    }

    return result;
}

std::vector<double> ParseStrikeRow(const std::string& rowStr)
{
    std::vector<double> result;
    if (rowStr.empty() || rowStr[0] != '[') return result;

    size_t start = 1;
    size_t pos = 1;
    int depth = 0;
    bool inString = false;
    bool escapeNext = false;

    while (pos < rowStr.length())
    {
        if (escapeNext)
        {
            escapeNext = false;
            pos++;
            continue;
        }

        char c = rowStr[pos];

        if (c == '\\' && inString)
        {
            escapeNext = true;
            pos++;
            continue;
        }

        if (c == '"' && !escapeNext)
        {
            inString = !inString;
            pos++;
            continue;
        }

        if (!inString)
        {
            if (c == '[') depth++;
            else if (c == ']')
            {
                if (depth == 0)
                {
                    // End of array
                    if (start < pos)
                    {
                        std::string val = rowStr.substr(start, pos - start);
                        // Trim
                        while (!val.empty() && (val.front() == ' ' || val.front() == '\t' || val.front() == '\r' || val.front() == '\n'))
                            val.erase(0, 1);
                        while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r' || val.back() == '\n'))
                            val.pop_back();
                        if (!val.empty() && val[0] != '[')
                            result.push_back(StringToDouble(val));
                    }
                    break;
                }
                depth--;
            }
            else if (c == ',' && depth == 0)
            {
                std::string val = rowStr.substr(start, pos - start);
                // Trim
                while (!val.empty() && (val.front() == ' ' || val.front() == '\t' || val.front() == '\r' || val.front() == '\n'))
                    val.erase(0, 1);
                while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r' || val.back() == '\n'))
                    val.pop_back();
                if (!val.empty() && val[0] != '[')
                    result.push_back(StringToDouble(val));
                start = pos + 1;
            }
        }
        pos++;
    }

    return result;
}

// Parse nested array (for priors)
std::vector<double> ParseNestedArray(const std::string& arrayStr, size_t& pos)
{
    std::vector<double> result;
    if (pos >= arrayStr.length() || arrayStr[pos] != '[') return result;

    pos++; // Skip '['
    size_t start = pos;
    int depth = 1;

    while (pos < arrayStr.length() && depth > 0)
    {
        if (arrayStr[pos] == '[') depth++;
        else if (arrayStr[pos] == ']') depth--;
        else if (arrayStr[pos] == ',' && depth == 1)
        {
            std::string val = arrayStr.substr(start, pos - start);
            // Trim
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
                val.erase(0, 1);
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
                val.pop_back();
            if (!val.empty())
                result.push_back(StringToDouble(val));
            start = pos + 1;
        }
        pos++;
    }

    // Last element
    if (start < pos - 1)
    {
        std::string val = arrayStr.substr(start, pos - start - 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
            val.erase(0, 1);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
            val.pop_back();
        if (!val.empty())
            result.push_back(StringToDouble(val));
    }

    return result;
}

// =========================
//        API FETCH
// =========================

bool FetchProfile(SCStudyInterfaceRef sc, GammaData* data, const std::string& ticker, 
    const std::string& type, const std::string& agg, const std::string& apiKey, bool useOi)
{
    std::string model = (type == "State" || type == "state") ? "state" : "classic";
    std::string url = "https://api.gexbot.com/" + UrlEncode(ticker) + "/" + model + "/" + 
        UrlEncode(agg) + "?key=" + UrlEncode(apiKey);

    std::string response = HttpGet(url, 30);
    if (response.empty())
    {
        data->LastError = "HTTP request failed or empty response";
        return false;
    }

    // Check for error in JSON response
    std::string errorMsg = ExtractJsonValue(response, "error");
    if (!errorMsg.empty())
    {
        data->LastError = "API error: " + errorMsg;
        return false;
    }

    // Parse strikes array
    std::vector<std::string> strikesArray = ExtractJsonArray(response, "strikes");

    data->ProfileData.clear();
    double maxAbs = 1.0;

    for (const std::string& rowStr : strikesArray)
    {
        std::vector<double> row = ParseStrikeRow(rowStr);
        if (row.size() < 2) continue;

        StrikeData strike;
        strike.Strike = row[0];

        double gexVol = row.size() > 1 ? row[1] : 0.0;
        double gexOi = row.size() > 2 ? row[2] : 0.0;

        strike.Value = useOi ? gexOi : gexVol;

        // Try to parse priors from the original JSON string if available
        // Priors are typically at index 3 as a nested array
        if (rowStr.length() > 0)
        {
            size_t priorsPos = 0;
            size_t commaCount = 0;
            for (size_t i = 0; i < rowStr.length(); i++)
            {
                if (rowStr[i] == ',' && commaCount == 2)
                {
                    priorsPos = i + 1;
                    break;
                }
                if (rowStr[i] == ',') commaCount++;
            }

            if (priorsPos > 0 && priorsPos < rowStr.length() && rowStr[priorsPos] == '[')
            {
                strike.Priors = ParseNestedArray(rowStr, priorsPos);
            }
        }

        data->ProfileData.push_back(strike);
        maxAbs = (maxAbs > fabs(strike.Value)) ? maxAbs : fabs(strike.Value);
    }

    data->MaxAbsValue = (maxAbs > 0) ? maxAbs : 1.0;

    // Parse meta data
    data->ProfileMeta.zero_gamma = StringToDouble(ExtractJsonValue(response, "zero_gamma"));
    data->ProfileMeta.sum_gex_vol = StringToDouble(ExtractJsonValue(response, "sum_gex_vol"));
    data->ProfileMeta.sum_gex_oi = StringToDouble(ExtractJsonValue(response, "sum_gex_oi"));
    data->ProfileMeta.delta_risk_reversal = StringToDouble(ExtractJsonValue(response, "delta_risk_reversal"));

    return true;
}

bool FetchMajors(SCStudyInterfaceRef sc, GammaData* data, const std::string& ticker,
    const std::string& type, const std::string& agg, const std::string& apiKey)
{
    std::string model = (type == "State" || type == "state") ? "state" : "classic";
    std::string url = "https://api.gexbot.com/" + UrlEncode(ticker) + "/" + model + "/" +
        UrlEncode(agg) + "/majors?key=" + UrlEncode(apiKey);

    std::string response = HttpGet(url, 30);
    if (response.empty())
    {
        data->LastError = "Majors HTTP request failed or empty response";
        return false;
    }

    // Check for error in JSON response
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

bool FetchGreeks(SCStudyInterfaceRef sc, GammaData* data, const std::string& ticker,
    const std::string& greek, const std::string& agg, const std::string& apiKey)
{
    std::string greekKey = greek + "_" + agg;
    std::string url = "https://api.gexbot.com/" + UrlEncode(ticker) + "/state/" +
        UrlEncode(greekKey) + "?key=" + UrlEncode(apiKey);

    std::string response = HttpGet(url, 30);
    if (response.empty())
    {
        data->LastError = "Greeks HTTP request failed or empty response";
        return false;
    }

    // Check for error in JSON response
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

    // Parse mini_contracts for profile
    std::vector<std::string> miniArray = ExtractJsonArray(response, "mini_contracts");

    data->ProfileData.clear();
    double maxAbs = 1.0;

    for (const std::string& rowStr : miniArray)
    {
        std::vector<double> row = ParseStrikeRow(rowStr);
        if (row.size() < 4) continue;

        StrikeData strike;
        strike.Strike = row[0];
        strike.Value = row.size() > 3 ? row[3] : 0.0; // Value is typically at index 3 for greeks

        // Parse priors from nested array (typically at index 4)
        if (rowStr.length() > 0)
        {
            size_t priorsPos = 0;
            size_t commaCount = 0;
            for (size_t i = 0; i < rowStr.length(); i++)
            {
                if (rowStr[i] == ',' && commaCount == 3)
                {
                    priorsPos = i + 1;
                    break;
                }
                if (rowStr[i] == ',') commaCount++;
            }

            if (priorsPos > 0 && priorsPos < rowStr.length() && rowStr[priorsPos] == '[')
            {
                strike.Priors = ParseNestedArray(rowStr, priorsPos);
            }
        }

        data->ProfileData.push_back(strike);
        maxAbs = (maxAbs > fabs(strike.Value)) ? maxAbs : fabs(strike.Value);
    }

    data->MaxAbsValue = (maxAbs > 0) ? maxAbs : 1.0;

    return true;
}

// =========================
//        SQLite WRITE
// =========================

bool InitializeDatabase(SCStudyInterfaceRef sc, GammaData* data, const std::string& dbPath)
{
    if (dbPath.empty()) return false;

    if (data->Database && data->DatabasePath == dbPath)
        return true;

    if (data->Database)
    {
        sqlite3_close(data->Database);
        data->Database = nullptr;
    }

    // Create directory if it doesn't exist
    size_t lastSlash = dbPath.find_last_of("\\/");
    if (lastSlash != std::string::npos)
    {
        std::string dirPath = dbPath.substr(0, lastSlash);
        if (!dirPath.empty())
        {
            // Create directory recursively
            std::wstring wDirPath(dirPath.begin(), dirPath.end());
            DWORD dwAttrib = GetFileAttributesW(wDirPath.c_str());
            if (dwAttrib == INVALID_FILE_ATTRIBUTES)
            {
                // Try to create directory
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
        }
    }

    if (sqlite3_open(dbPath.c_str(), &data->Database) != SQLITE_OK)
    {
        if (data->Database)
        {
            data->LastError = "Failed to open database: " + std::string(sqlite3_errmsg(data->Database));
            sqlite3_close(data->Database);
            data->Database = nullptr;
        }
        else
        {
            data->LastError = "Failed to open database: out of memory";
        }
        return false;
    }

    data->DatabasePath = dbPath;

    // Create table if not exists (structure exacte Quantower)
    const char* createTableSQL =
        "CREATE TABLE IF NOT EXISTS ticker_data ("
        "timestamp REAL PRIMARY KEY,"
        "spot REAL,"
        "zero_gamma REAL,"
        "major_pos_vol REAL,"
        "major_neg_vol REAL,"
        "major_pos_oi REAL,"
        "major_neg_oi REAL,"
        "sum_gex_vol REAL,"
        "sum_gex_oi REAL,"
        "delta_risk_reversal REAL,"
        "major_long_gamma REAL,"
        "major_short_gamma REAL,"
        "major_positive REAL,"
        "major_negative REAL,"
        "net REAL"
        ");";

    char* errMsg = nullptr;
    if (sqlite3_exec(data->Database, createTableSQL, nullptr, nullptr, &errMsg) != SQLITE_OK)
    {
        data->LastError = "Failed to create table: " + std::string(errMsg);
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

bool WriteToDatabase(SCStudyInterfaceRef sc, GammaData* data)
{
    if (!data->Database) return false;

    // Convert SCDateTime to Unix timestamp
    double scDateTime = sc.CurrentSystemDateTime.GetAsDouble();
    double unixTimestamp = (scDateTime - 25569.0) * 86400.0;

    // Get spot price from chart (use Close price as spot)
    double spot = 0.0;
    if (sc.BaseData[SC_CLOSE].GetArraySize() > 0 && sc.Index >= 0)
    {
        spot = sc.BaseData[SC_CLOSE][sc.Index];
    }

    // Calculate net GEX
    double netGex = data->Majors.mpos_vol - fabs(data->Majors.mneg_vol);
    double zeroGamma = data->ProfileMeta.zero_gamma != 0 ? data->ProfileMeta.zero_gamma : data->Majors.zero_gamma;

    // Structure exacte Quantower (même ordre que dans CREATE TABLE)
    const char* insertSQL =
        "INSERT OR REPLACE INTO ticker_data ("
        "timestamp, spot, zero_gamma,"
        "major_pos_vol, major_neg_vol,"
        "major_pos_oi, major_neg_oi,"
        "sum_gex_vol, sum_gex_oi,"
        "delta_risk_reversal,"
        "major_long_gamma, major_short_gamma,"
        "major_positive, major_negative,"
        "net"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(data->Database, insertSQL, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    // Calculer les valeurs supplémentaires
    double sumGexVol = data->ProfileMeta.sum_gex_vol;
    double sumGexOi = data->ProfileMeta.sum_gex_oi;
    double deltaRiskReversal = data->ProfileMeta.delta_risk_reversal;
    double net = spot + netGex / 100.0;

    // Bind dans l'ordre exact de la table
    sqlite3_bind_double(stmt, 1, unixTimestamp);  // timestamp
    sqlite3_bind_double(stmt, 2, spot);           // spot
    sqlite3_bind_double(stmt, 3, zeroGamma);      // zero_gamma
    sqlite3_bind_double(stmt, 4, data->Majors.mpos_vol);  // major_pos_vol
    sqlite3_bind_double(stmt, 5, data->Majors.mneg_vol);  // major_neg_vol
    sqlite3_bind_double(stmt, 6, data->Majors.mpos_oi);    // major_pos_oi
    sqlite3_bind_double(stmt, 7, data->Majors.mneg_oi);    // major_neg_oi
    sqlite3_bind_double(stmt, 8, sumGexVol);       // sum_gex_vol
    sqlite3_bind_double(stmt, 9, sumGexOi);        // sum_gex_oi
    sqlite3_bind_double(stmt, 10, deltaRiskReversal); // delta_risk_reversal
    sqlite3_bind_double(stmt, 11, data->Greeks.major_long_gamma);  // major_long_gamma
    sqlite3_bind_double(stmt, 12, data->Greeks.major_short_gamma); // major_short_gamma
    sqlite3_bind_double(stmt, 13, data->Greeks.major_positive);    // major_positive
    sqlite3_bind_double(stmt, 14, data->Greeks.major_negative);   // major_negative
    sqlite3_bind_double(stmt, 15, net);            // net

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    return success;
}

// Helper pour formater la date au format MM.dd.yyyy
std::string FormatDateSuffix(const SCDateTime& dt)
{
    SYSTEMTIME st = {};
    double dtDouble = dt.GetAsDouble();
    DATE date = static_cast<DATE>(dtDouble);
    VariantTimeToSystemTime(date, &st);
    
    char buffer[20];
    sprintf_s(buffer, sizeof(buffer), "%02d.%02d.%04d", st.wMonth, st.wDay, st.wYear);
    return buffer;
}

// Helper pour soustraire des jours
SCDateTime SubtractDays(SCDateTime base, int days)
{
    double adjusted = base.GetAsDouble() - days;
    return SCDateTime(adjusted);
}

// Forward fill: trouve la valeur la plus récente <= targetTime avec tolérance de 60 secondes
float GetValueAtTime(const std::map<SCDateTime, float>& map, SCDateTime targetTime)
{
    if (map.empty()) return -FLT_MAX;

    auto it = map.upper_bound(targetTime);
    if (it == map.begin()) return -FLT_MAX;

    --it;
    
    // Vérifier que la différence de temps n'est pas trop grande
    // Tolérance de 5 minutes pour gérer les décalages de fuseau horaire et les données non alignées
    double delta = fabs((targetTime - it->first).GetAsDouble() * 86400.0);
    if (delta <= 300.0) // 5 minutes de tolérance
        return it->second;
    
    return -FLT_MAX;
}

// Charge toutes les données historiques depuis la DB (compatible Quantower)
int LoadSingleDB(SCStudyInterfaceRef sc, const std::string& fullPath, int tzOffsetHours, GammaData* data)
{
    int rowCount = 0;
    sqlite3* db = nullptr;

    if (sqlite3_open(fullPath.c_str(), &db) != SQLITE_OK)
    {
        SCString msg;
        msg.Format("GEX_TERMINAL: Impossible d'ouvrir %s", fullPath.c_str());
        sc.AddMessageToLog(msg, 0);
        return 0;
    }

    // Structure Quantower réelle (selon le schéma fourni par l'utilisateur)
    // Colonnes: timestamp, spot, zero_gamma, major_pos_vol, major_neg_vol, major_pos_oi, major_neg_oi,
    //           major_long_gamma, major_short_gamma, major_positive, major_negative, net, etc.
    const char* query = 
        "SELECT timestamp, "
        "COALESCE(spot, 0) AS spot, "
        "COALESCE(zero_gamma, 0) AS zero_val, "
        "COALESCE(major_pos_vol, 0) AS posvol_val, "
        "COALESCE(major_neg_vol, 0) AS negvol_val, "
        "COALESCE(major_pos_oi, 0) AS posoi_val, "
        "COALESCE(major_neg_oi, 0) AS negoi_val, "
        "COALESCE(major_long_gamma, 0) AS long_val, "
        "COALESCE(major_short_gamma, 0) AS short_val, "
        "COALESCE(major_positive, 0) AS majpos_val, "
        "COALESCE(major_negative, 0) AS majneg_val "
        "FROM ticker_data;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) != SQLITE_OK)
    {
        const char* errMsg = sqlite3_errmsg(db);
        SCString msg;
        msg.Format("GEX_TERMINAL: Erreur SQL pour %s: %s", fullPath.c_str(), errMsg ? errMsg : "unknown");
        sc.AddMessageToLog(msg, 0);
        sqlite3_close(db);
        return 0;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        double ts = sqlite3_column_double(stmt, 0);
        if (ts <= 0) continue;

        // Convertir timestamp Unix en SCDateTime avec offset de fuseau horaire
        // Ajouter l'offset AVANT la conversion (comme dans le code de référence)
        SCDateTime dt((ts + tzOffsetHours * 3600.0) / 86400.0 + 25569.0);

        // Ordre des colonnes dans la requête:
        // 0: timestamp, 1: spot, 2: long_val, 3: short_val, 4: majpos_val, 5: majneg_val,
        // 6: zero_val, 7: posvol_val, 8: negvol_val, 9: posoi_val, 10: negoi_val
        double spot = sqlite3_column_double(stmt, 1);
        double lng = sqlite3_column_double(stmt, 2);
        double sht = sqlite3_column_double(stmt, 3);
        double mjp = sqlite3_column_double(stmt, 4);
        double mjn = sqlite3_column_double(stmt, 5);
        double zero = sqlite3_column_double(stmt, 6);
        double posv = sqlite3_column_double(stmt, 7);
        double negv = sqlite3_column_double(stmt, 8);
        double posoi = sqlite3_column_double(stmt, 9);
        double negoi = sqlite3_column_double(stmt, 10);

        // Stocker dans les maps avec SCDateTime directement
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

        // Calculer net (spot + netGex / 100.0)
        if (!std::isnan(spot) && !std::isnan(posv) && !std::isnan(negv))
        {
            double netGex = posv - fabs(negv);
            data->netMap[dt] = static_cast<float>(spot + netGex / 100.0);
        }

        ++rowCount;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (rowCount > 0)
    {
        SCString msg;
        msg.Format("GEX_TERMINAL: Chargé %d lignes depuis %s", rowCount, fullPath.c_str());
        sc.AddMessageToLog(msg, 0);
    }

    return rowCount;
}

// Helper pour vérifier si un fichier existe
bool FileExists(const std::string& path)
{
    FILE* file = fopen(path.c_str(), "rb");
    if (file)
    {
        fclose(file);
        return true;
    }
    return false;
}

// Charge les fichiers DB récents (comme le code de référence)
void LoadRecentGammaFiles(SCStudyInterfaceRef sc, GammaData* data, const std::string& baseFolder, const std::string& ticker, int days, int tzOffset, int refreshSeconds)
{
    // Nettoyer si les paramètres ont changé
    if (baseFolder != data->LastBasePath || ticker != data->LastTickerForDB || days != data->LastDaysCount || tzOffset != data->LastTZOffset)
    {
        data->CachedDBPaths.clear();
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
        data->LastTickerForDB = ticker;
        data->LastDaysCount = days;
        data->LastTZOffset = tzOffset;
    }

    // Utiliser la date actuelle comme référence (pas la dernière barre du graphique)
    SCDateTime reference = sc.GetCurrentDateTime();

    int totalRowsLoaded = 0;
    for (int i = days - 1; i >= 0; --i)
    {
        SCDateTime date = SubtractDays(reference, i);
        std::string suffix = FormatDateSuffix(date);
        std::string path = baseFolder + "\\Tickers " + suffix + "\\" + ticker + ".db";

        bool isToday = (i == 0);
        bool isAlreadyLoaded = data->CachedDBPaths.count(path) > 0;
        bool refreshDue = (sc.CurrentSystemDateTime - data->LastRefreshTime).GetAsDouble() > (refreshSeconds / 86400.0);

        if (FileExists(path))
        {
            if (isToday && refreshDue)
            {
                int rows = LoadSingleDB(sc, path, tzOffset, data);
                data->LastRefreshTime = sc.CurrentSystemDateTime;
                totalRowsLoaded += rows;
            }
            else if (!isToday && !isAlreadyLoaded)
            {
                int rows = LoadSingleDB(sc, path, tzOffset, data);
                if (rows > 0)
                {
                    data->CachedDBPaths.insert(path);
                    totalRowsLoaded += rows;
                }
            }
        }
        else
        {
            // Log pour debug - fichier non trouvé (seulement pour les premiers jours)
            if (i < 3) // Log pour les 3 premiers jours seulement
            {
                SCString msg;
                msg.Format("GEX_TERMINAL: Fichier non trouvé: %s", path.c_str());
                sc.AddMessageToLog(msg, 0);
            }
        }
    }
    
    // Log récapitulatif
    if (totalRowsLoaded > 0)
    {
        SCString msg;
        msg.Format("GEX_TERMINAL: Total chargé: %d lignes, Zero points: %d", totalRowsLoaded, data->zeroMap.size());
        sc.AddMessageToLog(msg, 0);
    }
}

// =========================
//        MAIN FUNCTION
// =========================

void FetchAllData(SCStudyInterfaceRef sc, GammaData* data)
{
    if (data->IsProcessing) return;
    data->IsProcessing = true;

    std::string apiKey = sc.Input[0].GetString();
    std::string ticker = sc.Input[1].GetString();
    int refreshInterval = sc.Input[2].GetInt();
    
    // Comme Quantower: toujours utiliser "zero" comme aggregation
    std::string aggregation = "zero";

    if (apiKey.empty() || apiKey == "YOUR_API_KEY")
    {
        data->LastError = "API key manquante";
        data->IsProcessing = false;
        return;
    }

    // Check if refresh needed
    bool needsRefresh = false;
    if (apiKey != data->LastApiKey || ticker != data->LastTicker || refreshInterval != data->LastRefreshInterval)
    {
        needsRefresh = true;
    }
    else
    {
        double timeSinceUpdate = (sc.CurrentSystemDateTime - data->LastUpdate).GetAsDouble() * 86400.0;
        if (timeSinceUpdate >= refreshInterval)
            needsRefresh = true;
    }

    if (!needsRefresh)
    {
        data->IsProcessing = false;
        return;
    }

    // Comme dans Quantower: appeler TOUS les endpoints automatiquement (classic ET state)
    // Aggregation toujours "zero"
    
    // Classic d'abord (toujours disponible dans l'abonnement classic) - Volume ET OI
    FetchMajors(sc, data, ticker, "classic", aggregation, apiKey);

    // OPTIMIZED: Removed duplicate call
    FetchProfile(sc, data, ticker, "classic", aggregation, apiKey, false); // false = Volume
    // FetchProfile(sc, data, ticker, "classic", aggregation, apiKey, true); // REMOVED redundant second call
    
    // Ensuite tester State (si disponible dans la clé API)
    std::string stateUrl = "https://api.gexbot.com/" + UrlEncode(ticker) + "/state/" +
        UrlEncode(aggregation) + "?key=" + UrlEncode(apiKey);
    std::string stateResponse = HttpGet(stateUrl, 30);
    
    // Vérifier si state est disponible (pas d'erreur dans la réponse)
    bool stateAvailable = (!stateResponse.empty() && 
                           stateResponse.find("\"error\"") == std::string::npos &&
                           stateResponse.find("access denied") == std::string::npos &&
                           stateResponse.find("Access Denied") == std::string::npos);
    
    if (stateAvailable)
    {
        // State est disponible, récupérer les données state
        FetchGreeks(sc, data, ticker, "GEX", aggregation, apiKey);
        
        // OPTIMIZED: Reuse stateResponse instead of fetching again
        data->ProfileMeta.zero_gamma = StringToDouble(ExtractJsonValue(stateResponse, "zero_gamma"));
        data->ProfileMeta.sum_gex_vol = StringToDouble(ExtractJsonValue(stateResponse, "sum_gex_vol"));
        data->ProfileMeta.sum_gex_oi = StringToDouble(ExtractJsonValue(stateResponse, "sum_gex_oi"));
        data->ProfileMeta.delta_risk_reversal = StringToDouble(ExtractJsonValue(stateResponse, "delta_risk_reversal"));
    }
    // Si state n'est pas disponible (accès refusé), on utilise seulement classic (déjà chargé)

    // Write to database if enabled
    std::string dbPath = sc.Input[5].GetString(); // DatabaseWritePathInput est maintenant à l'index 5
    if (!dbPath.empty())
    {
        if (InitializeDatabase(sc, data, dbPath))
        {
            if (!WriteToDatabase(sc, data))
            {
                // Log error but don't fail the entire operation
                data->LastError = "Database write failed (check log)";
            }
        }
    }

    // Update cache
    data->LastApiKey = apiKey;
    data->LastTicker = ticker;
    data->LastRefreshInterval = refreshInterval;
    data->LastUpdate = sc.CurrentSystemDateTime;
    data->LastError = "OK";

    data->IsProcessing = false;
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
    SCInputRef MultiplierInput = sc.Input[2]; // Multiplicateur pour ajuster les niveaux
    SCInputRef RefreshIntervalInput = sc.Input[3];
    SCInputRef DatabaseReadPathInput = sc.Input[4];
    SCInputRef DatabaseWritePathInput = sc.Input[5];
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
        MultiplierInput.SetFloatLimits(0.001, 1000.0);
        RefreshIntervalInput.Name = "Refresh (seconds)"; RefreshIntervalInput.SetInt(10);
        RefreshIntervalInput.SetIntLimits(1, 3600);
        DatabaseReadPathInput.Name = "DB Read Path (e.g. C:\\Quantower\\GexBot)"; DatabaseReadPathInput.SetString("");
        DatabaseWritePathInput.Name = "DB Write Path (e.g. C:\\SierraChart\\GexBot)"; DatabaseWritePathInput.SetString("");
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
            if (data->Database)
            {
                sqlite3_close(data->Database);
                data->Database = nullptr;
            }
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

    // Récupérer les paramètres (déclarer au début pour être accessible partout)
    std::string ticker = TickerInput.GetString();
    float multiplier = MultiplierInput.GetFloat();
    
    // Charger les données historiques UNE SEULE FOIS au démarrage ou si les paramètres changent
    std::string readDbPath = DatabaseReadPathInput.GetString();
    int daysToLoad = DaysToLoadInput.GetInt();
    int tzOffset = TZOffsetInput.GetInt();
    int refreshInterval = RefreshIntervalInput.GetInt();
    
    // Vérifier si les paramètres ont changé ou si c'est la première fois
    bool paramsChanged = (readDbPath != data->LastBasePath || 
                         ticker != data->LastTickerForDB || 
                         daysToLoad != data->LastDaysCount || 
                         tzOffset != data->LastTZOffset);
    
    if (!readDbPath.empty() && !ticker.empty() && (paramsChanged || !data->HistoricalDataLoaded))
    {
        // Charger les données historiques (une seule fois ou si paramètres changent)
        LoadRecentGammaFiles(sc, data, readDbPath, ticker, daysToLoad, tzOffset, refreshInterval);
        data->HistoricalDataLoaded = true; // Marquer comme chargé
    }

    // Pour la barre actuelle (live), récupérer depuis l'API et mettre à jour les maps
    bool isHistoricalBar = (sc.Index < sc.ArraySize - 1);
    if (!isHistoricalBar)
    {
        // Fetch depuis l'API
        FetchAllData(sc, data);
        
        // Mettre à jour les maps avec les nouvelles données (utiliser SCDateTime)
        SCDateTime scDateTime = sc.CurrentSystemDateTime;
        float zeroGamma = (data->ProfileMeta.zero_gamma != 0) ? static_cast<float>(data->ProfileMeta.zero_gamma) : static_cast<float>(data->Majors.zero_gamma);
        
        if (zeroGamma != 0) data->zeroMap[scDateTime] = zeroGamma;
        if (data->Majors.mpos_vol != 0) data->posVolMap[scDateTime] = static_cast<float>(data->Majors.mpos_vol);
        if (data->Majors.mneg_vol != 0) data->negVolMap[scDateTime] = static_cast<float>(data->Majors.mneg_vol);
        if (data->Majors.mpos_oi != 0) data->posOiMap[scDateTime] = static_cast<float>(data->Majors.mpos_oi);
        if (data->Majors.mneg_oi != 0) data->negOiMap[scDateTime] = static_cast<float>(data->Majors.mneg_oi);
        if (data->Greeks.major_long_gamma != 0) data->longMap[scDateTime] = static_cast<float>(data->Greeks.major_long_gamma);
        if (data->Greeks.major_short_gamma != 0) data->shortMap[scDateTime] = static_cast<float>(data->Greeks.major_short_gamma);
        if (data->Greeks.major_positive != 0) data->majPosMap[scDateTime] = static_cast<float>(data->Greeks.major_positive);
        if (data->Greeks.major_negative != 0) data->majNegMap[scDateTime] = static_cast<float>(data->Greeks.major_negative);
        
        // Calculer net
        if (sc.BaseData[SC_CLOSE].GetArraySize() > 0 && sc.Index >= 0)
        {
            float spot = static_cast<float>(sc.BaseData[SC_CLOSE][sc.Index]);
            if (spot != 0 && data->Majors.mpos_vol != 0 && data->Majors.mneg_vol != 0)
            {
                float netGex = static_cast<float>(data->Majors.mpos_vol - fabs(data->Majors.mneg_vol));
                data->netMap[scDateTime] = spot + netGex / 100.0f;
            }
        }
        
        // Écrire dans la DB (chemin d'écriture séparé)
        std::string writeDbPath = DatabaseWritePathInput.GetString();
        if (!writeDbPath.empty() && !ticker.empty())
        {
            // Construire le chemin de la DB pour aujourd'hui
            SCDateTime today = sc.GetCurrentDateTime();
            int year = today.GetYear();
            int month = today.GetMonth();
            int day = today.GetDay();
            char dateStr[32];
            sprintf_s(dateStr, sizeof(dateStr), "%02d.%02d.%04d", month, day, year);
            std::string todayDbPath = writeDbPath + "\\Tickers " + dateStr + "\\" + ticker + ".db";
            
            if (InitializeDatabase(sc, data, todayDbPath))
            {
                WriteToDatabase(sc, data);
            }
        }
    }

    // Obtenir les valeurs avec forward fill depuis les maps
    SCDateTime now = sc.BaseDateTimeIn[sc.Index];
    
    // Debug: pour la première barre, log le nombre de points chargés
    if (sc.Index == 0 && !data->zeroMap.empty())
    {
        SCString msg;
        msg.Format("GEX_TERMINAL: Maps chargées - Zero: %d, PosVol: %d, Long: %d", 
                   data->zeroMap.size(), data->posVolMap.size(), data->longMap.size());
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

    // Appliquer le multiplicateur aux valeurs
    // Set subgraph values (utiliser les valeurs des maps ou les valeurs actuelles si pas de données historiques)
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
