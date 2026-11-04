/* DirectPMD85 is 8-bit computer emulator based on the FabGL library
 * -> see http://www.fabglib.org/ or FabGL on GitHub.
 *  
 * For proper operation, an ESP32 module with a VGA monitor 
 * and a PS2 keyboard connected according to the 
 * diagram on the website www.fabglib.org is required.
 * 
 * Cassette recorder is emulated using a microSD card. The tape is represented 
 * by the file "/tape.ptp" in the root directory of the card (FAT32). 
 * ".ptp" files with programs for PMD85 are available e.g. here:
 * https://pmd85.borik.net/wiki/Download
 * It is recommended to number the individual programs in the ptp file 
 * in ascending order. 
 * 
 * DirectPMD85 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or any later version.
 * DirectPMD85 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY.
 * Stan Pechal, 2023
 * Version 1.01 - minor code cleaning and addition of comments
*/

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "fabgl.h"
#include "emudevs/i8080.h"          // For processor

#define SD_MOSI  12
#define SD_CLK   14
#define SD_CS    13

#define SD_MISO_TTGO    2
#define SD_MISO_OLIMEX  35

enum BoardType {
    BOARD_UNKNOWN,
    BOARD_LILYGO_TTGO_VGA32,
    BOARD_OLIMEX_FABGL
};

BoardType detectarPlaca() {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    if (chip_info.revision >= 3) return BOARD_OLIMEX_FABGL;
    if (chip_info.revision == 1) return BOARD_LILYGO_TTGO_VGA32;
    return BOARD_UNKNOWN;
}


int pin_sd_cs   = SD_CS;
int pin_sd_mosi = SD_MOSI;
int pin_sd_miso = SD_MISO_TTGO;
int pin_sd_clk  = SD_CLK;
SPIClass sdSPI(HSPI);        // SPI bus used for the microSD (pins are routed through the GPIO matrix)

fabgl::VGADirectController DisplayController;
fabgl::PS2Controller PS2Controller;
SoundGenerator  soundGenerator;
SquareWaveformGenerator square;

// Constants for video output
static constexpr int borderSize           = 22;
static constexpr int borderXSize          = 56;
static constexpr int scanlinesPerCallback = 2;  // screen height should be divisible by this value

static TaskHandle_t  mainTaskHandle;
void * auxPoint;

// **************************************************************************************************
// "Tape recorder" is 1 file on the microSD
File file;
bool isTape = false;      // Is the tape file open
int cntTape = 0;

// Sound constants
#define SAMPLE_RATE 16000
#define FREQ_HI 4000
#define FREQ_LO 1000
#define FREQ_ME 2000

// Hardware emulated on the PMD85 computer
// Processor I8080 will be used from the library FabGL
fabgl::i8080 m_i8080;
// Variables for emulating 8255 (keyboard connection)
int portPA = 0;    // Data sent to PA port
int keyboardIn[16];      // Value read from keyboard PB port
int shiftFlag = 0x7F;
// Variables for VGA video output
uint8_t brcolor; // bright color
uint8_t fgcolor; // normal color
uint8_t bgcolor; // dark color
uint8_t darkbgcolor; // black
int width, height;  // Display dimensions
bool blinkFlag = false;   // Flag for pixel blinking
int BlinkCnt=0;   // Counter for video pixel blink
// Variables for emulating 8255 on extended ROM module
int portExtPB = 0;    // Data sent to PB port
int portExtPC = 0;    // Data sent to PC port

// RAM memory will be just Byte array
volatile unsigned char pmd85ram[65536];   // selected addresses are overwritten by ROM in read mode

// **************************************************************************************************
// Machine selection: choose ONE model here
#define PMD85_1   1
#define PMD85_2   2
#define PMD85_2A  3     // not implemented yet
#define PMD85_3   4     // not implemented yet
#define MACHINE   PMD85_2

// Monitor ROM is in the array "pmd85rom[]", ROM module (BASIC G) in the array "basicrom[]"
#if MACHINE == PMD85_1
  #include "monit85.h"    // monit1.rom (4 kB, at 0x8000)
  #include "basic1.h"     // basic1.rmm
#elif MACHINE == PMD85_2
  #include "monit2.h"     // monit2.rom (4 kB, at 0x8000)
  #include "basic2.h"     // basic2.rmm
#else
  #error "This MACHINE is not implemented yet"
#endif

// **************************************************************************************************
// System 8255 (keyboard, sound, LEDs, paging on 2A/3). Selected by A7=1, A3=0, A2=1;
// A1:A0 select the register, so the real addresses 0xF4-0xF7 are mirrored (BASIC uses 0x86 for port C).
static uint8_t ppiCtrl = 0x9B;   // after reset all ports are inputs
static uint8_t portC   = 0;      // output latch of port C

static void updateSound()
{
  // PC0 = steady ~1 kHz tone, PC1 = steady ~4 kHz tone (PC2 = software beeper, not emulated yet)
  switch (portC & 0x3) {
    case 0: soundGenerator.play(false); break;
    case 1: square.setFrequency(FREQ_LO); soundGenerator.play(true); break;
    case 2: square.setFrequency(FREQ_HI); soundGenerator.play(true); break;
    case 3: square.setFrequency(FREQ_ME); soundGenerator.play(true); break;
  }
}

static void ppiSysReset()
{
  ppiCtrl = 0x9B;
  portPA = 0;
  portC = 0;
  updateSound();
}

static void ppiSysWrite(int reg, int value)
{
  switch (reg) {
    case 0: portPA = value; break;
    case 2: portC = value; updateSound(); break;
    case 3:
      if (value & 0x80) {          // mode set: control word, output latches are cleared
        ppiCtrl = value;
        portPA = 0;
        portC = 0;
      } else {                     // bit set/reset on port C
        int bit = (value >> 1) & 7;
        if (value & 1) portC |= (1 << bit); else portC &= ~(1 << bit);
      }
      updateSound();
      break;
    default: break;                // port B is input only
  }
}

