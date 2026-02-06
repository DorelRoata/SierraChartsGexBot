# SierraChart GexBot Terminal API

This project contains a custom C++ study for **Sierra Chart** that integrates with the **GexBot API**. It is designed to fetch, process, and visualize real-time and historical Gamma Exposure (GEX) data directly on your trading charts.

## Features

*   **Real-time API Integration**: Fetches Majors, Profile, and Greeks data from `api.gexbot.com` using Sierra Chart's Async HTTP.
*   **Local Data Caching**: Uses **CSV Files** to store historical data locally, enabling fast retrieval and "forward fill" logic for days without immediate API data.
*   **Comprehensive Visualization**: Plots key metrics as subgraphs:
    *   Zero Gamma Levels
    *   Major Positive/Negative Vol & OI
    *   Long/Short Gamma
    *   Greeks (Major Positive/Negative)
*   **Customizable**: User inputs for API Key, Ticker, Refresh Intervals, and Database Paths.

## Architecture

The following diagram illustrates the high-level architecture of the system:

```mermaid
graph TD
    subgraph "External"
        API[GexBot API<br/>api.gexbot.com]
    end

    subgraph "Local Workstation"
        direction TB
        
        subgraph "Sierra Chart Environment"
            SC[Sierra Chart Terminal]
            Study[Custom Study<br/>GEX_TERMINAL_API]
        end

        subgraph "Local Storage"
            DB[(CSV Storage<br/>.csv files)]
        end
    end

    %% Data Flow
    API -- "HTTP GET (JSON)" --> Study
    Study -- "Parse & Process" --> SC
    Study -- "Read/Write History" --> DB
    
    %% Internal Study Components
    subgraph "Study Components"
        HTTP[SC Async HTTP]
        JSON[JSON Parser]
        Cache[Data Structures<br/>Maps & Vectors]
        Renderer[Subgraph Renderer]
    end
    
    Study --- HTTP
    Study --- JSON
    Study --- Cache
    Study --- Renderer
```

## Setup & Usage

1.  **Compile**: Use the Sierra Chart C++ compiler to build `GexBotTerminalAPI.cpp`.
2.  **Add to Chart**: Add the **GEX BOT API** study to your chart.
3.  **Configuration**:
    *   **API Key**: Enter your valid GexBot API key.
    *   **Ticker**: Specify the ticker (e.g., `ES_SPX`).
    *   **CSV Paths**: Set valid local paths for reading/writing the CSV files (e.g., `C:\GexBot\Data`).
