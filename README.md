# continuous-fabric-shade-variation
An IoT-integrated continuous shade monitoring system utilizing an AS7341 multi-spectral sensor array and ESP32 edge computing. It evaluates real-time CIELAB and CMC (2:1) colorimetric tolerances via an AWS backend that can challenge conventional expensive spectrophotometer achieving tight accuracy. 

# IoT-Integrated Continuous Shade Monitoring System

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Hardware: ESP32](https://img.shields.io/badge/Hardware-ESP32-blue.svg)]()
[![Cloud: AWS IoT](https://img.shields.io/badge/Cloud-AWS_IoT_Core-orange.svg)]()
[![Live Dashboard](https://img.shields.io/badge/Live_Demo-di--pro.online-success.svg)](https://di-pro.online)

## Project Overview
Conventional spectrophotometer systems in industrial textile manufacturing are highly expensive. These legacy instruments rely heavily on offline, discrete single-point sampling, which fails to monitor continuous high-speed fabric traversal and frequently results in undetected shade bands. 

This repository contains the hardware schematics, embedded firmware, and cloud backend source code for an affordable, fully automated IoT-integrated continuous shade monitoring system designed to bring Industry 4.0 capabilities directly to the factory floor. The system evaluates real-time CIELAB and CMC 2:1 colorimetric tolerances via an AWS backend providing thorough report (`.xlsx file)` of shade variation across the continuous length of fabric.

---

## System Architecture & Key Features

### 1. Spatial Multi-Spectral Sensing
Replaces singular sensors with a distributed array of multiplexed **AS7341 multi-spectral sensors** spanning the fabric width. It normalizes raw XYZ coordinates against the specific Tristimulus Reference White constants for Illuminant D65 (Standard Daylight).

### 2. Edge-Compute Optimization
Powered by a multithreaded ESP32 microcontroller utilizing the FreeRTOS framework. Hardware Floating-Point Unit (FPU) acceleration and explicit core pinning allow the system to decouple hardware optical timing from network telemetry, sustaining a continuous scanning rate of 16.68 ms per measurement cycle.

### 3. Enterprise Telemetry Pipeline
Transitions from localized embedded processing to an Industrial Telemetry-grade architecture using **AWS IoT Core**. High-frequency data streams bypass HTTP REST overhead by utilizing the Lightweight Message Queuing Telemetry Transport (MQTT) protocol, secured via 2048-bit RSA-encrypted TLS certificates.

### 4. Dynamic Tolerance & Mathematical Engine
The FastAPI Python backend dynamically warps the tolerance boundary into an ellipsoidal geometry based on human visual perception using the standard $\Delta E_{CMC}$ equation:

$$\Delta E_{CMC} = \sqrt{\left(\frac{\Delta L^{\ast}}{l S_L}\right)^2 + \left(\frac{\Delta C_{ab}^{\ast}}{c S_c}\right)^2 + \left(\frac{\Delta H_{ab}^{\ast}}{S_H}\right)^2}$$

### 5. Mechanical Flutter Compensation
Implements a novel penalty heuristic utilizing the Near-Infrared (NIR) channel as an ambient anchor to prevent false readings caused by physical fabric displacement:

$$L_{penalized}^* = L_{raw}^* \times \left(1 - \frac{NIR_{measured}}{NIR_{baseline}}\right)$$

---

## Repository Structure

| Directory | Description |
| :--- | :--- |
| **`firmware/`** | ESP32 C++ source code (`hardware_data_logger.ino`), FreeRTOS tasks, and multiplexer driver files. |
| **`backend_Source Code/`** | Python FastAPI server, MQTT handlers, and core mathematical algorithms (`backend_server.py`). |
| **`client_app_frontend/`** | 60-FPS Web Dashboard assets (`index.html`, `desktop_dashboard_app.py`, WebSockets, Chart.js configurations). The production build is actively hosted at di-pro.online.<br><br>**Guest Access for Reviewers:**<br>Username: `DIPROrM`<br>Password: `Dipr0` |
| **`docs/`** | Detailed operational calibration manuals. |

---

## Operational & Calibration Modes

For comprehensive setup and operation instructions, please refer to the `docs/` folder. The system supports three distinct operational modes for establishing the pristine colorimetric baseline depending on the master fabric size:

* **Standard Array Mode:** Evaluates shade variation across the full width of the test fabric utilizing the entire spatial array simultaneously.
* **Selective Sequence:** Evaluates shade variation taking the center of the fabric as the baseline standard, utilizing sequential single-sensor logging.
* **Manual QTX Entry:** Allows direct serial terminal input of certified L*, a*, and b* standard values.

---

## 📖 Citation and Data Availability

This repository supports a manuscript currently under peer review. If you utilize this code, cloud architecture, or mathematical model in your research prior to official publication, please cite this repository directly:

> **[Dipro Roy     |    MD Sujon Hasan    |    Mohammad Noor Nabi     |    Dr. Shaikh MD Mominul Alam], "Development and Validation of a Cost-Effective Real-Time IoT-Enabled Multispectral
Colorimetric System for Shade Variation Evaluation of In-Motion Textile Fabric
," *Under Review* at [Heliyon], 2026.**
> *(Note: The official DOI and citation details will be updated here upon publication.)*

All firmware, cloud architectural code, and mathematical models are provided open-source to ensure full scientific reproducibility. 

---

## ⚖️ License
This project is licensed under the **MIT License**. See the `LICENSE` file for details.