static int ppiSysRead(int reg)
{
  switch (reg) {
    case 0: return portPA;
    case 1: return (keyboardIn[portPA & 0x0F]) & shiftFlag;
    case 2: return (ppiCtrl & 0x08) ? (0xF0 | (portC & 0x0F)) : portC;  // upper half as input -> pull-ups
    default: return 0xFF;
  }
}

// **************************************************************************************************
// Monitor ROM is copied to RAM so it can be patched (tape hook on PMD 85-2 and later)
static uint8_t monitorRom[4096];

// **************************************************************************************************
// Tape. PMD 85-1 receives data through the 8251 (byte by byte, see readIO).
// PMD 85-2 and later decode the tape signal by software: the monitor measures the time between
// edges of the DSR pin of the 8251 (bit 7 of port 0x1F). Instead of generating that waveform, the
// tape byte reader of the monitor is replaced by a tiny stub that reads bytes from the PTP file
// through three virtual ports (flash loading):
//   0x3D  non-consuming status, bit0 = 1 when the tape has no more data
//   0x3F  fetch the next tape byte into a latch, bit0 = 1 on end of tape
//   0x3E  returns the latched byte
// Buffered access to the tape file. Calling File::available()/read() for every single byte is slow
// on a microSD card, so the file is read in chunks and the remaining size is tracked here.
static constexpr int TAPE_BUF = 512;
static uint8_t  tapeBuf[TAPE_BUF];
static int      tapeBufLen = 0, tapeBufPos = 0;
static uint32_t tapeFileLeft = 0;        // bytes still in the file, not yet copied to tapeBuf

static void tapeRewind()
{
  tapeBufLen = tapeBufPos = 0;
  cntTape = 0;
  tapeFileLeft = 0;
  if (file && file.seek(0)) tapeFileLeft = file.size();
}

static inline uint32_t tapeAvail() { return (uint32_t)(tapeBufLen - tapeBufPos) + tapeFileLeft; }

static int tapeRead()                    // next byte of the file, -1 at the end
{
  if (tapeBufPos >= tapeBufLen) {
    if (tapeFileLeft == 0) return -1;
    size_t n = file.read(tapeBuf, tapeFileLeft < (uint32_t)TAPE_BUF ? tapeFileLeft : (uint32_t)TAPE_BUF);
    if (n == 0) { tapeFileLeft = 0; return -1; }
    tapeBufLen = (int)n; tapeBufPos = 0; tapeFileLeft -= n;
  }
  return tapeBuf[tapeBufPos++];
}

#if MACHINE != PMD85_1
static uint8_t tapeLatch = 0;

static bool tapeHasData()
{
  return (cntTape > 0) ? (tapeAvail() > 0) : (tapeAvail() >= 3);
}

// PTP file = sequence of blocks, each one prefixed by its length (16 bit, little endian).
// The byte stream of all blocks is exactly what the tape contains.
static bool tapeFetch()
{
  tapeLatch = 0;
  while (cntTape == 0) {
    if (tapeAvail() < 2) return false;
    cntTape = tapeRead() & 0xFF;
    cntTape |= (tapeRead() & 0xFF) << 8;
  }
  int b = tapeRead();
  if (b < 0) return false;
  cntTape--;
  tapeLatch = (uint8_t)b;
  return true;
}

static inline bool tapeSignal() { return false; }   // DSR: no real waveform is generated
#endif

static bool romMatches(int addr, const uint8_t *expect, int n)
{
  return memcmp(&monitorRom[addr - 0x8000], expect, n) == 0;
}
static void romPatch(int addr, const uint8_t *repl, int n)
{
  memcpy(&monitorRom[addr - 0x8000], repl, n);
}

static void machineInit()
{
  memcpy(monitorRom, pmd85rom, sizeof(pmd85rom) < sizeof(monitorRom) ? sizeof(pmd85rom) : sizeof(monitorRom));
#if MACHINE == PMD85_2
  // Patches for monit2.rom (PMD 85-2), verified byte by byte before being applied.
  // 8B6C  byte reader:      IN 3F / RRC / IN 3E / RET           (carry = end of tape, A = byte)
  static const uint8_t e1[] = {0xC5,0xD5,0xE5,0x2A,0x73,0xC1};
  static const uint8_t r1[] = {0xDB,0x3F,0x0F,0xDB,0x3E,0xC9};
  // 8B72  trampoline (original code of this area is unreachable once 8B6C is replaced):
  //       IN 3D / RAR / JNC 8EF3 (tape present: skip pilot calibration)
  //       CALL 81EE / MVI D,13 / MOV B,A / JMP 8EC4          (original first instructions of HEADIN)
  static const uint8_t e2[] = {0x0E,0x26,0xCD,0x95,0x88,0x37,0xCA,0x98,0x8B,0x79,0xBD,0xF2,0x72,0x8B,0x11};
  static const uint8_t r2[] = {0xDB,0x3D,0x1F,0xD2,0xF3,0x8E,0xCD,0xEE,0x81,0x16,0x13,0x47,0xC3,0xC4,0x8E};
  // 8EBE  HEADIN entry:     JMP 8B72
  static const uint8_t e3[] = {0xCD,0xEE,0x81,0x16,0x13,0x47};
  static const uint8_t r3[] = {0xC3,0x72,0x8B,0x00,0x00,0x00};
  // 848D  bit-sync helper (jumped into the old byte reader): RET
  static const uint8_t e4[] = {0xC5};
  static const uint8_t r4[] = {0xC9};
  // Apply all or nothing: if the monitor is a different version the tape hook is not installed
  if (romMatches(0x8B6C, e1, sizeof(e1)) && romMatches(0x8B72, e2, sizeof(e2)) &&
      romMatches(0x8EBE, e3, sizeof(e3)) && romMatches(0x848D, e4, sizeof(e4))) {
    romPatch(0x8B6C, r1, sizeof(r1));
    romPatch(0x8B72, r2, sizeof(r2));
    romPatch(0x8EBE, r3, sizeof(r3));
    romPatch(0x848D, r4, sizeof(r4));
  }
#endif
}

