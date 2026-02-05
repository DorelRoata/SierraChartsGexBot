# Code Review: GexBot Terminal API

This document outlines the findings from a code review of `GexBotTerminalAPI.cpp`.

## Critical Issues

### 1. Blocking HTTP Calls
The study uses `WinHttpSendRequest` and `WinHttpReceiveResponse` (synchronous) directly within the main study function execution path.
- **Impact**: This will **freeze the Sierra Chart User Interface** for the duration of the network request (potentially hundreds of milliseconds or seconds).
- **Recommendation**: Refactor to use Sierra Chart's asynchronous `sc.MakeHTTPRequest` function. This requires handling the response in a separate event (e.g., `sc.HTTPRequestID`), ensuring the UI remains responsive.

## Performance & Optimization

### 2. Redundant API Calls
The code performs multiple identical network requests, wasting bandwidth and increasing latency.

*   **Duplicate `FetchProfile` calls**:
    ```cpp
    FetchProfile(sc, data, ticker, "classic", aggregation, apiKey, false); // false = Volume
    FetchProfile(sc, data, ticker, "classic", aggregation, apiKey, true);  // true = OI
    ```
    Both calls fetch the **exact same URL**. The second call completely overwrites the data from the first call. Additionally, the specific data being differentiated (`gexVol` vs `gexOi`) is stored in `ProfileData` which is unused (see below).

*   **Redundant "State" Endpoint Fetch**:
    ```cpp
    std::string stateUrl = ...;
    std::string stateResponse = HttpGet(stateUrl, 30);
    // ...
    if (stateAvailable) {
        // ...
        std::string profileUrl = ...; // Identical to stateUrl
        std::string profileResponse = HttpGet(profileUrl, 30); // Fetches SAME URL again
    }
    ```
    The code fetches the state URL to check availability, then fetches it again to parse metadata. The first response should be reused.

### 3. Dead Code & Unused Data (`ProfileData`)
The `GammaData` structure contains a `std::vector<StrikeData> ProfileData`.
-   This vector is cleared and populated with parsed strike data in `FetchProfile` and `FetchGreeks`.
-   **Finding**: This vector is **never read** or used anywhere in the study logic (`scsf_GexBotAPI` or `WriteToDatabase`).
-   **Impact**: Wasted CPU cycles parsing the JSON arrays and wasted memory storing the strikes.
-   **Recommendation**: Remove `ProfileData` and the associated parsing logic.

## Code Quality & Maintainability

### 4. Manual JSON Parsing
The project uses custom string manipulation functions (`ExtractJsonValue`, `ExtractJsonArray`, `ParseStrikeRow`) to parse JSON.
-   **Risk**: This approach is fragile. Unexpected whitespace or JSON formatting changes by the API provider could break the parser.
-   **Recommendation**: Consider using a lightweight, header-only JSON library like `nlohmann/json` or `picojson` for more robust parsing.

### 5. Memory Management
-   The code allocates `GammaData` using `new` and correctly deletes it when `sc.LastCallToFunction` is true. This is correct for Sierra Chart studies.

## Summary of Planned Fixes
1.  **Remove Dead Code**: Delete `ProfileData` and associated parsing logic.
2.  **Optimize Network Calls**:
    -   Remove the duplicate `FetchProfile` call.
    -   Reuse the `stateResponse` string for metadata parsing instead of re-fetching.
    -   Only parse `ProfileMeta` since `ProfileData` is unused.
