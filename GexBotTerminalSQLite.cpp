#include "sierrachart.h"
#include "sqlite3.h"
#include <vector>
#include <string>
#include <oleauto.h>

SCDLLName("GEX_TERMINAL")

struct GammaRow
{
    double ts; // SCDateTime (GetAsDouble) dans le fuseau du chart
    float LG, SG, MP, MN, Z, NV;
};

struct GammaData
{
    std::vector<GammaRow> Rows;

    sqlite3* TodayDB = nullptr;
    double LastRawTs = 0.0;

    std::string LastFolder;
    std::string LastTicker;
    int LastDays = -1;

    SCDateTime LastSystemDay;
    bool Initialized = false;

    // Cache par bougie (pour cohérence historique vs temps réel)
    int LastBarIndex = -1;
    bool HasCachedRow = false;
    GammaRow CachedRow = {}; // évite warning "uninitialized"
};

static bool FileExists(const std::string& p)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (f) { fclose(f); return true; }
    return false;
}

static std::string FormatDateSuffix(const SCDateTime& d)
{
    SYSTEMTIME st = {};
    VariantTimeToSystemTime((DATE)d.GetAsDouble(), &st);
    char buf[32];
    sprintf_s(buf, "%02d.%02d.%04d", st.wMonth, st.wDay, st.wYear);
    return buf;
}

static void ResetAll(GammaData* d)
{
    d->Rows.clear();
    d->LastRawTs = 0.0;
    d->Initialized = false;

    d->LastBarIndex = -1;
    d->HasCachedRow = false;
    d->CachedRow = {};

    if (d->TodayDB)
    {
        sqlite3_close(d->TodayDB);
        d->TodayDB = nullptr;
    }
}

static void LoadIncremental(sqlite3* db, const SCDateTime& TimeScaleAdjustment, GammaData* d)
{
    std::string q =
        "SELECT timestamp, major_long_gamma, major_short_gamma, "
        "major_positive, major_negative, zero_gamma, net_gex_vol "
        "FROM ticker_data ";

    if (d->LastRawTs > 0.0)
        q += "WHERE timestamp > " + std::to_string((long long)d->LastRawTs) + " ";

    q += "ORDER BY timestamp";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, q.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return;

    while (sqlite3_step(st) == SQLITE_ROW)
    {
        double raw = sqlite3_column_double(st, 0); // Unix UTC seconds

        // Unix seconds (UTC) -> SCDateTime (UTC)
        SCDateTime utcTime = SCDateTime(raw / 86400.0 + 25569.0);

        // UTC -> Chart TimeZone via sc.TimeScaleAdjustment
        SCDateTime chartTime = utcTime + TimeScaleAdjustment;

        GammaRow r;
        r.ts = chartTime.GetAsDouble();
        r.LG = (float)sqlite3_column_double(st, 1);
        r.SG = (float)sqlite3_column_double(st, 2);
        r.MP = (float)sqlite3_column_double(st, 3);
        r.MN = (float)sqlite3_column_double(st, 4);
        r.Z = (float)sqlite3_column_double(st, 5);
        r.NV = (float)sqlite3_column_double(st, 6);

        d->Rows.push_back(r);
        d->LastRawTs = raw;
    }

    sqlite3_finalize(st);
}

static bool FindLastInBar(
    const std::vector<GammaRow>& rows,
    double barStart,
    double barEnd,
    double maxAgeDays,
    GammaRow& out)
{
    if (rows.empty())
        return false;

    auto it = std::lower_bound(
        rows.begin(),
        rows.end(),
        barEnd,
        [](const GammaRow& r, double t) { return r.ts < t; });

    if (it == rows.begin())
        return false;

    --it;

    if (it->ts < barStart)
        return false;

    if ((it->ts - barStart) <= maxAgeDays)
    {
        out = *it;
        return true;
    }

    return false;
}

static void EnsureLoaded(
    SCStudyInterfaceRef sc,
    GammaData* d,
    const std::string& folder,
    const std::string& ticker,
    int days)
{
    SCDateTime systemDay = sc.CurrentSystemDateTime.GetDate();

    bool paramsChanged =
        folder != d->LastFolder ||
        ticker != d->LastTicker ||
        days != d->LastDays;

    bool dayChanged =
        d->LastSystemDay != 0 &&
        systemDay != d->LastSystemDay;

    if (!d->Initialized || paramsChanged)
    {
        ResetAll(d);

        d->LastFolder = folder;
        d->LastTicker = ticker;
        d->LastDays = days;
        d->LastSystemDay = systemDay;

        // Chargement historique (jours précédents)
        int loadedDays = 0;
        SCDateTime day = systemDay;

        while (loadedDays < days - 1)
        {
            day = SCDateTime(day.GetAsDouble() - 1);

            int dow = day.GetDayOfWeek();
            if (dow == SATURDAY || dow == SUNDAY)
                continue;

            std::string path =
                folder + "\\Tickers " +
                FormatDateSuffix(day) + "\\" +
                ticker + ".db";

            if (!FileExists(path))
                continue;

            sqlite3* db = nullptr;
            if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK)
            {
                LoadIncremental(db, sc.TimeScaleAdjustment, d);
                sqlite3_close(db);
            }

            loadedDays++;
        }


        d->Initialized = true;
    }

    if (dayChanged)
    {
        if (d->TodayDB)
        {
            sqlite3_close(d->TodayDB);
            d->TodayDB = nullptr;
        }

        d->LastRawTs = 0.0;
        d->LastSystemDay = systemDay;

        // Reset cache bougie (sécurité)
        d->LastBarIndex = -1;
        d->HasCachedRow = false;
        d->CachedRow = {};
    }

    // Ouvrir la DB du jour si dispo
    std::string todayPath =
        folder + "\\Tickers " +
        FormatDateSuffix(systemDay) + "\\" +
        ticker + ".db";

    if (!d->TodayDB && FileExists(todayPath))
    {
        sqlite3_open_v2(todayPath.c_str(), &d->TodayDB, SQLITE_OPEN_READONLY, nullptr);
    }
}