// **************************************************************************************************
// Reset: also used by the ESC key
static void machineReset()
{
  ppiSysReset();
  m_i8080.setPC(0x8000);   // PMD85-1/2/2A start at this address due blocking A15
  tapeRewind();
}

// **************************************************************************************************
// Requests from the keyboard callback. The callback runs in another task, so it must not touch the CPU
// or the tape file directly: it only sets these flags and loop() carries them out between CPU slices.
static volatile bool resetReq      = false;   // ESC: reset (also rewinds the tape)
static volatile bool tapeRewindReq = false;   // Ctrl+Backspace: rewind the tape without resetting
static volatile bool ctrlDown      = false;   // Ctrl state (Ctrl is also the left Enter key)

// **************************************************************************************************
// Functions for communication on the bus
// Memory map PMD 85-1 / 85-2 (48 kB RAM):
//   0000-7FFF RAM, 8000-8FFF monitor ROM, 9000-9FFF nothing, A000-AFFF monitor ROM (mirror),
//   B000-BFFF nothing, C000-FFFF RAM / video RAM. Writes always go to RAM when RAM exists.
static int readByte(void * context, int address)
{
  address &= 0xFFFF;
  if ((address & 0xC000) != 0x8000) return pmd85ram[address];
  switch ((address >> 12) & 3) {
    case 0:
    case 2:  return monitorRom[address & 0x0FFF];
    default: return 0xFF;      // nothing there
  }
}
static void writeByte(void * context, int address, int value)
{
  address &= 0xFFFF;
  if ((address & 0xC000) == 0x8000) return;    // no RAM at 8000-BFFF on 48 kB models
  pmd85ram[address] = (unsigned char)value;
}
static int readWord(void * context, int addr)                 { return readByte(context, addr) | (readByte(context, addr + 1) << 8); };
static void writeWord(void * context, int addr, int value)    { writeByte(context, addr, value & 0xFF); writeByte(context, addr + 1, value >> 8); } ;

static int readIO(void * context, int address)
{
  address &= 0xFF;
  if ((address & 0x8C) == 0x84) return ppiSysRead(address & 3);     // system 8255
  switch (address) {
#if MACHINE == PMD85_1
    // *** 8251 port - for read from "tape" recorder
    case 0x1E:      // Data
      if(!tapeAvail()) return 0;   // Serial port reads data from file, not from tape recorder
      if((cntTape == 0) &&  tapeAvail()) { cntTape = (tapeRead() & 0xFF); if(tapeAvail()) cntTape += ((tapeRead()<<8) & 0xFF00); }
      if((cntTape) &&  tapeAvail()) { cntTape--; return tapeRead(); } else return 0xFF;
    break;
    case 0x1F:      // Status
      if(tapeAvail()) return (0x02); else return (0x00);
    break;
#else
    case 0x1E: return 0xFF;
    case 0x1F: return 0x05 | (tapeSignal() ? 0x80 : 0x00);   // TxRDY | TxEMPTY | DSR (tape in)
    case 0x3D: return tapeHasData() ? 0x00 : 0x01;           // virtual: tape status (no side effects)
    case 0x3F: return tapeFetch() ? 0x00 : 0x01;             // virtual: fetch next byte, 1 = end of tape
    case 0x3E: return tapeLatch;                             // virtual: fetched byte
#endif
    // *** 8255 extended ROM module
    case 0xF8:      // PA port
    {
      unsigned int idx = ((portExtPC<<8) & 0x3F00) | (portExtPB & 0xFF);
      if (idx < sizeof(basicrom)) return basicrom[idx]; else return 0xFF;
    }
    break;
    case 0xF9:      // PB port
      return portExtPB;
    break;
    case 0xFA:      // PC port
      return portExtPC;
    break;
    default: return 0xFF; break;  // Return "not selected I/O" - bus with 0xFF
  }
};
static void writeIO(void * context, int address, int value)
{
  address &= 0xFF;
  if ((address & 0x8C) == 0x84) { ppiSysWrite(address & 3, value); return; }   // system 8255
  switch (address) {
    // *** 8255 ports - extended ROM module
    case 0xF9:      // PB port
      portExtPB = value;
    break;
    case 0xFA:      // PC port
      portExtPC = value;
    break;
    default: break;
  }
};

