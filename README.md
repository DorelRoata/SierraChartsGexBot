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

The system is designed with a **Hub-and-Spoke** architecture to minimize API usage and file conflicts while allowing unlimited chart instances.

```mermaid
graph TD
    subgraph "External"
        Cloud[GexBot API Cloud]
    end

    subgraph "Master Chart"
        SourceStudy[**GEX BOT API**<br/>(Original DLL)]
        Collector[**GexBot Data Collector**<br/>(Scraper)]
        SourceStudy -- "Subgraphs (SG1-SG9)" --> Collector
    end

    subgraph "Local Storage"
        CSV[(**CSV Repository**<br/>Daily Files)]
        Collector -- "Writes (Append Mode)" --> CSV
    end

    subgraph "Client Charts (Unlimited)"
        Viewer1[**GexBot CSV Viewer**<br/>Chart 2]
        Viewer2[**GexBot CSV Viewer**<br/>Chart 3]
        ViewerN[**GexBot CSV Viewer**<br/>Chart N]
    end

    %% Data Flow
    Cloud -- "HTTP JSON" --> SourceStudy
    CSV -- "Read Polling" --> Viewer1
    CSV -- "Read Polling" --> Viewer2
    CSV -- "Read Polling" --> ViewerN
```

## Setup & Usage

### 1. Master Chart (The Source)
*   **Study 1**: Add the original `GEX BOT API` (or Binary DLL). Configure it with your valid API Key.
*   **Study 2**: Add `GexBotDataCollector`.
    *   **Source Chart ID**: Set to the Chart ID of this Master Chart.
    *   **Output Path**: Set to a central folder (e.g., `C:\GexBot\Data`).
    *   **Tickers**: Ensure the ticker name matches correctly.

### 2. Client Charts (The Viewers)
*   **Study**: Add `GexBotCSVViewer`.
*   **CSV Path**: Set to the *same* Output Path used in the Collector.
*   **Refresh Interval**: Default is 10s. The viewer will automatically check for updates.
*   **Benefit**: These charts use **ZERO** API credits and have **NO** networking overhead.

## Proposed Subgraph Mapping & CSV Structure

The following mapping is planned for future updates to align the CSV output and Subgraphs with the default GEXBOT API DLL standards:

| SG Index (UI) | Subgraph Name (Label) | Description |
| :--- | :--- | :--- |
| **SG1** | Major Call Gamma by Volume | Major Positive Gamma Level (derived from Volume) |
| **SG2** | Major Put Gamma by Volume | Major Negative Gamma Level (derived from Volume) |
| **SG3** | Zero Gamma | Zero Gamma Level |
| **SG4** | Major Call Gamma by OI | Major Positive Gamma Level (derived from OI) |
| **SG5** | Major Put Gamma by OI | Major Negative Gamma Level (derived from OI) |
| **SG6** | Major Long Gamma | Major Long Gamma Level |
| **SG7** | Major Short Gamma | Major Short Gamma Level |
| **SG8** | Net GEX Volume | Net Gamma Exposure (Volume) |
| **SG9** | Net GEX OI | Net Gamma Exposure (Open Interest) |

