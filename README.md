# SierraCharts GexBot Integration

This project provides a robust integration between GexBot data and Sierra Chart, enabling real-time recording, storage, and visualization of Gamma Exposure (GEX) data.

## 🏗️ Architecture

The system consists of two independent studies (DLLs) that work in a **Producer-Consumer** relationship:

### 1. Producer: `GexBotDataCollector`
*   **Role:** Runs on the chart that has the GexBot API connected.
*   **Function:** Reads the subgraph data from the GexBot API study, formats it, and writes it to a local CSV file.
*   **Format:** Creates daily files automatically (e.g., `Tickers 02.09.2026/ES_SPX.csv`).
*   **Performance:** Lightweight write operation throttled to a user-defined interval (default 10s).

### 2. Consumer: `GexBotCSVViewer`
*   **Role:** Runs on **any** chart where you want to view the history.
*   **Function:** Reads the CSV files created by the Collector and plots them as historical subgraphs.
*   **Integration:** Can be used on 1-minute, 5-minute, or any other timeframe charts.
*   **Optimization:** Uses **Incremental Reading** technology. It tracks the file size and only reads new data appended since the last check, ensuring O(1) performance even with large datasets.

## 🚀 Setup & Installation

### 1. Prerequisite
Ensure you have the GexBot API study installed and working on a chart in Sierra Chart.

### 2. Compilation
1.  Open Sierra Chart.
2.  Go to **Analysis** -> **Build Custom Studies DLL**.
3.  Select the following files:
    *   `GexBotDataCollector.cpp`
    *   `GexBotCSVViewer.cpp`
4.  Click **Build**.
5.  Wait for the "Remote build is complete" message.

### 3. Usage Guide

#### Step A: Setup the Collector (The "Recorder")
1.  Open the chart that has the GexBot API running.
2.  Go to **Analysis** -> **Studies**.
3.  Add **`GexBot Data Collector`**.
4.  Open the **Settings** for the study:
    *   **Source Chart ID:** Set to `0` if it's the same chart, or the ID of the chart with GexBot API.
    *   **Source Study ID:** The ID of the GexBot API study on that chart.
    *   **Ticker Name:** `ES_SPX` (or your specific ticker).
    *   **Output Directory:** `C:\GexBot\Data` (Ensure this folder exists or let the study create it).
    *   **Write Interval:** `10` seconds (Recommended).

#### Step B: Setup the Viewer (The "Player")
1.  Open any chart where you want to see the data (e.g., a 5-minute ES chart).
2.  Go to **Analysis** -> **Studies**.
3.  Add **`GexBot CSV Viewer`**.
4.  Open the **Settings**:
    *   **Ticker:** Must match what you set in the Collector (e.g., `ES_SPX`).
    *   **Local CSV Path:** Must match the Collector's output path (`C:\GexBot\Data`).
    *   **Days to Load:** `2` (Loads today and yesterday). Increase this if you need more history (e.g., `30` for a month), but be aware the initial load will take a second.
    *   **Refresh Interval:** `10` seconds (Should match or be slightly longer than the Collector).

## 🎛️ Configuration Options

### GexBot Data Collector
| Input Name | Description | Default |
| :--- | :--- | :--- |
| **Source Chart ID** | The ID of the chart hosting the GexBot API study. Use `0` for "Current Chart". | `0` |
| **Source Study ID** | The unique ID of the GexBot API study (found in the Studies list). | `0` |
| **Ticker Name** | The name used for the CSV filename. | `ES_SPX` |
| **Output Directory** | The base folder where daily subfolders will be created. | `C:\GexBot\Data` |
| **Market Start/End** | Time filter to only record during specific hours. | `09:30:00` - `16:00:00` |
| **Write Interval** | How often to save data to the CSV. Lower = more resolution, Higher = less disk usage. | `10s` |

### GexBot CSV Viewer
| Input Name | Description | Default |
| :--- | :--- | :--- |
| **Ticker** | Name of the ticker file to look for. | `ES_SPX` |
| **Local CSV Path** | Base folder to search for data. | `C:\GexBot\Data` |
| **Refresh Interval** | How often to check the file for new data. | `10s` |
| **Days to Load** | Number of past days to load into history. | `2` |
| **UTC Offset** | Offset in hours to adjust the timestamp if needed. | `0` |

## 📊 Subgraph Mapping (Viewer)
The Viewer automatically maps the CSV data to the following subgraphs:
*   **SG1:** Major Call Gamma (Vol)
*   **SG2:** Major Put Gamma (Vol)
*   **SG3:** Zero Gamma
*   **SG4:** Major Call Gamma (OI)
*   **SG5:** Major Put Gamma (OI)
*   **SG6:** Major Long Gamma
*   **SG7:** Major Short Gamma
*   **SG8:** Net GEX (Vol)
*   **SG9:** Net GEX (OI)

## ⚡ Performance Note
The Viewer uses an **Incremental Read** strategy. It remembers the file position (offset) where it last stopped reading. On every update, it seeks directly to that position and only parses the few new lines added. This ensures zero CPU overhead during the trading day, even when the data file grows large.