// **************************************************************************************************
// Keyboard interface for selected keys
// Handles Key Up following keys:
void procesKeyUp(VirtualKey key) {
  switch (key) {
      case VirtualKey::VK_F1: keyboardIn[0] = 0x7F; break;  // F1
      case VirtualKey::VK_F2: keyboardIn[1] = 0x7F; break;  // F2
      case VirtualKey::VK_F3: keyboardIn[2] = 0x7F; break;  // F3
      case VirtualKey::VK_F4: keyboardIn[3] = 0x7F; break;  // F4
      case VirtualKey::VK_F5: keyboardIn[4] = 0x7F; break;  // F5
      case VirtualKey::VK_F6: keyboardIn[5] = 0x7F; break;  // F6
      case VirtualKey::VK_F7: keyboardIn[6] = 0x7F; break;  // F7
      case VirtualKey::VK_F8: keyboardIn[7] = 0x7F; break;  // F8
      case VirtualKey::VK_F9: keyboardIn[8] = 0x7F; break;  // F9
      case VirtualKey::VK_F10: keyboardIn[9] = 0x7F; break;  // F10
      case VirtualKey::VK_F11: keyboardIn[10] = 0x7F; break;  // F11
      case VirtualKey::VK_F12: keyboardIn[11] = 0x7F; break;  // F12
      case VirtualKey::VK_PRINTSCREEN: keyboardIn[12] = 0x7F;  break; // PRINTSCREEN - WRK
      case VirtualKey::VK_SCROLLLOCK: keyboardIn[13] = 0x7F;  break; // SCROLLLOCK - C-D
      case VirtualKey::VK_PAUSE: keyboardIn[14] = 0x7F;  break; // PAUSE - RCL

      case VirtualKey::VK_KP_0:
      case VirtualKey::VK_RIGHTPAREN:
      case VirtualKey::VK_0: keyboardIn[9] = 0x7F; break;  // 0
      case VirtualKey::VK_KP_1:
      case VirtualKey::VK_EXCLAIM:
      case VirtualKey::VK_1: keyboardIn[0] = 0x7F; break;  // 1
      case VirtualKey::VK_KP_2:
      case VirtualKey::VK_AT:
      case VirtualKey::VK_2: keyboardIn[1] = 0x7F; break;  // 2
      case VirtualKey::VK_KP_3:
      case VirtualKey::VK_HASH:
      case VirtualKey::VK_3: keyboardIn[2] = 0x7F; break;  // 3
      case VirtualKey::VK_KP_4:
      case VirtualKey::VK_DOLLAR:
      case VirtualKey::VK_4: keyboardIn[3] = 0x7F; break;  // 4
      case VirtualKey::VK_KP_5:
      case VirtualKey::VK_PERCENT:
      case VirtualKey::VK_5: keyboardIn[4] = 0x7F; break;  // 5
      case VirtualKey::VK_KP_6:
      case VirtualKey::VK_CARET:
      case VirtualKey::VK_6: keyboardIn[5] = 0x7F; break;  // 6
      case VirtualKey::VK_KP_7:
      case VirtualKey::VK_AMPERSAND:
      case VirtualKey::VK_7: keyboardIn[6] = 0x7F; break;  // 7
      case VirtualKey::VK_KP_8:
      case VirtualKey::VK_ASTERISK:
      case VirtualKey::VK_8: keyboardIn[7] = 0x7F; break;  // 8
      case VirtualKey::VK_KP_9:
      case VirtualKey::VK_LEFTPAREN:
      case VirtualKey::VK_9: keyboardIn[8] = 0x7F; break;  // 9
      case VirtualKey::VK_UNDERSCORE:
      case VirtualKey::VK_MINUS: keyboardIn[10] = 0x7F; break;  // -_
      case VirtualKey::VK_EQUALS:
      case VirtualKey::VK_KP_PLUS:
      case VirtualKey::VK_PLUS: keyboardIn[11] = 0x7F; break;  // +=
      case VirtualKey::VK_INSERT: keyboardIn[12] = 0x7F;  break; // INS
      case VirtualKey::VK_DELETE: keyboardIn[13] = 0x7F;  break; // DEL
      case VirtualKey::VK_BACKSPACE: keyboardIn[14] = 0x7F;  break;// BACKSPACE - CLR 

      case VirtualKey::VK_q:
      case VirtualKey::VK_Q: keyboardIn[0] = 0x7F; break;  // q-Q
      case VirtualKey::VK_w:
      case VirtualKey::VK_W: keyboardIn[1] = 0x7F; break;  // w-W
      case VirtualKey::VK_e:
      case VirtualKey::VK_E: keyboardIn[2] = 0x7F; break;  // e-E
      case VirtualKey::VK_r:
      case VirtualKey::VK_R: keyboardIn[3] = 0x7F; break;  // r-R
      case VirtualKey::VK_t:
      case VirtualKey::VK_T: keyboardIn[4] = 0x7F; break;  // t-T
      case VirtualKey::VK_z:
      case VirtualKey::VK_Z: keyboardIn[5] = 0x7F; break;  // z-Z
      case VirtualKey::VK_u:
      case VirtualKey::VK_U: keyboardIn[6] = 0x7F; break;  // u-U
      case VirtualKey::VK_i:
      case VirtualKey::VK_I: keyboardIn[7] = 0x7F; break;  // i-I
      case VirtualKey::VK_o:
      case VirtualKey::VK_O: keyboardIn[8] = 0x7F; break;  // o-O
      case VirtualKey::VK_p:
      case VirtualKey::VK_P: keyboardIn[9] = 0x7F; break;  // p-P
      case VirtualKey::VK_LEFTBRACKET:
      case VirtualKey::VK_LEFTBRACE: keyboardIn[10] = 0x7F; break;  // [{
      case VirtualKey::VK_RIGHTBRACKET:
      case VirtualKey::VK_RIGHTBRACE: keyboardIn[11] = 0x7F; break;  // ]}
      case VirtualKey::VK_LEFT: keyboardIn[12] = 0x7F;  break; // < left
      case VirtualKey::VK_HOME: keyboardIn[13] = 0x7F;  break; // Home
      case VirtualKey::VK_RIGHT: keyboardIn[14] = 0x7F;  break;// > right 

      case VirtualKey::VK_a:
      case VirtualKey::VK_A: keyboardIn[0] = 0x7F; break;  // a-A
      case VirtualKey::VK_s:
      case VirtualKey::VK_S: keyboardIn[1] = 0x7F; break;  // s-S
      case VirtualKey::VK_d:
      case VirtualKey::VK_D: keyboardIn[2] = 0x7F; break;  // d-D
      case VirtualKey::VK_f:
      case VirtualKey::VK_F: keyboardIn[3] = 0x7F; break;  // f-F
      case VirtualKey::VK_g:
      case VirtualKey::VK_G: keyboardIn[4] = 0x7F; break;  // g-G
      case VirtualKey::VK_h:
      case VirtualKey::VK_H: keyboardIn[5] = 0x7F; break;  // h-H
      case VirtualKey::VK_j:
      case VirtualKey::VK_J: keyboardIn[6] = 0x7F; break;  // j-J
      case VirtualKey::VK_k:
      case VirtualKey::VK_K: keyboardIn[7] = 0x7F; break;  // k-K
      case VirtualKey::VK_l:
      case VirtualKey::VK_L: keyboardIn[8] = 0x7F; break;  // l-L
      case VirtualKey::VK_COLON:
      case VirtualKey::VK_SEMICOLON: keyboardIn[9] = 0x7F; break;  // ;-:
      case VirtualKey::VK_QUOTE:
      case VirtualKey::VK_QUOTEDBL: keyboardIn[10] = 0x7F; break;  // '-"
      case VirtualKey::VK_VERTICALBAR:
      case VirtualKey::VK_BACKSLASH: keyboardIn[11] = 0x7F; break;  // |\ - []
      case VirtualKey::VK_PAGEUP:
      case VirtualKey::VK_UP: keyboardIn[12] = 0x7F;  break; // Up
      case VirtualKey::VK_END: keyboardIn[13] = 0x7F;   break;// End
      case VirtualKey::VK_PAGEDOWN:
      case VirtualKey::VK_DOWN: keyboardIn[14] = 0x7F;  break; // Down 

      case VirtualKey::VK_SPACE: keyboardIn[0] = 0x7F; break;  // space
      case VirtualKey::VK_y:
      case VirtualKey::VK_Y: keyboardIn[1] = 0x7F; break;  // y-Y
      case VirtualKey::VK_x:
      case VirtualKey::VK_X: keyboardIn[2] = 0x7F; break;  // x-X
      case VirtualKey::VK_c:
      case VirtualKey::VK_C: keyboardIn[3] = 0x7F; break;  // c-C
      case VirtualKey::VK_v:
      case VirtualKey::VK_V: keyboardIn[4] = 0x7F; break;  // v-V
      case VirtualKey::VK_b:
      case VirtualKey::VK_B: keyboardIn[5] = 0x7F; break;  // b-B
      case VirtualKey::VK_n:
      case VirtualKey::VK_N: keyboardIn[6] = 0x7F; break;  // n-N
      case VirtualKey::VK_m:
      case VirtualKey::VK_M: keyboardIn[7] = 0x7F; break;  // m-M
      case VirtualKey::VK_LESS:
      case VirtualKey::VK_COMMA: keyboardIn[8] = 0x7F; break;  // <-,
      case VirtualKey::VK_GREATER:
      case VirtualKey::VK_PERIOD: keyboardIn[9] = 0x7F; break;  // >-.
      case VirtualKey::VK_SLASH:
      case VirtualKey::VK_QUESTION: keyboardIn[10] = 0x7F; break;  // /-?
      case VirtualKey::VK_LCTRL:
      case VirtualKey::VK_RCTRL: ctrlDown = false; keyboardIn[13] = 0x7F; break;  // L Enter
      case VirtualKey::VK_RETURN:
      case VirtualKey::VK_KP_ENTER: keyboardIn[14] = 0x7F; break;  // R Enter

      case VirtualKey::VK_TAB:
      case VirtualKey::VK_LSHIFT:
      case VirtualKey::VK_RSHIFT: shiftFlag = 0x7F; break;  // L and R shift + TAB
      default: break;
      }
};