SCSFExport scsf_GEX_TERMINAL(SCStudyInterfaceRef sc)
{
    SCSubgraphRef LG = sc.Subgraph[0];
    SCSubgraphRef SG = sc.Subgraph[1];
    SCSubgraphRef MP = sc.Subgraph[2];
    SCSubgraphRef MN = sc.Subgraph[3];
    SCSubgraphRef Z = sc.Subgraph[4];
    SCSubgraphRef NV = sc.Subgraph[5];

    SCInputRef Folder = sc.Input[0];
    SCInputRef Ticker = sc.Input[1];
    SCInputRef Days = sc.Input[2];
    SCInputRef MaxAgeSec = sc.Input[3];

    if (sc.SetDefaults)
    {
        sc.GraphName = "GEX TERMINAL";
        sc.AutoLoop = 0;
        sc.GraphRegion = 0;

        LG.Name = "LongGamma";  LG.DrawStyle = DRAWSTYLE_DASH; LG.PrimaryColor = RGB(0, 255, 255); LG.LineWidth = 2;
        SG.Name = "ShortGamma"; SG.DrawStyle = DRAWSTYLE_DASH; SG.PrimaryColor = RGB(174, 74, 213); SG.LineWidth = 2;
        MP.Name = "MajorPositive"; MP.DrawStyle = DRAWSTYLE_DASH; MP.PrimaryColor = RGB(0, 255, 0); MP.LineWidth = 1;
        MN.Name = "MajorNegative"; MN.DrawStyle = DRAWSTYLE_DASH; MN.PrimaryColor = RGB(255, 0, 0); MN.LineWidth = 1;
        Z.Name = "Zero"; Z.DrawStyle = DRAWSTYLE_DASH; Z.PrimaryColor = RGB(252, 177, 3); Z.LineWidth = 1;
        NV.Name = "MajorPosVol"; NV.DrawStyle = DRAWSTYLE_DASH; NV.PrimaryColor = RGB(0, 159, 0); NV.LineWidth = 1;

        Folder.Name = "FOLDER";
        Folder.SetString("C:\\Users\\cferry\\Documents\\MAIN\\Trading\\Options\\Data");

        Ticker.Name = "TICKER";
        Ticker.SetString("ES_SPX");

        Days.Name = "NB DAYS";
        Days.SetInt(2);

        MaxAgeSec.Name = "Max age (sec)";
        MaxAgeSec.SetInt(60);

        return;
    }

    if (sc.LastCallToFunction)
    {
        GammaData* d0 = (GammaData*)sc.GetPersistentPointer(0);
        if (d0)
        {
            if (d0->TodayDB) sqlite3_close(d0->TodayDB);
            delete d0;
            sc.SetPersistentPointer(0, nullptr);
        }
        return;
    }

    GammaData* d = (GammaData*)sc.GetPersistentPointer(0);
    if (!d)
    {
        d = new GammaData();
        sc.SetPersistentPointer(0, d);
    }

    EnsureLoaded(sc, d,
        Folder.GetString(),
        Ticker.GetString(),
        Days.GetInt());

    // Lecture DB immédiate (sans Refresh)
    if (d->TodayDB)
        LoadIncremental(d->TodayDB, sc.TimeScaleAdjustment, d);

    const double maxAgeDays = MaxAgeSec.GetInt() / 86400.0;

    int start = sc.UpdateStartIndex;
    if (start < 0) start = 0;

    for (int i = start; i < sc.ArraySize; i++)
    {
        double barStart = sc.BaseDateTimeIn[i].GetAsDouble();
        double barEnd = sc.GetEndingDateTimeForBarIndex(i).GetAsDouble();

        // nouvelle bougie -> reset cache
        if (i != d->LastBarIndex)
        {
            d->LastBarIndex = i;
            d->HasCachedRow = false;
        }

        // capter UNE fois la valeur valide pour cette bougie
        if (!d->HasCachedRow)
        {
            GammaRow r;
            if (FindLastInBar(d->Rows, barStart, barEnd, maxAgeDays, r))
            {
                d->CachedRow = r;
                d->HasCachedRow = true;
            }
        }

        // afficher si on a capturé
        if (d->HasCachedRow)
        {
            LG[i] = d->CachedRow.LG;
            SG[i] = d->CachedRow.SG;
            MP[i] = d->CachedRow.MP;
            MN[i] = d->CachedRow.MN;
            Z[i] = d->CachedRow.Z;
            NV[i] = d->CachedRow.NV;
        }
    }
}
