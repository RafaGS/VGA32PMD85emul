# Tesla PMD 85 Emulator — ESP32-FabGL Port (Olimex & LilyGO TTGO VGA32)

[**English Version**](#english-version) | [**Versión en Español**](#versión-en-español)

## English Version

More information at [Minibot's PMD 85 Emulator Project](https://minibots.wordpress.com/2026/11/04/emulador-de-tesla-pmd-85-2-con-olimex-esp32-sbc-fabgl/).

Port of the Tesla PMD 85 emulator adapted for ESP32 boards running the FabGL library—specifically supporting the **Olimex ESP32-SBC-FabGL** and **LilyGO TTGO VGA32**—introducing native support for PMD 85-2, memory bus fixes, tape loading hooks, and PlatformIO toolchain support.

This project expands upon the baseline PMD 85 emulator by Stan6314 by targeting ESP32-based retro hardware via FabGL. It incorporates architectural refinements to accurately handle hardware differences between PMD 85-1 and PMD 85-2 models—specifically memory bus layout adjustments, out-of-bounds ROM access protections, unified machine reset logic, and dynamic virtual port tape interception for `.ptp` images across supported development boards.

### Key Features

* **Dual Board Support:** Compatible with both **Olimex ESP32-SBC-FabGL** and **LilyGO TTGO VGA32** (v1.x / v2.x) development boards.

* **Machine Selection & Bus Accuracy:** Added `MACHINE` selector macro (`PMD85_1` and `PMD85_2`) for model-specific ROM set selection. Corrected memory bus behavior so `0x9000`–`0x9FFF` and `0xB000`–`0xBFFF` ranges no longer map as accessible RAM on 48 KB configurations.

* **Robust ROM Management & Unified Reset:** Added out-of-bounds read safeguards to the ROM subsystem and centralized initialization logic into a unified `machineReset()` routine.

* **Fast Tape Interception for Monitor 2:** Instead of simulating software-decoded edge transitions via the DSR pin (as used by Monitor 2, unlike the 8251-based Monitor 1), the emulator patches a RAM copy of Monitor 2 (byte reader, header entry, and helper routine) with a hook redirecting `.ptp` stream reads through three virtual I/O ports after validating the ROM byte-by-byte.

* **In-Session Tape Rewind:** Added the `[Ctrl]` + `[Backspace]` keyboard shortcut to reset tape playback position without resetting the machine state.

* **Modern PlatformIO Environment:** Migrated build system to PlatformIO with multi-environment configurations for easy targeting of either Olimex or LilyGO boards.

### System Architecture

* **Core Emulation Engine:** 8080/Z80-compatible CPU core with memory bus decoding reflecting authentic PMD 85 address line constraints per model.

* **Tape I/O Subsystem:** Virtualized I/O ports interfacing directly with SD card / flash storage holding `.ptp` tape files, bypassed through memory-patched hooks in RAM-mirrored monitor routines.

* **Display & Input Layer:** Hardware rendering and PS/2 keyboard processing driven via the onboard peripherals and FabGL framework, configured for either Olimex or TTGO VGA32 pinouts.

### Quick Start Guide

1. **Clone the Repository:** Clone this project and open it inside VS Code with the PlatformIO extension installed.

2. **Select Target & Machine:** In `platformio.ini` (or build flags), choose your board environment (`olimex` or `ttgo_vga32`) and set `MACHINE` to either `PMD85_1` or `PMD85_2`.

3. **Build & Flash:** Connect your board via USB and run `PlatformIO: Upload`.

4. **Load Software & Play:** Connect a VGA monitor and PS/2 keyboard. Place your `.ptp` tape files onto the SD card; use `[Ctrl]` + `[Backspace]` at any time to rewind the tape to the beginning.

### Requirements

* **Hardware:** 
  
  * Compatible ESP32 board: **Olimex ESP32-SBC-FabGL** or **LilyGO TTGO VGA32** (v1.x / v2.x).
  * VGA display / monitor.
  * Standard PS/2 keyboard (or USB keyboard with passive PS/2 adapter).

* **Software Environment:** PlatformIO Core / PlatformIO IDE for VS Code, Espressif 32 platform package, and FabGL library.

* **Storage:** MicroSD card formatted as FAT32 for loading `.ptp` software images.

### Limitations

* Tape hook requires unmodified, standard Monitor 2 ROM binaries; patched or non-standard custom monitor variants will bypass the virtual port hook if the byte-by-byte check fails.

* Video output timings are constrained by the FabGL display modes supported on the respective ESP32 boards.

### License

This project is licensed under the GNU General Public License v3.0 (GPL-3.0). See the LICENSE file for details.

Based on [PMD-85-Emulator](https://github.com/Stan6314/PMD-85-Emulator) by **Stan6314**. FabGL library is copyright (c) Fabrizio Di Vittorio and licensed under GPL v3.

---

## Versión en Español

Más información en [Proyecto Emulador PMD 85 de Minibots](https://minibots.wordpress.com/2026/11/04/emulador-de-tesla-pmd-85-2-con-olimex-esp32-sbc-fabgl/).

Adaptación del emulador de Tesla PMD 85 para placas de desarrollo ESP32 mediante la biblioteca FabGL —compatible con **Olimex ESP32-SBC-FabGL** y **LilyGO TTGO VGA32**—, incorporando soporte nativo para el modelo PMD 85-2, correcciones en el mapa de memoria, interceptación de cinta y soporte para PlatformIO.

Esta versión amplía el emulador base de Tesla PMD 85 original de Stan6314 orientándolo a hardware retro basado en ESP32 con FabGL. Introduce mejoras arquitectónicas clave para reflejar con fidelidad las diferencias entre las versiones PMD 85-1 y PMD 85-2: compatibilidad multi-placa, ajuste estricto del bus de memoria, protección de lectura en ROM, unificación del ciclo de reset y un mecanismo de enganche por puertos virtuales para la carga de cintas en formato `.ptp`.

### Características Principales

* **Compatibilidad con Doble Placa:** Funciona de forma nativa tanto en **Olimex ESP32-SBC-FabGL** como en **LilyGO TTGO VGA32** (v1.x / v2.x).

* **Selector de Modelo y Corrección de Bus:** Selector `MACHINE` (`PMD85_1` y `PMD85_2`) para cargar los juegos de ROM correspondientes. Se corrigió el bus del sistema para evitar que los rangos `9000-9FFF` y `B000-BFFF` respondan indebidamente como RAM en configuraciones de 48 KB.

* **Protección de ROM y Reset Unificado:** Implementación de límites de lectura en el array de memoria ROM para evitar accesos fuera de rango e integración de toda la inicialización en la rutina `machineReset()`.

* **Carga de Cinta Rápida para Monitor 2:** Dado que el Monitor 2 decodifica flancos por software en el pin DSR en vez de usar el USART 8251, el emulador sustituye tres rutinas críticas (lectura de byte, entrada de cabecera y rutina auxiliar) en una copia en RAM del monitor tras verificar la ROM byte a byte, redirigiendo la lectura del fichero `.ptp` a través de tres puertos virtuales.

* **Rebobinado en Caliente:** Nueva combinación de teclas `[Ctrl]` + `[Retroceso]` para rebobinar la cinta al inicio en cualquier momento sin reiniciar la máquina.

* **Migración a PlatformIO:** Proyecto configurado con entornos diferenciados en PlatformIO para compilar de forma inmediata según la placa objetivo elegida.

### Arquitectura del Sistema

* **Motor de Emulación:** Núcleo de CPU compatible 8080/Z80 con decodificación de bus parametrizada según el hardware real de cada revisión del PMD 85.

* **Subsistema de Cinta (.ptp):** Enrutamiento de datos desde tarjeta SD / memoria flash mediante puertos virtuales I/O y parches transparentes en la memoria RAM del monitor.

* **Capa Gráfica y Periféricos:** Generación de señal de vídeo y captura de teclado PS/2 gestionados a través de FabGL, adaptando el patillaje según se utilice Olimex o TTGO VGA32.

### Guía Rápida de Uso

1. **Clonar el Repositorio:** Descarga el proyecto y ábrelo en VS Code con PlatformIO instalado.

2. **Configurar Entorno y Modelo:** En `platformio.ini` selecciona el entorno de tu placa (`olimex` o `ttgo_vga32`) y define el valor de `MACHINE` (`PMD85_1` o `PMD85_2`).

3. **Compilar y Flashear:** Conecta tu placa por USB y ejecuta `PlatformIO: Upload`.

4. **Carga y Ejecución:** Conecta pantalla VGA y teclado PS/2. Copia tus cintas `.ptp` en la tarjeta SD; pulsa `[Ctrl]` + `[Retroceso]` si necesitas rebobinar la cinta.

### Requisitos

* **Hardware:** 
  
  * Placa ESP32 compatible: **Olimex ESP32-SBC-FabGL** o **LilyGO TTGO VGA32** (v1.x / v2.x).
  * Monitor con entrada VGA.
  * Teclado estándar PS/2 (o teclado USB con adaptador pasivo PS/2).

* **Entorno de Desarrollo:** PlatformIO (Core o extensión para VS Code), toolchain de Espressif 32 y biblioteca FabGL.

* **Almacenamiento:** Tarjeta MicroSD en formato FAT32 con archivos de cinta `.ptp`.

### Limitaciones

* El enganche de cinta virtual depende de la comprobación byte a byte de la ROM del Monitor 2; variantes modificadas o ROMs no estándar no activarán el enganche virtual.

* Las frecuencias de refresco y resolución están sujetas a los modos compatibles con FabGL en el ESP32.

### Licencia

Este proyecto se distribuye bajo la Licencia Pública General de GNU v3.0 (GPL-3.0). Consulta el archivo LICENSE para más información.

Basado en el proyecto original [PMD-85-Emulator](https://github.com/Stan6314/PMD-85-Emulator) de **Stan6314**. La biblioteca FabGL es propiedad de Fabrizio Di Vittorio bajo licencia GPL v3.