// Handles Key Down following keys:
void procesKeyDown(VirtualKey key) {
  switch (key) {
      case VirtualKey::VK_ESCAPE: resetReq = true; break;  // ESC

      case VirtualKey::VK_F1: keyboardIn[0] = 0x7E; break;  // F1
      case VirtualKey::VK_F2: keyboardIn[1] = 0x7E; break;  // F2
      case VirtualKey::VK_F3: keyboardIn[2] = 0x7E; break;  // F3
      case VirtualKey::VK_F4: keyboardIn[3] = 0x7E; break;  // F4
      case VirtualKey::VK_F5: keyboardIn[4] = 0x7E; break;  // F5
      case VirtualKey::VK_F6: keyboardIn[5] = 0x7E; break;  // F6
      case VirtualKey::VK_F7: keyboardIn[6] = 0x7E; break;  // F7
      case VirtualKey::VK_F8: keyboardIn[7] = 0x7E; break;  // F8
      case VirtualKey::VK_F9: keyboardIn[8] = 0x7E; break;  // F9
      case VirtualKey::VK_F10: keyboardIn[9] = 0x7E; break;  // F10
      case VirtualKey::VK_F11: keyboardIn[10] = 0x7E; break;  // F11
      case VirtualKey::VK_F12: keyboardIn[11] = 0x7E; break;  // F12
      case VirtualKey::VK_PRINTSCREEN: keyboardIn[12] = 0x7E; break;  // PRINTSCREEN
      case VirtualKey::VK_SCROLLLOCK: keyboardIn[13] = 0x7E; break;  // SCROLLLOCK
      case VirtualKey::VK_PAUSE: keyboardIn[14] = 0x7E; break; // PAUSE 

      case VirtualKey::VK_KP_0:
      case VirtualKey::VK_RIGHTPAREN:
      case VirtualKey::VK_0: keyboardIn[9] = 0x7D; break;  // 0
      case VirtualKey::VK_KP_1:
      case VirtualKey::VK_EXCLAIM:
      case VirtualKey::VK_1: keyboardIn[0] = 0x7D; break;  // 1
      case VirtualKey::VK_KP_2:
      case VirtualKey::VK_AT:
      case VirtualKey::VK_2: keyboardIn[1] = 0x7D; break;  // 2
      case VirtualKey::VK_KP_3:
      case VirtualKey::VK_HASH:
      case VirtualKey::VK_3: keyboardIn[2] = 0x7D; break;  // 3
      case VirtualKey::VK_KP_4:
      case VirtualKey::VK_DOLLAR:
      case VirtualKey::VK_4: keyboardIn[3] = 0x7D; break;  // 4
      case VirtualKey::VK_KP_5:
      case VirtualKey::VK_PERCENT:
      case VirtualKey::VK_5: keyboardIn[4] = 0x7D; break;  // 5
      case VirtualKey::VK_KP_6:
      case VirtualKey::VK_CARET:
      case VirtualKey::VK_6: keyboardIn[5] = 0x7D; break;  // 6
      case VirtualKey::VK_KP_7:
      case VirtualKey::VK_AMPERSAND:
      case VirtualKey::VK_7: keyboardIn[6] = 0x7D; break;  // 7
      case VirtualKey::VK_KP_8:
      case VirtualKey::VK_ASTERISK:
      case VirtualKey::VK_8: keyboardIn[7] = 0x7D; break;  // 8
      case VirtualKey::VK_KP_9:
      case VirtualKey::VK_LEFTPAREN:
      case VirtualKey::VK_9: keyboardIn[8] = 0x7D; break;  // 9
      case VirtualKey::VK_UNDERSCORE:
      case VirtualKey::VK_MINUS: keyboardIn[10] = 0x7D; break;  // -_
      case VirtualKey::VK_EQUALS:
      case VirtualKey::VK_KP_PLUS:
      case VirtualKey::VK_PLUS: keyboardIn[11] = 0x7D; break;  // +=
      case VirtualKey::VK_INSERT: keyboardIn[12] = 0x7D; break;  // INS
      case VirtualKey::VK_DELETE: keyboardIn[13] = 0x7D; break;  // DEL
      case VirtualKey::VK_BACKSPACE:
          if (ctrlDown) {                        // Ctrl+Backspace: rewind the tape (not a PMD 85 key)
            tapeRewindReq = true;
            keyboardIn[13] = 0x7F;               // release the L Enter generated by Ctrl
          } else keyboardIn[14] = 0x7D;          // BACKSPACE - CLR
          break;


      case VirtualKey::VK_q:
      case VirtualKey::VK_Q: keyboardIn[0] = 0x7B; break;  // q-Q
      case VirtualKey::VK_w:
      case VirtualKey::VK_W: keyboardIn[1] = 0x7B; break;  // w-W
      case VirtualKey::VK_e:
      case VirtualKey::VK_E: keyboardIn[2] = 0x7B; break;  // e-E
      case VirtualKey::VK_r:
      case VirtualKey::VK_R: keyboardIn[3] = 0x7B; break;  // r-R
      case VirtualKey::VK_t:
      case VirtualKey::VK_T: keyboardIn[4] = 0x7B; break;  // t-T
      case VirtualKey::VK_z:
      case VirtualKey::VK_Z: keyboardIn[5] = 0x7B; break;  // z-Z
      case VirtualKey::VK_u:
      case VirtualKey::VK_U: keyboardIn[6] = 0x7B; break;  // u-U
      case VirtualKey::VK_i:
      case VirtualKey::VK_I: keyboardIn[7] = 0x7B; break;  // i-I
      case VirtualKey::VK_o:
      case VirtualKey::VK_O: keyboardIn[8] = 0x7B; break;  // o-O
      case VirtualKey::VK_p:
      case VirtualKey::VK_P: keyboardIn[9] = 0x7B; break;  // p-P
      case VirtualKey::VK_LEFTBRACKET:
      case VirtualKey::VK_LEFTBRACE: keyboardIn[10] = 0x7B; break;  // [{
      case VirtualKey::VK_RIGHTBRACKET:
      case VirtualKey::VK_RIGHTBRACE: keyboardIn[11] = 0x7B; break;  // ]}
      case VirtualKey::VK_LEFT: keyboardIn[12] = 0x7B; break;  // < left
      case VirtualKey::VK_HOME: keyboardIn[13] = 0x7B; break;  // Home
      case VirtualKey::VK_RIGHT: keyboardIn[14] = 0x7B; break; // > right 

      case VirtualKey::VK_a:
      case VirtualKey::VK_A: keyboardIn[0] = 0x77; break;  // a-A
      case VirtualKey::VK_s:
      case VirtualKey::VK_S: keyboardIn[1] = 0x77; break;  // s-S
      case VirtualKey::VK_d:
      case VirtualKey::VK_D: keyboardIn[2] = 0x77; break;  // d-D
      case VirtualKey::VK_f:
      case VirtualKey::VK_F: keyboardIn[3] = 0x77; break;  // f-F
      case VirtualKey::VK_g:
      case VirtualKey::VK_G: keyboardIn[4] = 0x77; break;  // g-G
      case VirtualKey::VK_h:
      case VirtualKey::VK_H: keyboardIn[5] = 0x77; break;  // h-H
      case VirtualKey::VK_j:
      case VirtualKey::VK_J: keyboardIn[6] = 0x77; break;  // j-J
      case VirtualKey::VK_k:
      case VirtualKey::VK_K: keyboardIn[7] = 0x77; break;  // k-K
      case VirtualKey::VK_l:
      case VirtualKey::VK_L: keyboardIn[8] = 0x77; break;  // l-L
      case VirtualKey::VK_COLON:
      case VirtualKey::VK_SEMICOLON: keyboardIn[9] = 0x77; break;  // ;-:
      case VirtualKey::VK_QUOTE:
      case VirtualKey::VK_QUOTEDBL: keyboardIn[10] = 0x77; break;  // '-"
      case VirtualKey::VK_VERTICALBAR:
      case VirtualKey::VK_BACKSLASH: keyboardIn[11] = 0x77; break;  // |\ - []
      case VirtualKey::VK_PAGEUP:
      case VirtualKey::VK_UP: keyboardIn[12] = 0x77;  break; // Up
      case VirtualKey::VK_END: keyboardIn[13] = 0x77;  break; // End
      case VirtualKey::VK_PAGEDOWN:
      case VirtualKey::VK_DOWN: keyboardIn[14] = 0x77;  break; // Down 

      case VirtualKey::VK_SPACE: keyboardIn[0] = 0x6F; break;  // space
      case VirtualKey::VK_y:
      case VirtualKey::VK_Y: keyboardIn[1] = 0x6F; break;  // y-Y
      case VirtualKey::VK_x:
      case VirtualKey::VK_X: keyboardIn[2] = 0x6F; break;  // x-X
      case VirtualKey::VK_c:
      case VirtualKey::VK_C: keyboardIn[3] = 0x6F; break;  // c-C
      case VirtualKey::VK_v:
      case VirtualKey::VK_V: keyboardIn[4] = 0x6F; break;  // v-V
      case VirtualKey::VK_b:
      case VirtualKey::VK_B: keyboardIn[5] = 0x6F; break;  // b-B
      case VirtualKey::VK_n:
      case VirtualKey::VK_N: keyboardIn[6] = 0x6F; break;  // n-N
      case VirtualKey::VK_m:
      case VirtualKey::VK_M: keyboardIn[7] = 0x6F; break;  // m-M
      case VirtualKey::VK_LESS:
      case VirtualKey::VK_COMMA: keyboardIn[8] = 0x6F; break;  // <-,
      case VirtualKey::VK_GREATER:
      case VirtualKey::VK_PERIOD: keyboardIn[9] = 0x6F; break;  // >-.
      case VirtualKey::VK_SLASH:
      case VirtualKey::VK_QUESTION: keyboardIn[10] = 0x6F; break;  // /-?
      case VirtualKey::VK_LCTRL:
      case VirtualKey::VK_RCTRL: ctrlDown = true; keyboardIn[13] = 0x6F; break;  // L Enter
      case VirtualKey::VK_RETURN:
      case VirtualKey::VK_KP_ENTER: keyboardIn[14] = 0x6F; break;  // R Enter

      case VirtualKey::VK_LSHIFT:
      case VirtualKey::VK_RSHIFT: shiftFlag = 0x5F; break;  // L and R shift
      case VirtualKey::VK_TAB: shiftFlag = 0x3F; break;  // TAB
      default: break;
      }
};

// **************************************************************************************************
void IRAM_ATTR drawScanline(void * arg, uint8_t * dest, int scanLine)
{
  // draws "scanlinesPerCallback" scanlines every time drawScanline() is called
  for (int i = 0; i < scanlinesPerCallback; ++i) {
    // fill border with background color
    memset(dest, darkbgcolor, width);
    if (!(scanLine < borderSize || scanLine >= height - borderSize)) {
      for (int i = 0; i < 48; ++i)  // 48 bytes must transformed to 288 pixels on row
        {
          unsigned char videobyte = pmd85ram[0xC000 + (scanLine-borderSize)*64 + i];  // Video RAM start address + row start + byte on row
          // Set 6 pixel on the screen
          if ((videobyte & 0x80) && blinkFlag) {    // If blinking
            VGA_PIXELINROW(dest, i*6+borderXSize) = bgcolor;
            VGA_PIXELINROW(dest, i*6+1+borderXSize) = bgcolor;
            VGA_PIXELINROW(dest, i*6+2+borderXSize) = bgcolor;
            VGA_PIXELINROW(dest, i*6+3+borderXSize) = bgcolor;
            VGA_PIXELINROW(dest, i*6+4+borderXSize) = bgcolor;
            VGA_PIXELINROW(dest, i*6+5+borderXSize) = bgcolor;
          } else {
            if (videobyte & 0x40) {   // If bright
              if(videobyte & 0x01) VGA_PIXELINROW(dest, i*6+borderXSize) = brcolor;
              else VGA_PIXELINROW(dest, i*6+borderXSize) = bgcolor;
              if(videobyte & 0x02) VGA_PIXELINROW(dest, i*6+1+borderXSize) = brcolor;
              else VGA_PIXELINROW(dest, i*6+1+borderXSize) = bgcolor;
              if(videobyte & 0x04) VGA_PIXELINROW(dest, i*6+2+borderXSize) = brcolor;
              else VGA_PIXELINROW(dest, i*6+2+borderXSize) = bgcolor;
              if(videobyte & 0x08) VGA_PIXELINROW(dest, i*6+3+borderXSize) = brcolor;
              else VGA_PIXELINROW(dest, i*6+3+borderXSize) = bgcolor;
              if(videobyte & 0x10) VGA_PIXELINROW(dest, i*6+4+borderXSize) = brcolor;
              else VGA_PIXELINROW(dest, i*6+4+borderXSize) = bgcolor;
              if(videobyte & 0x20) VGA_PIXELINROW(dest, i*6+5+borderXSize) = brcolor;
              else VGA_PIXELINROW(dest, i*6+5+borderXSize) = bgcolor;
            } else {    // If normal color
              if(videobyte & 0x01) VGA_PIXELINROW(dest, i*6+borderXSize) = fgcolor;
              else VGA_PIXELINROW(dest, i*6+borderXSize) = bgcolor;
              if(videobyte & 0x02) VGA_PIXELINROW(dest, i*6+1+borderXSize) = fgcolor;
              else VGA_PIXELINROW(dest, i*6+1+borderXSize) = bgcolor;
              if(videobyte & 0x04) VGA_PIXELINROW(dest, i*6+2+borderXSize) = fgcolor;
              else VGA_PIXELINROW(dest, i*6+2+borderXSize) = bgcolor;
              if(videobyte & 0x08) VGA_PIXELINROW(dest, i*6+3+borderXSize) = fgcolor;
              else VGA_PIXELINROW(dest, i*6+3+borderXSize) = bgcolor;
              if(videobyte & 0x10) VGA_PIXELINROW(dest, i*6+4+borderXSize) = fgcolor;
              else VGA_PIXELINROW(dest, i*6+4+borderXSize) = bgcolor;
              if(videobyte & 0x20) VGA_PIXELINROW(dest, i*6+5+borderXSize) = fgcolor;
              else VGA_PIXELINROW(dest, i*6+5+borderXSize) = bgcolor;
            }
          }
        }
    }
    // go to next scanline
    ++scanLine;
    dest += width;
  }
  if (scanLine == height) {
    // signal end of screen
    vTaskNotifyGiveFromISR(mainTaskHandle, NULL);
  }
}

// **************************************************************************************************
// microSD: tries the MISO pin of the detected board first and then the other one, so a wrong
// board detection does not leave the card unusable.
static bool mountSD(int miso)
{
  sdSPI.end();
  sdSPI.begin(pin_sd_clk, miso, pin_sd_mosi, pin_sd_cs);
  if (SD.begin(pin_sd_cs, sdSPI)) { pin_sd_miso = miso; return true; }
  SD.end();
  return false;
}

static void initTape()
{
  int first  = (pin_sd_miso == SD_MISO_OLIMEX) ? SD_MISO_OLIMEX : SD_MISO_TTGO;
  int second = (first == SD_MISO_TTGO) ? SD_MISO_OLIMEX : SD_MISO_TTGO;
  if (!mountSD(first) && !mountSD(second)) {
    Serial.println("microSD: card not found (tape not available)");
    return;
  }
  Serial.printf("microSD: mounted (MISO=%d)\n", pin_sd_miso);
  file = SD.open("/tape.ptp", FILE_READ);   // File represents tape
  if (file) {
    isTape = true;
    Serial.printf("Tape: /tape.ptp (%u bytes)\n", (unsigned)file.size());
  } else {
    Serial.println("Tape: /tape.ptp not found on the card");
  }
}

// **************************************************************************************************
void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== Tesla PMD85 Emulator ===");

    BoardType miPlaca = detectarPlaca();
    if (miPlaca == BOARD_LILYGO_TTGO_VGA32) {
        Serial.println("Placa: TTGO VGA32");
        pin_sd_miso = SD_MISO_TTGO;
    } else if (miPlaca == BOARD_OLIMEX_FABGL) {
        Serial.println("Placa: Olimex FabGL");
        pin_sd_miso = SD_MISO_OLIMEX;
    } else {
        Serial.println("Placa no reconocida, usando pines TTGO por defecto");
    }

    mainTaskHandle = xTaskGetCurrentTaskHandle();

  // Settings for "sound generator"
  soundGenerator.attach(&square);
  square.enable(true);
  square.setSampleRate(SAMPLE_RATE);  // For square it is enough

  // microSD emulating tape recorder
  initTape();

  // Set VGA for PMD85 display green monitor
  DisplayController.begin();
  DisplayController.setScanlinesPerCallBack(scanlinesPerCallback);
  DisplayController.setDrawScanlineCallback(drawScanline);
  DisplayController.setResolution(VGA_400x300_60Hz);
  brcolor = DisplayController.createRawPixel(RGB222(0, 3, 0)); // bright green
  fgcolor = DisplayController.createRawPixel(RGB222(0, 2, 0)); // green
  bgcolor = DisplayController.createRawPixel(RGB222(0, 1, 0)); // dark green
  darkbgcolor = DisplayController.createRawPixel(RGB222(0, 0, 0)); // black
  width  = DisplayController.getScreenWidth();
  height = DisplayController.getScreenHeight();
  // Start keyboard
  PS2Controller.begin(PS2Preset::KeyboardPort0, KbdMode::GenerateVirtualKeys);

  // Set CPU bus functions and start it
  m_i8080.setCallbacks(auxPoint, readByte, writeByte, readWord, writeWord, readIO, writeIO); 
  m_i8080.reset();
  machineInit();
  machineReset();
  for (int i = 0; i < 16; i++) keyboardIn[i]=0xFF;

  // Set function pro Keyboard processing
  PS2Controller.keyboard()->onVirtualKey = [&](VirtualKey * vk, bool keyDown) {
      if (keyDown) {
        procesKeyDown(*vk);
    } else procesKeyUp(*vk);
  };
}

// **************************************************************************************************
// **************************************************************************************************
void loop()
{
  static int numCycles;
  if (resetReq)      { resetReq = false; machineReset(); }                                   // ESC
  if (tapeRewindReq) { tapeRewindReq = false; tapeRewind(); Serial.println("Tape rewound"); }  // Ctrl+Backspace
  numCycles = 0;
  while(numCycles < 30000) numCycles += m_i8080.step(); // approx. 30000 cycles per 16.6 milisec (60 Hz VGA)
  BlinkCnt++;
  if(BlinkCnt >= 30) { BlinkCnt=0; blinkFlag = !blinkFlag; }     // Blink flag manipulation

  // wait for vertical sync
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
