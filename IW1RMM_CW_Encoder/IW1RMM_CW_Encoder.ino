/*
 * VK2IDL CW Morse Encoder - LVGL Edition
 * Version: Version: 7.1.4  (sketch file: _714) - Touch calibration interactive
 * Hardware: ESP32 CYD (ESP32-2432S028R)
 * Display: ILI9341 320x240 with XPT2046 Touch
 *
 * 
 *
 * Modified by IW1RMM (Mauri) - 2026
 * Based on VK2IDL original workv7
 */

#include <lvgl.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include "DacESP32.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_bt.h"

// BLE UUIDs per UART service (standard Nordic UART)
#define BLE_SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // RX dal client
#define BLE_CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // TX al client

// ============================================================================
// HARDWARE OBJECTS
// ============================================================================
DacESP32 dac1(GPIO_NUM_26);

// ============================================================================
// PIN DEFINITIONS
// ============================================================================
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1
#define TFT_BL   21

#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define AUDIO_OUT    26
#define PADDLE_DAH   22
#define PADDLE_DIT   35
#define MORSE_OUT    27

// SD Card - pin nativi CYD (dedicati, fisicamente cablati alla SD sul PCB)
// I pin della SD NON sono condivisi con TFT (13/14/15) o RGB LED.
// La SD usa il bus SPI VSPI (stesso del touch), ma con CS separato (5 vs 33).
#define SD_CS   5   // Chip Select SD (unico per la SD card)
#define SD_MOSI 23  // Master Out Slave In
#define SD_MISO 19  // Master In Slave Out
#define SD_SCK  18  // Serial Clock

// ============================================================================
// EEPROM (Preferences) KEYS
// ============================================================================
#define PREF_RATIO      "ratio"
#define PREF_WEIGHT     "weight"
#define PREF_PAD_REV    "padrev"
#define PREF_BEACON_INT "bcnint"
#define PREF_WPM        "wpm"
#define PREF_VOL        "vol"
#define PREF_FREQ       "freq"
// Prefisso chiavi NVS per memorie K3NG A-Z
// La chiave sarà "mem_A", "mem_B", ... "mem_Z"
#define PREF_MEM_PREFIX "mem_"

// ============================================================================
// CONSTANTS
// ============================================================================
const int screenWidth  = 320;
const int screenHeight = 240;
const String VERSION   = "v7.1.4";
const String TITLE     = "VK2IDL CW ENCODER";

const int MIN_WPM  = 5;
const int MAX_WPM  = 100;
const int MIN_FREQ = 300;
const int MAX_FREQ = 1200;
unsigned long lastClockUpdate = 0;
#define toneVol1 DAC_CW_SCALE_1
#define toneVol2 DAC_CW_SCALE_2
#define toneVol3 DAC_CW_SCALE_4
#define toneVol4 DAC_CW_SCALE_8

// ============================================================================
// DISPLAY QUEUE  Ã¢â‚¬â€ messaggi da morseTask Ã¢â€ â€™ lvglTask
// ============================================================================
typedef enum {
  DISP_TX_LED = 0,   // payload: u8 (0=off, 1=on)
  DISP_TX_BAR,       // payload: char (append to blue bar)
  DISP_CLEAR,        // clear all display areas
  DISP_WPM,          // payload: i32 (new wpm value)
  DISP_VOL,          // payload: i32 (new vol level)
  DISP_FREQ,         // payload: i32 (new freq value)
  DISP_IAMBIC_MODE,  // payload: i32 (KeyerMode enum)
  DISP_KEYER_LABEL,  // payload: i32 (0=KEYER/yellow, 1=BUFFER/green)
  DISP_TUNE,         // payload: u8 (0=off, 1=on)
  DISP_BEACON,       // payload: u8 (0=off, 1=on)
  DISP_FARNSWORTH,   // payload: i32 (WPM se >0=ON, 0=OFF)
} DisplayMsgType;

typedef struct {
  DisplayMsgType type;
  union {
    int32_t  i32;
    uint8_t  u8;
    char     ch;
  } payload;
} DisplayMsg;

static QueueHandle_t displayQueue = NULL;

// Helper: post dal morseTask senza bloccare
inline void postDisplay(DisplayMsgType t, int32_t val = 0) {
  DisplayMsg m;
  m.type        = t;
  m.payload.i32 = val;
  xQueueSend(displayQueue, &m, 0);   // non-blocking: se piena, scarta
}
inline void postDisplayChar(char c) {
  // Espandi prosign e caratteri speciali in etichette leggibili <XX>
  const char* prosign = nullptr;
  switch ((unsigned char)c) {
    case '+':  prosign = "<AR>"; break;  // .-.-. AR end of message
    case '=':  prosign = "<BT>"; break;  // -...- BT paragraph/separator
    case '~':  prosign = "<SK>"; break;  // ...-.- SK end of work
    case '^':  prosign = "<SN>"; break;  // ...-. SN understood
    case '&':  prosign = "<AS>"; break;  // .-... AS wait
    case '(':  prosign = "<KN>"; break;  // -.--.  KN over to you only
    default:   break;
  }
  if (prosign) {
    // Manda ogni carattere dell'etichetta sulla queue
    for (const char* p = prosign; *p; p++) {
      DisplayMsg m;
      m.type       = DISP_TX_BAR;
      m.payload.ch = *p;
      xQueueSend(displayQueue, &m, 0);
    }
  } else {
    DisplayMsg m;
    m.type       = DISP_TX_BAR;
    m.payload.ch = c;
    xQueueSend(displayQueue, &m, 0);
  }
}

// ============================================================================
// MUTEX per variabili condivise
// ============================================================================
static SemaphoreHandle_t xMutex = NULL;

#define LOCK()   xSemaphoreTake(xMutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(xMutex)

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
TFT_eSPI        tft = TFT_eSPI();
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
Preferences     preferences;

int touch_x_min, touch_x_max, touch_y_min, touch_y_max;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t         buf[screenWidth * 10];

// Morse parameters (accesso con LOCK se scritti da UI task)
int    wpm         = 20;
int    audioTone   = 600;
int    volumeLevel = 2;
dac_cosine_atten_t toneVol = toneVol2;
bool   isMuted     = false;

float  dahRatio       = 3.0f;
float  spacing_Weight = 1.0f;
bool   paddleReversed = false;
int    beaconIntervalSec = 60;
int    contestCounter    = 1;
long gmtOffsetHours = 0; // 

float  ditLength  = 60.0f;
float  dahLength  = 180.0f;
float  charSpace  = 180.0f;
float  elemSpace  = 60.0f;
float  wordSpace_ = 420.0f;

// Farnsworth Timing
bool   farnsworthEnabled = false;  // On/Off
int    farnsworthWPM = 12;         // Effective WPM (spacing più lento)
int    tempFarnsworthWPM = 12;  // ← AGGIUNGI QUESTA RIGA (valore temporaneo nel popup)

// SD Card Logging e // VARIABILI GLOBALI HSCW
bool     sdLoggingEnabled = false;   // On/Off
File     logFile;                     // File handle
String   logFileName = "";            // Nome file corrente
unsigned long systemStartTime = 0;   // millis() all'avvio (per uptime)

// HSCW popup slider
int      tempHscwWPM = 60;           // Valore temporaneo slider
lv_obj_t *popupHscwSlider = NULL;    // Slider LVGL
lv_obj_t *popupHscwLabel = NULL;     // Label valore
// OROLOGIO
lv_obj_t * ui_LabelClock = NULL; // Aggiungi questa riga in alto


// Paddle state (solo morseTask scrive/legge)
volatile int  LEFTpaddleState  = HIGH;
volatile int  RIGHTpaddleState = HIGH;
int  ditPaddleFlag  = 0;
int  dahPaddleFlag  = 0;
bool dahOnFlag      = false;
bool ditOnFlag      = false;
bool dahToneFlag    = false;
bool ditToneFlag    = false;
bool manual_ON      = false;
unsigned long paddleStartTime = 0;
unsigned long paddleEndTime   = 0;
unsigned long paddleCountTime = 0;

// Paddle decoder identico al vecchio sketch VK2IDL
// morseNum accumula i bit (dit=1, dah=0) per decodifica
static int           morseNum      = 1;    // accumulatore albero binario
static bool          paddleChar    = false; // carattere pronto per decodifica
static bool          paddleSpace   = false; // flag word space
static unsigned long paddleOutTime = 0;     // fine ultimo elemento
static unsigned long paddleInTime  = 0;     // tempo corrente

// Tabella mySet[] estesa ITU-R M.1677 + prosign
// Accesso: morseNum parte da 1, dit = morseNum<<1, dah = (morseNum<<1)+1
// Prosign visibili: SK=~ (..-.-)  SN=^ (...-.)  KN/paren=( (-.--.)
static const char mySet[] = {
  0,    //   0  unused
  ' ',  //   1  root / word space
  'E',  //   2  .
  'T',  //   3  -
  'I',  //   4  ..
  'A',  //   5  .-
  'N',  //   6  -.
  'M',  //   7  --
  'S',  //   8  ...
  'U',  //   9  ..-
  'R',  //  10  .-.
  'W',  //  11  .--
  'D',  //  12  -..
  'K',  //  13  -.-
  'G',  //  14  --.
  'O',  //  15  ---
  'H',  //  16  ....
  'V',  //  17  ...-
  'F',  //  18  ..-.
  0,    //  19  ..--
  'L',  //  20  .-..
  0,    //  21  .-.- (prefisso AR)
  'P',  //  22  .--.
  'J',  //  23  .---
  'B',  //  24  -...
  'X',  //  25  -..-
  'C',  //  26  -.-.
  'Y',  //  27  -.--
  'Z',  //  28  --..
  'Q',  //  29  --.-
  0,    //  30  ---.
  0,    //  31  ----
  '5',  //  32  .....
  '4',  //  33  ....-
  '^',  //  34  ...-. SN understood
  '3',  //  35  ...--
  0,    //  36
  0,    //  37
  0,    //  38
  '2',  //  39  ..---
  '&',  //  40  .-... AS/wait
  0,    //  41
  '+',  //  42  .-.-. AR  (era erroneamente a 43)
  0,    //  43  .-.-- non assegnato
  0,    //  44
  0,    //  45
  0,    //  46
  '1',  //  47  .----
  '6',  //  48  -....
  '=',  //  49  -...- BT
  '/',  //  50  -..-.  slash
  0,    //  51
  0,    //  52
  0,    //  53
  '(',  //  54  -.--.  KN / open paren
  0,    //  55
  '7',  //  56  --...
  0,    //  57
  0,    //  58
  0,    //  59
  '8',  //  60  ---.
  0,    //  61
  '9',  //  62  ----.
  '0',  //  63  -----
  0,    //  64
  0,    //  65
  0,    //  66
  0,    //  67
  0,    //  68
  '~',  //  69  ...-.- SK end of work
  0,    //  70
  0,    //  71
  0,    //  72
  0,    //  73
  0,    //  74
  0,    //  75
  '?',  //  76  ..--.. question mark
  0,    //  77
  0,    //  78
  0,    //  79
  0,    //  80
  0,    //  81
  0,    //  82
  0,    //  83
  0,    //  84
  '.',  //  85  .-.-.- period
  0,    //  86
  0,    //  87
  0,    //  88
  0,    //  89
  '@',  //  90  .--.-. at sign
  0,    //  91
  0,    //  92
  0,    //  93
  '\'', //  94  .----. apostrophe
  0,    //  95
  0,    //  96
  '-',  //  97  -....- hyphen
  0,    //  98
  0,    //  99
  0,    // 100
  0,    // 101
  0,    // 102
  0,    // 103
  0,    // 104
  0,    // 105
  ';',  // 106  -.-.-. semicolon
  '!',  // 107  -.-.-- exclamation
  0,    // 108
  ')',  // 109  -.--.- close paren
  0,    // 110
  0,    // 111
  0,    // 112
  0,    // 113
  0,    // 114
  ',',  // 115  --..-- comma
  0,    // 116
  0,    // 117
  0,    // 118
  0,    // 119
  ':',  // 120  ---... colon
};
static const int mySetSize = sizeof(mySet);

// Stato morse (condivisi Ã¢â‚¬â€ usa LOCK)
String buffer       = "";
bool   isTuneMode   = false;
bool   isTxActive   = false;
bool   stopRequested = false;

bool     sdBusReady = false;   // bus pronto
bool     sdAvailable  = false; // SD inizializzata correttamente

// Testo caricato
String   sdText       = "";    // testo completo del file
String   sdFileName   = "";    // nome file corrente
int      sdCharIndex  = 0;     // indice prossimo carattere da inviare

// Stato player
enum SdState { SD_IDLE, SD_PLAYING, SD_PAUSED };
volatile SdState sdState = SD_IDLE;

// File list
static const int SD_MAX_FILES = 20;
String   sdFileList[SD_MAX_FILES];
int      sdFileCount  = 0;

// UI refs (tab SD)
lv_obj_t *lblSdFile    = NULL;  // nome file + lunghezza
lv_obj_t *lblSdInfo    = NULL;  // freq + vol
lv_obj_t *lblSdText    = NULL;  // preview testo (riga 1)
lv_obj_t *lblSdRow[5] = {NULL,NULL,NULL,NULL,NULL}; // 5 righe testo SD
lv_obj_t *lblSdWpm     = NULL;  // valore WPM
lv_obj_t *btnSdPlay    = NULL;
lv_obj_t *btnSdPause   = NULL;
lv_obj_t *btnSdStop    = NULL;
lv_obj_t *sdPopup      = NULL;  // popup lista file
bool   beaconActive  = false;
unsigned long beaconInterval  = 60000UL;
unsigned long lastBeaconTime  = 0;

// Keyer mode
enum KeyerMode { IAMBIC_A, IAMBIC_B, MANUAL, AUTO_MODE };
KeyerMode currentMode = IAMBIC_B;
bool isBufferMode = false;

enum QrssMode {
  QRSS_OFF  = 0,
  QRSS_3    = 3,    // 3 secondi per dit
  QRSS_6    = 6,    // 6 secondi per dit
  QRSS_10   = 10,   // 10 secondi per dit
  QRSS_30   = 30    // 30 secondi per dit
};
// QRSS (Slow Speed CW)
QrssMode qrssMode = QRSS_OFF;        // Modalità QRSS corrente
float    qrssDitMs = 1000.0f;        // Durata dit in ms (calcolato da qrssMode)

// BLE (Bluetooth Low Energy)
bool bleEnabled = false;
bool bleClientConnected = false;
volatile bool bleUiUpdatePending = false;
volatile bool bleLogEnabledPending = false;
volatile bool bleLogDisabledPending = false;
BLEServer         *bleServer             = NULL;
BLECharacteristic *bleTxCharacteristic  = NULL;
BLECharacteristic *bleRxCharacteristic  = NULL;
String bleRxBuffer = "";

// Messaggi predefiniti
String cqMessage     = "CQ CQ DE IW1RMM IW1RMM K";
String nameMessage   = "NAME MAURI MAURI K";
String antRigMessage = "RIG IC7300 ANT DIPOLE K";
String qthMessage    = "QTH MILAN MILAN K";
String rstMessage    = "UR 599 599 K";
String testMessage   = "TEST DE IW1RMM K";
String macroMessage  = "";
// K3NG memories A-ZwinkeyModecheckSerialWinkey()
#define NUM_MEM 26
String k3ngMem[NUM_MEM];   // k3ngMem[0]=A ... k3ngMem[25]=Z

// ============================================================================
// WINKEY WK3 PROTOCOL - variabili globali
// ============================================================================
bool     winkeyMode = false;         // true=Winkey, false=K3NG
bool     winkeyHostOpen = false;     // Winkey handshake completato
uint8_t  winkeyVersion = 0x23;       // Version byte WK2 (0x23 = rev 2.3)
uint8_t  winkeyModeReg = 0x00;       // Mode register (Iambic B = 0x00 per spec WK3)
uint8_t  winkeyStatusByte = 0xC0;    // Status byte corrente (0xC0=idle, tag MSB=110)
bool     winkeyTune = false;         // Tune mode attivo via Winkey
unsigned long winkeyLastStatus = 0;  // Timestamp ultimo status inviato
// Winkey PTT / timing parameters
uint8_t wkPttLead        = 0;   // 0x04 p1: PTT lead time (x10ms)
uint8_t wkPttTailTime    = 0;   // 0x04 p2: PTT tail time (x10ms)
uint8_t wkFirstExtension = 0;   // 0x10 First extension (ms)
uint8_t wkKeyComp        = 0;   // 0x11 Key compensation (ms)
uint8_t wkPaddleSP       = 50;  // 0x12 Paddle switchpoint (default 50%)
uint8_t wkPttTail        = 0;   // 0x1A PTT tail buffered
uint8_t wkPttLeadTail    = 0;   // 0x1B PTT lead/tail buffered
// Speed pot (nessun hardware pot fisico)
uint8_t wkSpeedPotMin    = 5;   // 0x05 MinWPM
uint8_t wkSpeedPotRange  = 35;  // 0x05 WPM Range
// X1MODE / X2MODE / PinCfg extension registers
uint8_t wkX1Mode         = 0;   // Admin 0x0F p15 / Admin subcmd 0x0F
uint8_t wkX2Mode         = 0;   // Load Defaults p9
uint8_t wkPinCfg         = 0x05; // 0x09 PinConfig default (PTT+Sidetone enable)

// ============================================================================
// NUOVI FLAG v71
// ============================================================================
bool txEnabled   = true;   // \X — TX key output enable/disable
bool echoEnabled = false;  // \E — Echo paddle chars to Serial/BLE


// Winkey buffer (per gestire messaggi parziali)
#define WINKEY_BUF_SIZE 120
uint8_t  winkeyBuf[WINKEY_BUF_SIZE];
int      winkeyBufLen = 0;




bool contestMode   = false;
bool contestActive = false;

// Display ticker a 3 righe (solo lvglTask/loop)
// Ogni riga ÃƒÂ¨ larga MAX_COLS caratteri, riempita di spazi inizialmente
// I nuovi caratteri entrano da DESTRA, tutto scorre verso SINISTRA
// Display scroll buffers Ã¢â‚¬â€ logica identica al vecchio sketch VK2IDL originale
// L3=barra blu (font grande), L2=nera bassa, L1=nera alta
// drawRightString equivalente: label allineata a destra, testo cresce da sinistra
String stringBuf_L3 = "";   // riga blu (attiva): max buffMainMax chars
String stringBuf_L2 = "";   // nera bassa (overflow L3)
String stringBuf_L1 = "";   // nera alta  (overflow L2)
const int buffMainMax = 32; // max chars riga blu   (font_16, ~9.5px/char su 316px)
const int buffSecMax  = 42; // max chars righe nere (font_12)

// Timer
hw_timer_t       *morseTimer     = NULL;
volatile uint32_t morseTimerTick = 0;

// Task handles
TaskHandle_t morseTaskHandle  = NULL;
// lvglTaskHandle rimosso: LVGL gira nel loop()
TaskHandle_t serialTaskHandle = NULL;

// ============================================================================
// LVGL OBJECTS
// ============================================================================
lv_obj_t *tabview, *tabMorse1, *tabMorse2, *tabSD, *tabSettings, *tabSpare;
lv_obj_t *labelTxLed;
lv_obj_t *labelLog1, *labelLog2, *labelTx;
lv_obj_t *btnCQ, *btnName, *btnAntRig, *btnClear;
lv_obj_t *btnProtocol = NULL;  // K3NG/Winkey toggle (tab Settings)
lv_obj_t *btnFarnsworth = NULL;  // Farnsworth toggle (tab Settings)
lv_obj_t *btnSdLog = NULL;  // SD Logging toggle (tab Settings)
lv_obj_t *btnQrss = NULL;  // QRSS toggle (tab Settings)
lv_obj_t *btnBle = NULL;  // BLE toggle (tab Settings)
lv_obj_t *btnQth, *btnRst, *btnTest, *btnKeyerMode;
lv_obj_t *btnRatio, *btnWeight, *btnTune, *btnMacro;
lv_obj_t *btnPadRev, *btnBeacon, *btnContest, *btnStop;
lv_obj_t *btnWpmUp, *btnWpmDown, *btnVolUp, *btnVolDown, *btnFreqUp, *btnFreqDown;
lv_obj_t *btnIambicMode;
lv_obj_t *ledTxCircle;
lv_obj_t *ledTxCircle1, *ledTxCircle2;
lv_obj_t *btnIambicMode1, *btnIambicMode2;
lv_obj_t *labelLog1_t1, *labelLog2_t1, *labelTx_t1;
lv_obj_t *labelLog1_t2, *labelLog2_t2, *labelTx_t2;
lv_obj_t *labelWpm1, *labelVol1, *labelFreq1;
lv_obj_t *labelWpm2, *labelVol2, *labelFreq2;

// Popup
lv_obj_t *popupBox        = NULL;
// Variabili per il popup di Farnsworth
lv_obj_t *popupValueLabel = NULL;      // usata da altri popup (ratio, weight, ecc)
lv_obj_t *popupFarnsworthLabel1 = NULL; // label "Effective: xx WPM"
lv_obj_t *popupFarnsworthLabel2 = NULL; // label "Chars: xx WPM"
// Fine Variabili per il popup di Farnsworth

// Editor messaggi
lv_obj_t *msgEditorBox    = NULL;
lv_obj_t *msgEditorKb     = NULL;   // keyboard su lv_layer_top()
lv_obj_t *msgEditorTA     = NULL;   // textarea
int       msgEditIndex    = -1;     // 0=CQ,1=NAME,2=ANTRIG,3=QTH,4=RST,5=TEST
void showMsgEditor(int idx);
void onMsgEditorSave(lv_event_t *e);
void onMsgEditorCancel(lv_event_t *e);
float tempRatio   = 3.0f;
float tempWeight  = 1.0f;
bool  tempPadRev  = false;
int   tempBeacon  = 60;
int   tempContest = 1;

// Volume display helpers
int volumeUiLevel(int v) { return (v == 0) ? 0 : 5 - v; }

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================
void setupPins();


// ============================================================================
// [v56] CONFIGURAZIONE BUS SPI SUL CYD con ILI9341
// ============================================================================
// Fonte: GitHub TFT_eSPI issue #3706 (aprile 2025), JohannieK:
//   "Con ILI9341 + XPT2046 il touch funziona SOLO su HSPI.
//    Su VSPI si triggera con z=4096, x=0, y=0 continuamente."
//
// Il CYD ha 3 periferiche SPI con pin fisici dedicati:
//   1. Display ILI9341  - pin 13/14/12/15 (MOSI/SCK/MISO/CS)
//   2. Touch XPT2046    - pin 32/25/39/33 (MOSI/SCK/MISO/CS)
//   3. SD Card          - pin 23/18/19/5  (MOSI/SCK/MISO/CS)
//
// Configurazione CORRETTA per ILI9341 (v56):
//   TFT    -> VSPI (default TFT_eSPI, senza USE_HSPI_PORT)
//   Touch  -> HSPI (touchSPI = SPIClass(HSPI), CS=33)
//   SD     -> HSPI (sdSPI = SPIClass(HSPI), CS=5)
//
// Touch e SD condividono HSPI fisicamente ma con CS separati (33 vs 5):
// il protocollo SPI serializza le transazioni via CS, nessun conflitto.
//
// User_Setup.h RICHIESTO:
//   #define TFT_MISO  12  <- DEFINITA (non commentata)
//   #define TFT_MOSI  13
//   #define TFT_SCLK  14
//   #define TFT_CS    15
//   #define TFT_DC    2
//   #define TFT_RST   -1
//   #define TOUCH_CS  -1
//   // NO USE_HSPI_PORT <- toglila se presente
//   #define SPI_FREQUENCY  40000000
// ============================================================================


void setupTouch();
void setupLVGL();
void setupTasks();
void setupTimer();
void loadPreferences();
void savePreferences();
void calculateMorseTiming();

void my_disp_flush(lv_disp_drv_t*, const lv_area_t*, lv_color_t*);
void my_touchpad_read(lv_indev_drv_t*, lv_indev_data_t*);

void createUI();
void createTabMorse1();
void createTabMorse2();
void createTabSD();
void sdScanFiles(String targetFile = "");
void sdLoadFile(String filename);
void sdOpenFilePopup();
void sdCloseFilePopup();
void sdUpdateFileLabel();
void sdUpdateInfoLabel();
void sdUpdateWpmLabel();
void sdUpdateButtons();
void sdPlayerTask();    // chiamata da loop() Ã¢â‚¬â€ invia char al buffer Morse
void createTabSettings();
void createTabSpare();
void createLogAreas(lv_obj_t*);
void createCommonControls(lv_obj_t*, lv_obj_t**, lv_obj_t**, lv_obj_t**, lv_obj_t**, lv_obj_t**);

// Event handlers
void onWpmUp(lv_event_t*); void onWpmDown(lv_event_t*);
void onVolUp(lv_event_t*); void onVolDown(lv_event_t*);
void onFreqUp(lv_event_t*); void onFreqDown(lv_event_t*);
void onIambicMode(lv_event_t*);
void onCQ(lv_event_t*); void onName(lv_event_t*); void onAntRig(lv_event_t*);
void onClear(lv_event_t*); void onQth(lv_event_t*); void onRst(lv_event_t*);
void onTest(lv_event_t*); void onKeyerMode(lv_event_t*);
void onRatio(lv_event_t*); void onWeight(lv_event_t*);
void onTune(lv_event_t*); void onMacro(lv_event_t*);
void onPadRev(lv_event_t*); void onBeacon(lv_event_t*);
void onContest(lv_event_t*); void onStop(lv_event_t*);
void onRecalibrate(lv_event_t*);
void onProtocolToggle(lv_event_t*);
void onFarnsworthToggle(lv_event_t*);
void showPopupFarnsworth();
void onFarnsworthMinus(lv_event_t*);
void onFarnsworthPlus(lv_event_t*);
void onFarnsworthSave(lv_event_t*);
void onSdLogToggle(lv_event_t*);
void sdLogInit();
String getUptimeString();
void showPopupHscw(int currentWPM);
void onHscwSliderChange(lv_event_t*);
void onHscwSave(lv_event_t*);
void onHscwCancel(lv_event_t*);
void onQrssToggle(lv_event_t*);
void updateQrssButton();
const char* getQrssModeString(QrssMode mode);
void onBleToggle(lv_event_t*);
void bleInit();
void bleStop();
void updateBleButton();
void checkSerialBle();
void bleSend(const String &data);

// Forward declarations per callback BLE
class MyServerCallbacks;
class MyCallbacks;

void updateStatusLabels();

// Popup
lv_obj_t* createPopupBase(const char*);
void closePopup();
void showPopupRatio();  void showPopupWeight();
void showPopupPadRev(); void showPopupBeacon();
void showPopupContest();
void onRatioMinus(lv_event_t*);  void onRatioPlus(lv_event_t*);  void onRatioSave(lv_event_t*);
void onWeightMinus(lv_event_t*); void onWeightPlus(lv_event_t*); void onWeightSave(lv_event_t*);
void onPadRevToggle(lv_event_t*); void onPadRevSave(lv_event_t*);
void onBeaconMinus(lv_event_t*); void onBeaconPlus(lv_event_t*); void onBeaconSave(lv_event_t*);
void onContestMinus(lv_event_t*); void onContestPlus(lv_event_t*); void onContestSave(lv_event_t*);
void onPopupCancel(lv_event_t*);

// Morse
void addToBuffer(String);
void clearBuffer();
void sendFromBuffer();
void sendMorseChar(char);
void morseToneOn(int ms);
void morseToneOff(int ms);
void checkPaddle();
void paddle_morse_On();
void paddle_morse_Off();
void paddle_beginDIT();
void paddle_beginDAH();
void paddleTimer();

// lvglTask display processing
void processDisplayQueue();
void lvgl_updateTxLed(bool on);
void lvgl_appendTxBar(char c);
void lvgl_clearAll();

// Serial
void checkSerial();
void sendStatus();
void sendHelp();
void sendContestMsg();

// Tasks
void morseTask(void*);
// void lvglTask rimosso  LVGL nel loop()
void serialTask(void*);

void IRAM_ATTR onMorseTimer();

String getModeName();
int calculateDitDuration();

// ============================================================================
// TIMER-BASED NON-BLOCKING DELAY (chiamato SOLO da morseTask / Core 1)
// [FIX-2] sostituisce delay() con busy-wait sul tick HW a 1 kHz
// ============================================================================
static inline void morseDelay(uint32_t ms) {
  if (ms == 0) return;
  uint32_t target = morseTimerTick + ms;
  while ((int32_t)(target - morseTimerTick) > 0) {
    vTaskDelay(1 / portTICK_PERIOD_MS);  // cede CPU ogni 1ms, non spreca Core 0
  }
}
// ============================================================================
// BLE CALLBACKS
// ============================================================================

// Callback connessione/disconnessione client
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    bleClientConnected = true;
    bleUiUpdatePending = true;
    Serial.println("BLE: Client connected");
  }
  void onDisconnect(BLEServer *pServer) {
    bleClientConnected = false;
    bleUiUpdatePending = true;
    Serial.println("BLE: Client disconnected");
    if (bleEnabled) BLEDevice::startAdvertising();
  }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = pCharacteristic->getValue().c_str();
    for (int i = 0; i < rxValue.length(); i++) {
      bleRxBuffer += rxValue[i];
    }
  }
};
// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial1.end();   // â† aggiungi questa
  Serial.begin(115200);
  systemStartTime = millis();  // Marca tempo avvio per uptime
  delay(500);
 // SD PRIMA DI TUTTO - prima che tft.begin() occupi il bus
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(10);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS)) {
    sdAvailable = true;
    Serial.println("SD Card OK");
    bleSend("SD Card OK\n");
  } else {
    sdAvailable = false;
    Serial.println("SD Card non disponibile");
    bleSend("SD Card non disponibile\n");
  }
  
  Serial.println("\n\n=================================");
  Serial.println(TITLE + " " + VERSION);
  Serial.println("=================================");
  bleSend("\n\n=================================\n");
  bleSend(TITLE + " " + VERSION + "\n");
  bleSend("=================================\n");

  // Crea mutex e queue PRIMA di qualsiasi task
  xMutex       = xSemaphoreCreateMutex();
  displayQueue = xQueueCreate(64, sizeof(DisplayMsg));

  Serial.println("Step 1: Preferences...");
  bleSend("Step 1: Preferences...\n");

  preferences.begin("cyd-morse", false);
  loadPreferences();
  gmtOffsetHours = preferences.getLong("gmt_off", 1); // Carica il GMT salvato, default 1
  preferences.end();
  Serial.println("Step 2: Pins...");
  bleSend("Step 2: Pins...\n");
  setupPins();
  
  Serial.println("Step 3: LVGL...");
  bleSend("Step 3: LVGL...\n");
  setupLVGL();
  
  Serial.println("Step 4: Touch...");
  bleSend("Step 4: Touch...\n");
  setupTouch();
  {
    Preferences p;
    p.begin("touch_cal", true);
    bool cal = p.getBool("calibrated", false);
    p.end();
    if (!cal) runTouchCalibration();
  }

  Serial.println("Step 5: Timer...");
  bleSend("Step 5: Timer...\n");
  setupTimer();

  Serial.println("Step 6: UI...");
  bleSend("Step 6: UI...\n");
  ledTxCircle1=NULL; ledTxCircle2=NULL;
  btnIambicMode1=NULL; btnIambicMode2=NULL;
  labelLog1_t1=NULL; labelLog2_t1=NULL; labelTx_t1=NULL;
  labelLog1_t2=NULL; labelLog2_t2=NULL; labelTx_t2=NULL;
  createUI();

  // Ticker inizializzato: stringBuf_L3/L2/L1 vuote, label mostrano "Ready"

  Serial.println("Step 7: Bluetooth & Tasks...");
  bleSetup();
  setupTasks();

  Serial.println("\n=== INIT COMPLETE ===\n");
  Serial.printf("FREE HEAP: %d bytes\n", ESP.getFreeHeap());
  sendHelp();
}

void savePreferences() {
  preferences.begin("cyd-morse", false);
  preferences.putInt(PREF_WPM, wpm);
  preferences.putInt(PREF_FREQ, audioTone);
  preferences.putInt(PREF_VOL, volumeLevel);
  preferences.putFloat(PREF_RATIO, dahRatio);
  preferences.putFloat(PREF_WEIGHT, spacing_Weight);
  preferences.putBool(PREF_PAD_REV, paddleReversed);
  preferences.putInt(PREF_BEACON_INT, beaconIntervalSec);
  preferences.putInt("contestcnt", contestCounter);
  preferences.end();
}
// Salva UNA memoria K3NG in NVS (chiamata dopo ogni \P)
void saveMemory(int idx) {
  if (idx < 0 || idx >= NUM_MEM) return;
  char key[8];
  snprintf(key, sizeof(key), "mem_%c", 'A' + idx);
  preferences.begin("cyd-morse", false);
  preferences.putString(key, k3ngMem[idx]);
  preferences.end();
}

// Carica tutte le memorie K3NG da NVS (chiamata da loadPreferences)
void loadMemories() {
  preferences.begin("cyd-morse", true);
  for (int i = 0; i < NUM_MEM; i++) {
    char key[8];
    snprintf(key, sizeof(key), "mem_%c", 'A' + i);
    k3ngMem[i] = preferences.getString(key, "");
  }
  preferences.end();
}

void loadPreferences() {
  preferences.begin("cyd-morse", true);
  
  // Parametri tecnici
  wpm               = preferences.getInt(PREF_WPM, 15);
  audioTone         = preferences.getInt(PREF_FREQ, 700);
  volumeLevel       = preferences.getInt(PREF_VOL, 2);
  dahRatio          = preferences.getFloat(PREF_RATIO, 3.0f);
  spacing_Weight    = preferences.getFloat(PREF_WEIGHT, 1.0f);
  paddleReversed    = preferences.getBool(PREF_PAD_REV, false);
  beaconIntervalSec = preferences.getInt(PREF_BEACON_INT, 60);
  contestCounter    = preferences.getInt("contestcnt", 1);

  // CARICAMENTO MESSAGGI (Fondamentale!)
  cqMessage     = preferences.getString("msg0", "CQ CQ DE IW1RMM IW1RMM K");
  nameMessage   = preferences.getString("msg1", "NAME MAURI MAURI K");
  antRigMessage = preferences.getString("msg2", "ANT DIPOLE RIG FT817 BK");
  qthMessage    = preferences.getString("msg3", "QTH GENOVA GENOVA BK");
  rstMessage    = preferences.getString("msg4", "RST 599 599 BK");
  testMessage   = preferences.getString("msg5", "TEST TEST DE IW1RMM K");
  
  preferences.end();

  beaconInterval = (unsigned long)beaconIntervalSec * 1000UL;
  calculateMorseTiming();
  loadMemories();   // v71: carica memorie K3NG A-Z da NVS

}





void setupTasks() {
  // morseTask su Core 0 separato da LVGL che gira nel loop() su Core 1
  xTaskCreatePinnedToCore(morseTask,  "MorseTask",  8192, NULL, 2, &morseTaskHandle,  0);//era 4096
  // serialTask su Core 0 priorit  bassa
  xTaskCreatePinnedToCore(serialTask, "SerialTask", 4096, NULL, 1, &serialTaskHandle, 0);//era 4096
  // LVGL gira nel loop() Core 1 garantito da Arduino framework
  
}


void setupPins() {
  pinMode(MORSE_OUT, OUTPUT);
  digitalWrite(MORSE_OUT, LOW);
  pinMode(PADDLE_DAH, INPUT_PULLUP);
  pinMode(PADDLE_DIT, INPUT);
  dac1.outputCW(audioTone, toneVol);
  dac1.disable();
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
}

// ============================================================================
// TOUCH CALIBRATION - namespace separato "touch_cal"
// ============================================================================
void touchCalSave(int xmin, int xmax, int ymin, int ymax) {
  Preferences p;
  p.begin("touch_cal", false);
  p.putInt("x_min", xmin);
  p.putInt("x_max", xmax);
  p.putInt("y_min", ymin);
  p.putInt("y_max", ymax);
  p.putBool("calibrated", true);
  p.end();
  Serial.printf("TouchCal saved: x=%d-%d y=%d-%d\n", xmin, xmax, ymin, ymax);
}

bool touchCalLoad(int &xmin, int &xmax, int &ymin, int &ymax) {
  Preferences p;
  p.begin("touch_cal", true);  // read-only
  bool ok = p.getBool("calibrated", false);
  if (ok) {
    xmin = p.getInt("x_min", 220);
    xmax = p.getInt("x_max", 3730);
    ymin = p.getInt("y_min", 387);
    ymax = p.getInt("y_max", 3763);
  }
  p.end();
  return ok;
}

void touchCalReset() {
  Preferences p;
  p.begin("touch_cal", false);
  p.clear();  // cancella SOLO namespace "touch_cal"
  p.end();
  Serial.println("TouchCal: reset to uncalibrated");
}

// Disegna croce di calibrazione sul TFT raw
void drawCross(int x, int y, uint16_t color) {
  tft.drawLine(x - 15, y, x + 15, y, color);
  tft.drawLine(x, y - 15, x, y + 15, color);
  tft.drawCircle(x, y, 5, color);
}

// Attende un tocco valido e restituisce coordinate raw XPT2046
// Restituisce true se tocco acquisito, false se timeout (10s) o annullato
bool waitTouch(int &rawX, int &rawY) {
  unsigned long start = millis();
  // Aspetta prima che il dito sia sollevato
  while (touchscreen.tirqTouched() && touchscreen.touched()) {
    if (millis() - start > 10000) return false;
    delay(10);
  }
  // Aspetta il tocco
  start = millis();
  while (true) {
    if (millis() - start > 10000) return false;
    // Escape via seriale: invia 'R' per annullare
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'R' || c == 'r') {
        Serial.println("TouchCal: ANNULLATO via seriale");
        return false;
      }
    }
    if (touchscreen.tirqTouched() && touchscreen.touched()) {
      TS_Point p = touchscreen.getPoint();
      rawX = p.x;
      rawY = p.y;
      delay(80);  // debounce
      return true;
    }
    delay(10);
  }
}

void runTouchCalibration() {
  Serial.println("TouchCal: START - invia 'R' da Serial per annullare");

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("CALIBRAZIONE TOUCH", 160, 20, 2);
  tft.drawString("Tocca il centro di ogni croce", 160, 45, 2);
  tft.setTextColor(TFT_YELLOW);
  tft.drawString("(Serial 'R' per annullare)", 160, 65, 2);

  // Margine dai bordi
  const int M = 20;
  const int W = screenWidth;   // 320
  const int H = screenHeight;  // 240

  // 4 punti: TL, TR, BR, BL
  int px[4] = { M,     W-M,   W-M,   M     };
  int py[4] = { M,     M,     H-M,   H-M   };
  const char* labels[4] = {
    "1/4: Alto-Sinistra",
    "2/4: Alto-Destra",
    "3/4: Basso-Destra",
    "4/4: Basso-Sinistra"
  };

  int rawX[4], rawY[4];

  for (int i = 0; i < 4; i++) {
    tft.fillRect(0, 80, W, H - 80, TFT_BLACK);  // pulisce area sotto istruzioni
    tft.setTextColor(TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(labels[i], 160, 110, 2);
    drawCross(px[i], py[i], TFT_GREEN);

    Serial.printf("TouchCal: punto %s\n", labels[i]);

    int rx, ry;
    if (!waitTouch(rx, ry)) {
      // Annullato o timeout: usa valori hardcoded
      Serial.println("TouchCal: ANNULLATO - uso valori default");
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_YELLOW);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("Calibrazione annullata", 160, 110, 2);
      tft.drawString("Uso valori default", 160, 135, 2);
      delay(2000);
      return;  // setupTouch() usera' i valori hardcoded
    }

    rawX[i] = rx;
    rawY[i] = ry;
    Serial.printf("  raw: x=%d y=%d\n", rx, ry);

    // Conferma visiva
    drawCross(px[i], py[i], TFT_RED);
    delay(400);
  }

  // Calcola valori finali
  int xmin = (rawX[0] + rawX[3]) / 2;  // media TL.x e BL.x
  int xmax = (rawX[1] + rawX[2]) / 2;  // media TR.x e BR.x
  int ymin = (rawY[0] + rawY[1]) / 2;  // media TL.y e TR.y
  int ymax = (rawY[2] + rawY[3]) / 2;  // media BR.y e BL.y

  Serial.printf("TouchCal: calcolato x=%d-%d y=%d-%d\n", xmin, xmax, ymin, ymax);

  // Validazione range ragionevole
  bool valid = (xmin > 100 && xmin < 1000) &&
               (xmax > 3000 && xmax < 4095) &&
               (ymin > 100 && ymin < 1000) &&
               (ymax > 3000 && ymax < 4095) &&
               (xmax - xmin > 2000) &&
               (ymax - ymin > 2000);

  if (!valid) {
    Serial.println("TouchCal: valori fuori range - uso default");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Valori non validi!", 160, 100, 2);
    tft.drawString("Uso valori default", 160, 125, 2);
    tft.setTextColor(TFT_YELLOW);
    tft.drawString("Ripeti con Calibra Touch", 160, 150, 2);
    delay(3000);
    return;
  }

  // Mostra risultato e chiede conferma (timeout 10s = conferma automatica)
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Calibrazione OK!", 160, 70, 2);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("x_min: " + String(xmin), 160, 100, 2);
  tft.drawString("x_max: " + String(xmax), 160, 120, 2);
  tft.drawString("y_min: " + String(ymin), 160, 140, 2);
  tft.drawString("y_max: " + String(ymax), 160, 160, 2);
  tft.setTextColor(TFT_YELLOW);
  tft.drawString("Tocca per confermare (10s auto)", 160, 195, 2);

  Serial.println("TouchCal: tocca per confermare o attendi 10s");

  // Attende conferma tocco o timeout 10s
  unsigned long t = millis();
  bool confirmed = false;
  while (millis() - t < 10000) {
    if (touchscreen.tirqTouched() && touchscreen.touched()) {
      confirmed = true;
      break;
    }
    delay(50);
  }

  if (confirmed || (millis() - t >= 10000)) {
    touchCalSave(xmin, xmax, ymin, ymax);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Salvato! Riavvio...", 160, 120, 2);
    delay(1500);
    ESP.restart();
  }
}




void setupTouch() {
  // Carica da NVS se calibrato, altrimenti usa valori hardcoded di default
  if (!touchCalLoad(touch_x_min, touch_x_max, touch_y_min, touch_y_max)) {
    touch_x_min = 220;
    touch_x_max = 3730;
    touch_y_min = 387;
    touch_y_max = 3763;
    Serial.println("TouchCal: NVS non trovato, uso valori default");
  } else {
    Serial.println("TouchCal: valori caricati da NVS");
  }

  // Init bus VSPI separato per touch (non condivide con TFT_eSPI)
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI);  // CS NON va qui, lo gestisce XPT2046_Touchscreen
  touchscreen.begin(touchSPI);
  touchscreen.setRotation(1);

  Serial.println("Touch init OK (XPT2046_Touchscreen / VSPI)");
  bleSend("Touch init OK (XPT2046_Touchscreen / VSPI)\n");
  // SD init spostata DOPO setupLVGL() Ã¢â‚¬â€ tft.begin() resetta SPI e rompe SD se init prima
  Serial.printf("  Cal: x %d-%d  y %d-%d\n",
                touch_x_min, touch_x_max, touch_y_min, touch_y_max);
}

void setupLVGL() {
  Serial.println("LVGL: lv_init...");
  bleSend("LVGL: lv_init...\n");
  lv_init();
  delay(500);        // â† aggiungi questa riga
  Serial.println("LVGL: tft.begin...");
  bleSend("LVGL: tft.begin...\n");
  tft.begin();
  Serial.println("LVGL: tft.setRotation...");
  tft.setRotation(1);
  Serial.println("LVGL: lv_disp_draw_buf_init...");
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 10);
  Serial.println("LVGL: disp_drv...");
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res  = screenWidth;
  disp_drv.ver_res  = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);
  Serial.println("LVGL: indev_drv...");
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);
  Serial.println("LVGL: done.");
}
void setupTimer() {
  morseTimer = timerBegin(1000000);
  timerAttachInterrupt(morseTimer, &onMorseTimer);
  timerAlarm(morseTimer, 1000, true, 0);   // 1 ms tick
}
// ============================================================================
// LVGL CALLBACKS
// ============================================================================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t*)&color_p->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

// [FIX-5] Touch con XPT2046_Touchscreen (Stoffregen) su VSPI
// Calibrazione: x_min=220 x_max=3730 y_min=387 y_max=3763
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  if (touchscreen.tirqTouched() && touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();
    // Mappa coordinate raw Ã¢â€ â€™ pixel schermo 320x240
    int screenX = map(p.x, touch_x_min, touch_x_max, 0, screenWidth);
    int screenY = map(p.y, touch_y_min, touch_y_max, 0, screenHeight);
    data->point.x = constrain(screenX, 0, screenWidth  - 1);
    data->point.y = constrain(screenY, 0, screenHeight - 1);
    data->state   = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// ============================================================================
// UI CREATION
// ============================================================================
void createUI() {
  tabview   = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 25);
  tabMorse1 = lv_tabview_add_tab(tabview, "Morse 1");
  tabMorse2 = lv_tabview_add_tab(tabview, "Morse 2");
  tabSD     = lv_tabview_add_tab(tabview, "SD");
  tabSettings = lv_tabview_add_tab(tabview, "Settings");
  tabSpare  = lv_tabview_add_tab(tabview, "Spare");
  lv_obj_clear_flag(tabMorse1, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(tabMorse2, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(tabMorse1, 0, 0);
  lv_obj_set_style_pad_all(tabMorse2, 0, 0);
  createTabMorse1();
  createTabMorse2();
  createTabSD();
  createTabSettings();
  createTabSpare();
}

void createCommonControls(lv_obj_t *parent,
                          lv_obj_t **lblWpm, lv_obj_t **lblVol, lv_obj_t **lblFreq,
                          lv_obj_t **outLedTx, lv_obj_t **outBtnIambic) {
  const int topY   = 5;
  const int btnSz  = 22;
  const int gap    = 3;

  // WPM
  btnWpmDown = lv_btn_create(parent);
  lv_obj_set_size(btnWpmDown, btnSz, btnSz);
  lv_obj_set_pos(btnWpmDown, 20, topY);
  lv_obj_clear_flag(btnWpmDown, LV_OBJ_FLAG_SCROLLABLE);
  lv_label_set_text(lv_label_create(btnWpmDown), "-");
  lv_obj_center(lv_obj_get_child(btnWpmDown, 0));
  lv_obj_add_event_cb(btnWpmDown, onWpmDown, LV_EVENT_CLICKED, NULL);

  btnWpmUp = lv_btn_create(parent);
  lv_obj_set_size(btnWpmUp, btnSz, btnSz);
  lv_obj_align_to(btnWpmUp, btnWpmDown, LV_ALIGN_OUT_RIGHT_MID, gap, 0);
  lv_obj_clear_flag(btnWpmUp, LV_OBJ_FLAG_SCROLLABLE);
  lv_label_set_text(lv_label_create(btnWpmUp), "+");
  lv_obj_center(lv_obj_get_child(btnWpmUp, 0));
  lv_obj_add_event_cb(btnWpmUp, onWpmUp, LV_EVENT_CLICKED, NULL);

  *lblWpm = lv_label_create(parent);
  lv_label_set_text_fmt(*lblWpm, "WPM:%d", wpm);
  lv_obj_set_style_text_font(*lblWpm, &lv_font_montserrat_12, 0);
  lv_obj_align_to(*lblWpm, btnWpmDown, LV_ALIGN_OUT_BOTTOM_MID, btnSz/2, 1);

  // VOL
  btnVolDown = lv_btn_create(parent);
  lv_obj_set_size(btnVolDown, btnSz, btnSz);
  lv_obj_set_pos(btnVolDown, 95, topY);
  lv_obj_clear_flag(btnVolDown, LV_OBJ_FLAG_SCROLLABLE);
  lv_label_set_text(lv_label_create(btnVolDown), "-");
  lv_obj_center(lv_obj_get_child(btnVolDown, 0));
  lv_obj_add_event_cb(btnVolDown, onVolDown, LV_EVENT_CLICKED, NULL);

  btnVolUp = lv_btn_create(parent);
  lv_obj_set_size(btnVolUp, btnSz, btnSz);
  lv_obj_align_to(btnVolUp, btnVolDown, LV_ALIGN_OUT_RIGHT_MID, gap, 0);
  lv_obj_clear_flag(btnVolUp, LV_OBJ_FLAG_SCROLLABLE);
  lv_label_set_text(lv_label_create(btnVolUp), "+");
  lv_obj_center(lv_obj_get_child(btnVolUp, 0));
  lv_obj_add_event_cb(btnVolUp, onVolUp, LV_EVENT_CLICKED, NULL);

  *lblVol = lv_label_create(parent);
  if (isMuted)
    lv_label_set_text(*lblVol, "VOL:M");
  else
    lv_label_set_text_fmt(*lblVol, "VOL:%d", volumeUiLevel(volumeLevel));
  lv_obj_set_style_text_font(*lblVol, &lv_font_montserrat_12, 0);
  lv_obj_align_to(*lblVol, btnVolDown, LV_ALIGN_OUT_BOTTOM_MID, btnSz/2, 1);

  // FREQ
  btnFreqDown = lv_btn_create(parent);
  lv_obj_set_size(btnFreqDown, btnSz, btnSz);
  lv_obj_set_pos(btnFreqDown, 165, topY);
  lv_obj_clear_flag(btnFreqDown, LV_OBJ_FLAG_SCROLLABLE);
  lv_label_set_text(lv_label_create(btnFreqDown), "-");
  lv_obj_center(lv_obj_get_child(btnFreqDown, 0));
  lv_obj_add_event_cb(btnFreqDown, onFreqDown, LV_EVENT_CLICKED, NULL);

  btnFreqUp = lv_btn_create(parent);
  lv_obj_set_size(btnFreqUp, btnSz, btnSz);
  lv_obj_align_to(btnFreqUp, btnFreqDown, LV_ALIGN_OUT_RIGHT_MID, gap, 0);
  lv_obj_clear_flag(btnFreqUp, LV_OBJ_FLAG_SCROLLABLE);
  lv_label_set_text(lv_label_create(btnFreqUp), "+");
  lv_obj_center(lv_obj_get_child(btnFreqUp, 0));
  lv_obj_add_event_cb(btnFreqUp, onFreqUp, LV_EVENT_CLICKED, NULL);

  *lblFreq = lv_label_create(parent);
  lv_label_set_text_fmt(*lblFreq, "FRQ:%d", audioTone);
  lv_obj_set_style_text_font(*lblFreq, &lv_font_montserrat_12, 0);
  lv_obj_align_to(*lblFreq, btnFreqDown, LV_ALIGN_OUT_BOTTOM_MID, btnSz/2, 1);

  // TX LED
  lv_obj_t *newLed = lv_obj_create(parent);
  lv_obj_set_size(newLed, 16, 16);
  lv_obj_set_pos(newLed, 228, topY + 2);
  lv_obj_set_style_radius(newLed, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(newLed, lv_color_hex(0x404040), 0);
  lv_obj_set_style_border_width(newLed, 1, 0);
  lv_obj_set_style_border_color(newLed, lv_color_hex(0x808080), 0);
  lv_obj_set_style_pad_all(newLed, 0, 0);
  lv_obj_clear_flag(newLed, LV_OBJ_FLAG_SCROLLABLE);
  *outLedTx   = newLed;
  ledTxCircle = newLed;

  labelTxLed = lv_label_create(parent);
  lv_label_set_text(labelTxLed, "TX");
  lv_obj_set_style_text_color(labelTxLed, lv_color_hex(0x000000), 0);
  lv_obj_set_style_text_font(labelTxLed, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(labelTxLed, 228, topY + 23);

  // IAMBIC MODE button
  lv_obj_t *newIambic = lv_btn_create(parent);
  lv_obj_set_size(newIambic, 60, 22);
  lv_obj_set_pos(newIambic, 252, topY);
  lv_obj_clear_flag(newIambic, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *labelIambic = lv_label_create(newIambic);
  lv_label_set_text(labelIambic, "IAMB-B");
  lv_obj_set_style_text_font(labelIambic, &lv_font_montserrat_12, 0);
  lv_obj_center(labelIambic);
  lv_obj_set_style_bg_color(newIambic, lv_color_hex(0x00AACC), 0);
  lv_obj_add_event_cb(newIambic, onIambicMode, LV_EVENT_CLICKED, NULL);
  *outBtnIambic = newIambic;
  btnIambicMode = newIambic;
  // Label "KEY"
  lv_obj_t *lblKey = lv_label_create(parent);
  lv_label_set_text(lblKey, "KEY");
  lv_obj_set_style_text_color(lblKey, lv_color_hex(0x000000), 0);
  lv_obj_set_style_text_font(lblKey, &lv_font_montserrat_12, 0);
  lv_obj_align_to(lblKey, newIambic, LV_ALIGN_OUT_BOTTOM_MID, 0, 1);
}

void createTabMorse1() {
  createCommonControls(tabMorse1, &labelWpm1, &labelVol1, &labelFreq1, &ledTxCircle1, &btnIambicMode1);

  btnCQ = lv_btn_create(tabMorse1);
  lv_obj_set_size(btnCQ, 70, 28); lv_obj_set_pos(btnCQ, 6, 52);
  lv_obj_clear_flag(btnCQ, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *lCQ = lv_label_create(btnCQ); lv_label_set_text(lCQ, "CQ");
  lv_obj_set_style_text_font(lCQ, &lv_font_montserrat_14, 0); lv_obj_center(lCQ);
  lv_obj_add_event_cb(btnCQ, onCQ, LV_EVENT_PRESSED,  NULL);
  lv_obj_add_event_cb(btnCQ, onCQ, LV_EVENT_RELEASED, NULL);

  btnName = lv_btn_create(tabMorse1);
  lv_obj_set_size(btnName, 70, 28); lv_obj_set_pos(btnName, 81, 52);
  lv_obj_clear_flag(btnName, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *lN = lv_label_create(btnName); lv_label_set_text(lN, "NAME");
  lv_obj_set_style_text_font(lN, &lv_font_montserrat_14, 0); lv_obj_center(lN);
  lv_obj_add_event_cb(btnName, onName, LV_EVENT_PRESSED,  NULL);
  lv_obj_add_event_cb(btnName, onName, LV_EVENT_RELEASED, NULL);

  btnAntRig = lv_btn_create(tabMorse1);
  lv_obj_set_size(btnAntRig, 70, 28); lv_obj_set_pos(btnAntRig, 156, 52);
  lv_obj_clear_flag(btnAntRig, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *lAR = lv_label_create(btnAntRig); lv_label_set_text(lAR, "ANT/RIG");
  lv_obj_set_style_text_font(lAR, &lv_font_montserrat_14, 0); lv_obj_center(lAR);
  lv_obj_add_event_cb(btnAntRig, onAntRig, LV_EVENT_PRESSED,  NULL);
  lv_obj_add_event_cb(btnAntRig, onAntRig, LV_EVENT_RELEASED, NULL);

  btnClear = lv_btn_create(tabMorse1);
  lv_obj_set_size(btnClear, 70, 28); lv_obj_set_pos(btnClear, 231, 52);
  lv_obj_clear_flag(btnClear, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *lCl = lv_label_create(btnClear); lv_label_set_text(lCl, "CLEAR");
  lv_obj_set_style_text_font(lCl, &lv_font_montserrat_14, 0); lv_obj_center(lCl);
  lv_obj_add_event_cb(btnClear, onClear, LV_EVENT_CLICKED, NULL);

  btnQth = lv_btn_create(tabMorse1);
  lv_obj_set_size(btnQth, 70, 28); lv_obj_set_pos(btnQth, 6, 84);
  lv_obj_clear_flag(btnQth, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *lQ = lv_label_create(btnQth); lv_label_set_text(lQ, "QTH");
  lv_obj_set_style_text_font(lQ, &lv_font_montserrat_14, 0); lv_obj_center(lQ);
  lv_obj_add_event_cb(btnQth, onQth, LV_EVENT_PRESSED,  NULL);
  lv_obj_add_event_cb(btnQth, onQth, LV_EVENT_RELEASED, NULL);

  btnRst = lv_btn_create(tabMorse1);
  lv_obj_set_size(btnRst, 70, 28); lv_obj_set_pos(btnRst, 81, 84);
  lv_obj_clear_flag(btnRst, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *lR = lv_label_create(btnRst); lv_label_set_text(lR, "RST");
  lv_obj_set_style_text_font(lR, &lv_font_montserrat_14, 0); lv_obj_center(lR);
  lv_obj_add_event_cb(btnRst, onRst, LV_EVENT_PRESSED,  NULL);
  lv_obj_add_event_cb(btnRst, onRst, LV_EVENT_RELEASED, NULL);

  btnTest = lv_btn_create(tabMorse1);
  lv_obj_set_size(btnTest, 70, 28); lv_obj_set_pos(btnTest, 156, 84);
  lv_obj_clear_flag(btnTest, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *lT = lv_label_create(btnTest); lv_label_set_text(lT, "TEST");
  lv_obj_set_style_text_font(lT, &lv_font_montserrat_14, 0); lv_obj_center(lT);
  lv_obj_add_event_cb(btnTest, onTest, LV_EVENT_PRESSED,  NULL);
  lv_obj_add_event_cb(btnTest, onTest, LV_EVENT_RELEASED, NULL);

  btnKeyerMode = lv_btn_create(tabMorse1);
  lv_obj_set_size(btnKeyerMode, 70, 28); lv_obj_set_pos(btnKeyerMode, 231, 84);
  lv_obj_clear_flag(btnKeyerMode, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *lKM = lv_label_create(btnKeyerMode); lv_label_set_text(lKM, "KEYER");
  lv_obj_set_style_text_font(lKM, &lv_font_montserrat_14, 0); lv_obj_center(lKM);
  lv_obj_set_style_bg_color(btnKeyerMode, lv_color_hex(0xCCCC00), 0);
  lv_obj_add_event_cb(btnKeyerMode, onKeyerMode, LV_EVENT_CLICKED, NULL);

  createLogAreas(tabMorse1);
}

void createTabMorse2() {
  createCommonControls(tabMorse2, &labelWpm2, &labelVol2, &labelFreq2, &ledTxCircle2, &btnIambicMode2);

  btnRatio = lv_btn_create(tabMorse2);
  lv_obj_set_size(btnRatio, 70, 28); lv_obj_set_pos(btnRatio, 6, 52);
  lv_obj_t *lRa = lv_label_create(btnRatio); lv_label_set_text(lRa, "RATIO");
  lv_obj_center(lRa); lv_obj_add_event_cb(btnRatio, onRatio, LV_EVENT_CLICKED, NULL);

  btnWeight = lv_btn_create(tabMorse2);
  lv_obj_set_size(btnWeight, 70, 28); lv_obj_set_pos(btnWeight, 81, 52);
  lv_obj_t *lWe = lv_label_create(btnWeight); lv_label_set_text(lWe, "WEIGHT");
  lv_obj_center(lWe); lv_obj_add_event_cb(btnWeight, onWeight, LV_EVENT_CLICKED, NULL);

  btnTune = lv_btn_create(tabMorse2);
  lv_obj_set_size(btnTune, 70, 28); lv_obj_set_pos(btnTune, 156, 52);
  lv_obj_t *lTu = lv_label_create(btnTune); lv_label_set_text(lTu, "TUNE");
  lv_obj_center(lTu); lv_obj_add_event_cb(btnTune, onTune, LV_EVENT_CLICKED, NULL);

  btnMacro = lv_btn_create(tabMorse2);
  lv_obj_set_size(btnMacro, 70, 28); lv_obj_set_pos(btnMacro, 231, 52);
  lv_obj_t *lMa = lv_label_create(btnMacro); lv_label_set_text(lMa, "MACRO");
  lv_obj_center(lMa); lv_obj_add_event_cb(btnMacro, onMacro, LV_EVENT_CLICKED, NULL);

  btnPadRev = lv_btn_create(tabMorse2);
  lv_obj_set_size(btnPadRev, 70, 28); lv_obj_set_pos(btnPadRev, 6, 84);
  lv_obj_t *lPR = lv_label_create(btnPadRev); lv_label_set_text(lPR, "PAD.REV");
  lv_obj_center(lPR); lv_obj_add_event_cb(btnPadRev, onPadRev, LV_EVENT_CLICKED, NULL);

  btnBeacon = lv_btn_create(tabMorse2);
  lv_obj_set_size(btnBeacon, 70, 28); lv_obj_set_pos(btnBeacon, 81, 84);
  lv_obj_t *lBe = lv_label_create(btnBeacon); lv_label_set_text(lBe, "BEACON");
  lv_obj_center(lBe);
  lv_obj_add_event_cb(btnBeacon, onBeacon, LV_EVENT_PRESSED,  NULL);
  lv_obj_add_event_cb(btnBeacon, onBeacon, LV_EVENT_RELEASED, NULL);

  btnContest = lv_btn_create(tabMorse2);
  lv_obj_set_size(btnContest, 70, 28); lv_obj_set_pos(btnContest, 156, 84);
  lv_obj_t *lCo = lv_label_create(btnContest); lv_label_set_text(lCo, "CONTEST");
  lv_obj_center(lCo);
  lv_obj_add_event_cb(btnContest, onContest, LV_EVENT_PRESSED,  NULL);
  lv_obj_add_event_cb(btnContest, onContest, LV_EVENT_RELEASED, NULL);

  btnStop = lv_btn_create(tabMorse2);
  lv_obj_set_size(btnStop, 70, 28); lv_obj_set_pos(btnStop, 231, 84);
  lv_obj_set_style_bg_color(btnStop, lv_color_hex(0xFF0000), 0);
  lv_obj_t *lSt = lv_label_create(btnStop); lv_label_set_text(lSt, "STOP");
  lv_obj_center(lSt); lv_obj_add_event_cb(btnStop, onStop, LV_EVENT_CLICKED, NULL);

  createLogAreas(tabMorse2);
}

void createLogAreas(lv_obj_t *parent) {
  bool isTab1 = (parent == tabMorse1);

  lv_obj_t *logBg = lv_obj_create(parent);
  lv_obj_set_size(logBg, 320, 50); lv_obj_set_pos(logBg, 0, 116);
  lv_obj_set_style_bg_color(logBg, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(logBg, 0, 0);
  lv_obj_set_style_pad_all(logBg, 0, 0);
  lv_obj_clear_flag(logBg, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lLog1 = lv_label_create(logBg);
  lv_label_set_text(lLog1, ""); lv_obj_set_pos(lLog1, 2, 2);
  lv_obj_set_style_text_color(lLog1, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lLog1, &lv_font_montserrat_12, 0);
  lv_label_set_long_mode(lLog1, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(lLog1, 316);
  lv_obj_set_style_text_align(lLog1, LV_TEXT_ALIGN_RIGHT, 0);

  lv_obj_t *lLog2 = lv_label_create(logBg);
  lv_label_set_text(lLog2, ""); lv_obj_set_pos(lLog2, 2, 23);
  lv_obj_set_style_text_color(lLog2, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lLog2, &lv_font_montserrat_12, 0);
  lv_label_set_long_mode(lLog2, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(lLog2, 316);
  lv_obj_set_style_text_align(lLog2, LV_TEXT_ALIGN_RIGHT, 0);

  lv_obj_t *txBg = lv_obj_create(parent);
  lv_obj_set_size(txBg, 320, 35); lv_obj_set_pos(txBg, 0, 170);
  lv_obj_set_style_bg_color(txBg, lv_color_hex(0x0000FF), 0);
  lv_obj_set_style_border_width(txBg, 0, 0);
  lv_obj_set_style_pad_all(txBg, 0, 0);
  lv_obj_clear_flag(txBg, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lTx = lv_label_create(txBg);
  lv_label_set_text(lTx, "Ready"); lv_obj_set_pos(lTx, 0, 5);
  lv_obj_set_style_text_color(lTx, lv_color_hex(0xFFFF00), 0);
  lv_obj_set_style_text_font(lTx, &lv_font_montserrat_16, 0);
  lv_label_set_long_mode(lTx, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(lTx, 316);
  lv_obj_set_style_text_align(lTx, LV_TEXT_ALIGN_RIGHT, 0);

  if (isTab1) {
    labelLog1_t1 = lLog1; labelLog2_t1 = lLog2; labelTx_t1 = lTx;
  } else {
    labelLog1_t2 = lLog1; labelLog2_t2 = lLog2; labelTx_t2 = lTx;
  }
  labelLog1 = lLog1; labelLog2 = lLog2; labelTx = lTx;
}

void createTabSD() {
  // Ã¢â€â‚¬Ã¢â€â‚¬ Sfondo scuro + rimuovi padding LVGL (default ~8px che taglia a destra) Ã¢â€â‚¬Ã¢â€â‚¬
  lv_obj_set_style_bg_color(tabSD, lv_color_hex(0x0A0A1A), 0);
  lv_obj_set_style_bg_opa(tabSD, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(tabSD, 0, 0);
  lv_obj_set_style_pad_top(tabSD, 2, 0);
  lv_obj_clear_flag(tabSD, LV_OBJ_FLAG_SCROLLABLE);

  // Area utile: 320 x 215px (tab strip = 25px)
  // Layout:
  //   Y=2   H=16  info file  (nome + lunghezza)
  //   Y=18  H=16  info audio (freq + vol)
  //   Y=36  H=2   separatore
  //   Y=40  H=68  zona testo
  //   Y=112 H=28  speed bar  (SPD-  WPM  SPD+)
  //   Y=144 H=34  bottoni    (PLAY  PAUSE  STOP)
  //   FILE button: alto destra Y=2

  // Ã¢â€â‚¬Ã¢â€â‚¬ Info file Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
  lv_obj_t *lblF = lv_label_create(tabSD);
  lv_label_set_text(lblF, "Nessun file");
  lv_obj_set_pos(lblF, 4, 2);
  lv_obj_set_style_text_font(lblF, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblF, lv_color_hex(0xFFFF00), 0);
  lv_label_set_long_mode(lblF, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(lblF, 244);
  lblSdFile = lblF;

  // Ã¢â€â‚¬Ã¢â€â‚¬ Info audio Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
  lv_obj_t *lblI = lv_label_create(tabSD);
  lv_label_set_text(lblI, "Freq:--- Vol:---");
  lv_obj_set_pos(lblI, 4, 18);
  lv_obj_set_style_text_font(lblI, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblI, lv_color_hex(0x00FFFF), 0);
  lblSdInfo = lblI;

  // Ã¢â€â‚¬Ã¢â€â‚¬ Bottone FILE (alto destra, 4px dal bordo) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
  lv_obj_t *btnFile = lv_btn_create(tabSD);
  lv_obj_set_size(btnFile, 58, 30);
  lv_obj_set_pos(btnFile, 258, 2);
  lv_obj_set_style_bg_color(btnFile, lv_color_hex(0x0055AA), 0);
  lv_obj_set_style_radius(btnFile, 4, 0);
  lv_obj_t *lbf = lv_label_create(btnFile);
  lv_label_set_text(lbf, "FILE");
  lv_obj_set_style_text_font(lbf, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbf, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(lbf);
  lv_obj_add_event_cb(btnFile, [](lv_event_t*e){ sdOpenFilePopup(); },
                      LV_EVENT_CLICKED, NULL);

  // Ã¢â€â‚¬Ã¢â€â‚¬ Separatore Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
  lv_obj_t *sep = lv_obj_create(tabSD);
  lv_obj_set_size(sep, 318, 2);
  lv_obj_set_pos(sep, 0, 36);
  lv_obj_set_style_bg_color(sep, lv_color_hex(0x334455), 0);
  lv_obj_set_style_border_width(sep, 0, 0);
  lv_obj_set_style_pad_all(sep, 0, 0);

  // Ã¢â€â‚¬Ã¢â€â‚¬ Zona testo: 5 label fisse su tabSD Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
  // Niente container, niente WRAP, niente scroll.
  // 5 righe a Y fisso, LV_LABEL_LONG_CLIP, larghezza 316px.
  // sdLoadFile calcola le righe e le scrive direttamente.
  for (int r = 0; r < 5; r++) {
    lv_obj_t *lr = lv_label_create(tabSD);
    lv_label_set_text(lr, "");
    lv_obj_set_pos(lr, 2, 42 + r * 14);
    lv_obj_set_style_text_font(lr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lr, lv_color_hex(0xDDDDDD), 0);
    lv_label_set_long_mode(lr, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lr, 316);
    lblSdRow[r] = lr;
  }
  lblSdText = lblSdRow[0]; // compatibilitÃƒÂ 

  // Speed bar
  lv_obj_t *btnSpM = lv_btn_create(tabSD);
  lv_obj_set_size(btnSpM, 72, 26);
  lv_obj_set_pos(btnSpM, 2, 112);
  lv_obj_set_style_bg_color(btnSpM, lv_color_hex(0x003388), 0);
  lv_obj_set_style_radius(btnSpM, 4, 0);
  lv_obj_t *lSpM = lv_label_create(btnSpM);
  lv_label_set_text(lSpM, "SPD -");
  lv_obj_set_style_text_font(lSpM, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lSpM, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(lSpM);
  lv_obj_add_event_cb(btnSpM, [](lv_event_t*e){
    LOCK(); if(wpm>5){ wpm--; calculateMorseTiming(); } UNLOCK();
    sdUpdateWpmLabel();
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lWpm = lv_label_create(tabSD);
  lv_obj_set_pos(lWpm, 78, 116);
  lv_obj_set_style_text_font(lWpm, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lWpm, lv_color_hex(0xFFFF00), 0);  // giallo ben visibile su scuro
  lv_obj_set_width(lWpm, 162);
  lv_obj_set_style_text_align(lWpm, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(lWpm, "20 WPM");
  lblSdWpm = lWpm;

  lv_obj_t *btnSpP = lv_btn_create(tabSD);
  lv_obj_set_size(btnSpP, 72, 26);
  lv_obj_set_pos(btnSpP, 244, 112);
  lv_obj_set_style_bg_color(btnSpP, lv_color_hex(0x003388), 0);
  lv_obj_set_style_radius(btnSpP, 4, 0);
  lv_obj_t *lSpP = lv_label_create(btnSpP);
  lv_label_set_text(lSpP, "SPD +");
  lv_obj_set_style_text_font(lSpP, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lSpP, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(lSpP);
  lv_obj_add_event_cb(btnSpP, [](lv_event_t*e){
    LOCK(); if(wpm<35){ wpm++; calculateMorseTiming(); } UNLOCK();
    sdUpdateWpmLabel();
  }, LV_EVENT_CLICKED, NULL);

  // Play / Pause / Stop
  // Tre bottoni uguali: larghezza (318-6)/3 = 104px, gap 3px
  lv_obj_t *bPlay = lv_btn_create(tabSD);
  lv_obj_set_size(bPlay, 100, 34);
  lv_obj_set_pos(bPlay, 2, 144);
  lv_obj_set_style_bg_color(bPlay, lv_color_hex(0x007700), 0);
  lv_obj_set_style_radius(bPlay, 4, 0);
  lv_obj_t *lPlay = lv_label_create(bPlay);
  lv_label_set_text(lPlay, LV_SYMBOL_PLAY " PLAY");
  lv_obj_set_style_text_font(lPlay, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lPlay, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(lPlay);
  lv_obj_add_event_cb(bPlay, [](lv_event_t*e){
    if(sdText.length()==0 || sdState==SD_PLAYING) return;
    if(sdState==SD_PAUSED){ sdState=SD_PLAYING; }
    else { sdCharIndex=0; sdState=SD_PLAYING; }
    sdUpdateButtons();
  }, LV_EVENT_CLICKED, NULL);
  btnSdPlay = bPlay;

  lv_obj_t *bPause = lv_btn_create(tabSD);
  lv_obj_set_size(bPause, 100, 34);
  lv_obj_set_pos(bPause, 109, 144);
  lv_obj_set_style_bg_color(bPause, lv_color_hex(0x664400), 0);
  lv_obj_set_style_radius(bPause, 4, 0);
  lv_obj_t *lPause = lv_label_create(bPause);
  lv_label_set_text(lPause, LV_SYMBOL_PAUSE " PAUSE");
  lv_obj_set_style_text_font(lPause, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lPause, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(lPause);
  lv_obj_add_event_cb(bPause, [](lv_event_t*e){
    if(sdState==SD_PLAYING)   { sdState=SD_PAUSED;  sdUpdateButtons(); }
    else if(sdState==SD_PAUSED){ sdState=SD_PLAYING; sdUpdateButtons(); }
  }, LV_EVENT_CLICKED, NULL);
  btnSdPause = bPause;

  lv_obj_t *bStop = lv_btn_create(tabSD);
  lv_obj_set_size(bStop, 100, 34);
  lv_obj_set_pos(bStop, 216, 144);
  lv_obj_set_style_bg_color(bStop, lv_color_hex(0x770000), 0);
  lv_obj_set_style_radius(bStop, 4, 0);
  lv_obj_t *lStop = lv_label_create(bStop);
  lv_label_set_text(lStop, LV_SYMBOL_STOP " STOP");
  lv_obj_set_style_text_font(lStop, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lStop, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(lStop);
  lv_obj_add_event_cb(bStop, [](lv_event_t*e){
    if(sdState!=SD_IDLE){
      LOCK(); buffer=""; stopRequested=true; UNLOCK();
      sdCharIndex=0; sdState=SD_IDLE;
      sdUpdateButtons();
    }
  }, LV_EVENT_CLICKED, NULL);
  btnSdStop = bStop;

  sdUpdateWpmLabel();
  sdUpdateInfoLabel();
  sdUpdateButtons();
  if(sdAvailable) sdScanFiles();
}

// ============================================================================
// SD PLAYER Ã¢â‚¬â€ funzioni
// ============================================================================

// Scansione file .txt sulla SD
// Se targetFile != "" legge anche il contenuto di quel file
void sdScanFiles(String targetFile) {
  sdFileCount = 0;
  if (!sdAvailable) return;
  File root = SD.open("/");
  if (!root) { Serial.println("SD: impossibile aprire /"); return; }
  File f = root.openNextFile();
  while (f && sdFileCount < SD_MAX_FILES) {
    if (!f.isDirectory()) {
      String rawName = f.name();
      Serial.printf("SD scan: [%s]\n", rawName.c_str());
      String nm = rawName; nm.toUpperCase();
      if (nm.endsWith(".TXT")) {
        String clean = rawName.startsWith("/") ? rawName.substring(1) : rawName;
        sdFileList[sdFileCount] = clean;
        sdFileCount++;
        // Se questo ÃƒÂ¨ il file da caricare, leggilo ora mentre ÃƒÂ¨ ancora aperto
        if (targetFile.length() > 0) {
          String targetClean = targetFile.startsWith("/") ? targetFile.substring(1) : targetFile;
          if (clean.equalsIgnoreCase(targetClean)) {
            Serial.printf("Leggo [%s] direttamente\n", rawName.c_str());
            sdText = "";
            while (f.available()) {
              char c = f.read();
              if (c >= 32 && c <= 126) sdText += (char)toupper(c);
              else if (c == '\n' || c == '\r') sdText += ' ';
            }
            Serial.printf("Letti %d chars\n", sdText.length());
          }
        }
      }
    }
    f.close();
    f = root.openNextFile();
  }
  if (f) f.close();
  root.close();
  Serial.printf("SD: trovati %d file\n", sdFileCount);
}

// Carica testo da file SD Ã¢â‚¬â€ usa sdScanFiles per leggere il file
// evita SD.open() diretto che fallisce dopo LVGL init su CYD
void sdLoadFile(String filename) {
  if (!sdAvailable) return;
  String clean = filename.startsWith("/") ? filename.substring(1) : filename;
  Serial.printf("sdLoadFile: [%s]\n", clean.c_str());

  sdText = "";  // verrÃƒÂ  riempito da sdScanFiles se trova il file
  sdScanFiles(clean);

  if (sdText.length() == 0) {
    Serial.println("FAIL: file non letto");
    for (int r = 0; r < 5; r++)
      if (lblSdRow[r]) lv_label_set_text(lblSdRow[r], r==0 ? "ERR: file vuoto o non trovato" : "");
    return;
  }

  while (sdText.indexOf("  ") >= 0) sdText.replace("  ", " ");
  sdText.trim();
  sdFileName = clean;
  sdCharIndex = 0;
  sdState = SD_IDLE;

  sdUpdateFileLabel();
  sdUpdateInfoLabel();

  // Scrivi 5 righe nella finestra testo
  const int ROWW = 50;
  int pos = 0, textLen = sdText.length();
  for (int r = 0; r < 5; r++) {
    if (!lblSdRow[r]) continue;
    if (pos >= textLen) { lv_label_set_text(lblSdRow[r], ""); continue; }
    int end2 = pos + ROWW;
    if (end2 >= textLen) end2 = textLen;
    else { int sp = sdText.lastIndexOf(' ', end2); if (sp > pos) end2 = sp; }
    String row = sdText.substring(pos, end2);
    row.trim();
    lv_label_set_text(lblSdRow[r], row.c_str());
    pos = end2;
    while (pos < textLen && sdText[pos] == ' ') pos++;
  }
  sdUpdateButtons();
  Serial.println("sdLoadFile: OK");
}



// Popup lista file
void sdOpenFilePopup() {
  if (!sdAvailable) {
    // Popup errore SD
    lv_obj_t *pop = lv_obj_create(lv_scr_act());
    lv_obj_set_size(pop, 260, 100);
    lv_obj_align(pop, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(pop, lv_color_hex(0x1A0000), 0);
    lv_obj_t *lb = lv_label_create(pop);
    lv_label_set_text(lb, "SD Card non disponibile");
    lv_obj_set_style_text_color(lb, lv_color_hex(0xFF4444), 0);
    lv_obj_center(lb);
    lv_obj_t *bc = lv_btn_create(pop);
    lv_obj_set_size(bc, 80, 28);
    lv_obj_align(bc, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_t *bl = lv_label_create(bc);
    lv_label_set_text(bl, "CLOSE");
    lv_obj_center(bl);
    lv_obj_add_event_cb(bc, [](lv_event_t*e){
      lv_obj_del(lv_obj_get_parent(lv_event_get_target(e)));
    }, LV_EVENT_CLICKED, NULL);
    sdPopup = pop;
    return;
  }
  if (sdFileCount == 0) sdScanFiles();

  // Popup fullscreen lista file
  lv_obj_t *pop = lv_obj_create(lv_scr_act());
  lv_obj_set_size(pop, 300, 210);
  lv_obj_align(pop, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(pop, lv_color_hex(0x0A0A2A), 0);
  lv_obj_set_style_bg_opa(pop, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(pop, lv_color_hex(0x0066CC), 0);
  lv_obj_set_style_border_width(pop, 2, 0);
  lv_obj_set_style_pad_all(pop, 6, 0);
  lv_obj_clear_flag(pop, LV_OBJ_FLAG_SCROLLABLE);
  sdPopup = pop;

  // Titolo
  lv_obj_t *lTitle = lv_label_create(pop);
  lv_label_set_text(lTitle, "Seleziona file SD");
  lv_obj_set_style_text_color(lTitle, lv_color_hex(0xFFFF00), 0);
  lv_obj_set_style_text_font(lTitle, &lv_font_montserrat_12, 0);
  lv_obj_align(lTitle, LV_ALIGN_TOP_MID, 0, 2);

  // Lista file con sfondo scorrevole
  lv_obj_t *listBg = lv_obj_create(pop);
  lv_obj_set_size(listBg, 284, 150);
  lv_obj_set_pos(listBg, 0, 22);
  lv_obj_set_style_bg_color(listBg, lv_color_hex(0x050510), 0);
  lv_obj_set_style_border_width(listBg, 0, 0);
  lv_obj_set_style_pad_all(listBg, 4, 0);
  // Scrollabile verticalmente se molti file
  lv_obj_set_scroll_dir(listBg, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(listBg, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(listBg, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(listBg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  if (sdFileCount == 0) {
    lv_obj_t *lNone = lv_label_create(listBg);
    lv_label_set_text(lNone, "Nessun file .txt trovato");
    lv_obj_set_style_text_color(lNone, lv_color_hex(0x888888), 0);
  } else {
    for (int i = 0; i < sdFileCount; i++) {
      lv_obj_t *btn = lv_btn_create(listBg);
      lv_obj_set_size(btn, 272, 26);
      lv_obj_set_style_bg_color(btn, lv_color_hex(0x112244), 0);
      lv_obj_set_style_bg_color(btn, lv_color_hex(0x224488), LV_STATE_PRESSED);
      lv_obj_set_style_border_width(btn, 0, 0);
      lv_obj_set_style_pad_all(btn, 2, 0);
      lv_obj_t *lbtn = lv_label_create(btn);
      lv_label_set_text(lbtn, sdFileList[i].c_str());
      lv_obj_set_style_text_font(lbtn, &lv_font_montserrat_12, 0);
      lv_obj_set_style_text_color(lbtn, lv_color_hex(0xFFFFFF), 0);
      lv_obj_align(lbtn, LV_ALIGN_LEFT_MID, 4, 0);
      // user_data = puntatore alla String nel array globale
      // lv_event_get_current_target() ritorna SEMPRE il btn registrato,
      // anche se il touch fisico finisce sulla label figlia
      lv_obj_set_user_data(btn, (void*)&sdFileList[i]);
      lv_obj_add_event_cb(btn, [](lv_event_t*e){
        lv_obj_t *b = lv_event_get_current_target(e);
        String *fn  = (String*)lv_obj_get_user_data(b);
        if (fn) {
          sdLoadFile(*fn);
          sdCloseFilePopup();
        }
      }, LV_EVENT_CLICKED, NULL);
    }
  }

  // Bottone CLOSE
  lv_obj_t *bClose = lv_btn_create(pop);
  lv_obj_set_size(bClose, 80, 26);
  lv_obj_align(bClose, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_obj_set_style_bg_color(bClose, lv_color_hex(0x770000), 0);
  lv_obj_t *lClose = lv_label_create(bClose);
  lv_label_set_text(lClose, "CLOSE");
  lv_obj_set_style_text_font(lClose, &lv_font_montserrat_12, 0);
  lv_obj_center(lClose);
  lv_obj_add_event_cb(bClose, [](lv_event_t*e){ sdCloseFilePopup(); },
                      LV_EVENT_CLICKED, NULL);
}

void sdCloseFilePopup() {
  if (sdPopup) { lv_obj_del(sdPopup); sdPopup = NULL; }
}

// Aggiorna label info file
void sdUpdateFileLabel() {
  if (!lblSdFile) return;
  if (sdFileName.length() == 0) {
    lv_label_set_text(lblSdFile, "Nessun file");
    return;
  }
  String nm = sdFileName;
  if (nm.startsWith("/")) nm = nm.substring(1);
  if ((int)nm.length() > 20) nm = nm.substring(0, 17) + "...";
  char buf[64];
  snprintf(buf, sizeof(buf), "%s  [%d car]", nm.c_str(), sdText.length());
  lv_label_set_text(lblSdFile, buf);
}

// Aggiorna label info audio (freq + vol dallo sketch principale)
void sdUpdateInfoLabel() {
  if (!lblSdInfo) return;
  // Usa volumeUiLevel() Ã¢â‚¬â€ identico a come viene mostrato in Morse1/2
  // volumeLevel=1Ã¢â€ â€™VOL4, volumeLevel=4Ã¢â€ â€™VOL1 (stessa inversione)
  char buf[40];
  if (isMuted)
    snprintf(buf, sizeof(buf), "Freq:%d Hz  Vol:M", audioTone);
  else
    snprintf(buf, sizeof(buf), "Freq:%d Hz  Vol:%d", audioTone, volumeUiLevel(volumeLevel));
  lv_label_set_text(lblSdInfo, buf);
}

// Aggiorna label WPM
void sdUpdateWpmLabel() {
  if (!lblSdWpm) return;
  char buf[16];
  LOCK(); int w = wpm; UNLOCK();
  snprintf(buf, sizeof(buf), "%d WPM", w);
  lv_label_set_text(lblSdWpm, buf);
}

// Aggiorna colori/stato bottoni
void sdUpdateButtons() {
  if (!btnSdPlay) return;
  // PLAY: verde se idle/paused e testo presente, grigio se playing o no testo
  bool canPlay = (sdText.length() > 0) && (sdState != SD_PLAYING);
  lv_obj_set_style_bg_color(btnSdPlay,
    canPlay ? lv_color_hex(0x007700) : lv_color_hex(0x333333), 0);

  // PAUSE: giallo se playing, grigio altrimenti
  lv_obj_set_style_bg_color(btnSdPause,
    (sdState == SD_PLAYING) ? lv_color_hex(0x886600) :
    (sdState == SD_PAUSED)  ? lv_color_hex(0xAA4400) : lv_color_hex(0x333333), 0);

  // STOP: rosso se playing/paused, grigio se idle
  lv_obj_set_style_bg_color(btnSdStop,
    (sdState != SD_IDLE) ? lv_color_hex(0x770000) : lv_color_hex(0x333333), 0);
}

// Ã¢â€â‚¬Ã¢â€â‚¬ SD Player task Ã¢â‚¬â€ chiamato da loop() Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
// Inietta caratteri nel buffer Morse uno alla volta solo quando il buffer
// ÃƒÂ¨ vuoto (per non sovrapporre ad altri messaggi).
// La trasmissione avviene tramite morseTask (Core 0) esattamente come i tasti CQ.
void sdPlayerTask() {
  if (sdState != SD_PLAYING) return;
  if (sdText.length() == 0) return;

  // Invia solo se buffer morse completamente vuoto
  LOCK();
  bool busy     = (buffer.length() > 0);
  bool bcon     = beaconActive;
  bool tuneOn   = isTuneMode;
  UNLOCK();
  if (busy || bcon || tuneOn) return;

  if (sdCharIndex >= (int)sdText.length()) {
    sdState = SD_IDLE;
    sdCharIndex = 0;
    sdUpdateButtons();
    Serial.println("SD Player: fine testo");
    return;
  }

  // Invia UN carattere Ã¢â‚¬â€ morseTask lo preleva e lo trasmette
  // esattamente come i tasti CQ/NAME ecc.
  char c = sdText.charAt(sdCharIndex);
  LOCK();
  buffer    += c;
  isBufferMode = true;   // attiva modalitÃƒÂ  buffer come i tasti messaggio
  UNLOCK();
  sdCharIndex++;
}


// ============================================================================
// FARNSWORTH UI - createTabSettings modificato + popup + event handlers
// ============================================================================

void createTabSettings() {
  // Header compatto
  lv_obj_t *lT = lv_label_create(tabSettings);
  lv_label_set_text(lT, "VK2IDL CW ENCODER"); 
  lv_obj_set_pos(lT, 10, 5);
  lv_obj_set_style_text_font(lT, &lv_font_montserrat_12, 0);

  // Pulsante QRSS (in alto a destra, allineato con titolo)
  btnQrss = lv_btn_create(tabSettings);
  lv_obj_set_size(btnQrss, 140, 32); 
  lv_obj_set_pos(btnQrss, 160, 5);  // X=160, Y=5
  lv_obj_set_style_bg_color(btnQrss, lv_color_hex(0x555555), 0); // grigio = OFF
  lv_obj_t *lQrss = lv_label_create(btnQrss); 
  lv_label_set_text(lQrss, "QRSS: OFF");
  lv_obj_set_style_text_font(lQrss, &lv_font_montserrat_12, 0);
  lv_obj_center(lQrss); 
  lv_obj_add_event_cb(btnQrss, onQrssToggle, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lV = lv_label_create(tabSettings);
  lv_label_set_text(lV, ("FW:" + VERSION + " - IW1RMM").c_str()); 
  lv_obj_set_pos(lV, 10, 22);
  lv_obj_set_style_text_font(lV, &lv_font_montserrat_10, 0);

  // Pulsante K3NG / Winkey toggle
  btnProtocol = lv_btn_create(tabSettings);
  lv_obj_set_size(btnProtocol, 140, 32); 
  lv_obj_set_pos(btnProtocol, 10, 50);
  lv_obj_set_style_bg_color(btnProtocol, lv_color_hex(0x00AA00), 0); // verde = K3NG
  lv_obj_t *lProt = lv_label_create(btnProtocol); 
  lv_label_set_text(lProt, "Mode: K3NG");
  lv_obj_set_style_text_font(lProt, &lv_font_montserrat_12, 0);
  lv_obj_center(lProt); 
  lv_obj_add_event_cb(btnProtocol, onProtocolToggle, LV_EVENT_CLICKED, NULL);

  // Pulsante SD Logging (affianco a Farnsworth, sotto Protocol)
  btnSdLog = lv_btn_create(tabSettings);
  lv_obj_set_size(btnSdLog, 140, 32); 
  lv_obj_set_pos(btnSdLog, 160, 90);  // stessa Y di Farnsworth
  lv_obj_set_style_bg_color(btnSdLog, lv_color_hex(0x555555), 0); // grigio = OFF
  lv_obj_t *lLog = lv_label_create(btnSdLog); 
  lv_label_set_text(lLog, "SD LOG: OFF");
  lv_obj_set_style_text_font(lLog, &lv_font_montserrat_12, 0);
  lv_obj_center(lLog); 
  lv_obj_add_event_cb(btnSdLog, onSdLogToggle, LV_EVENT_CLICKED, NULL);

  // Pulsante Calibra Touch (spostato sotto)
  lv_obj_t *btnCal = lv_btn_create(tabSettings);
  lv_obj_set_size(btnCal, 140, 32); 
  lv_obj_set_pos(btnCal, 160, 50);  // Y=50
  lv_obj_t *lCal = lv_label_create(btnCal); 
  lv_label_set_text(lCal, "Calibra Touch");
  lv_obj_set_style_text_font(lCal, &lv_font_montserrat_12, 0);
  lv_obj_center(lCal); 
  lv_obj_add_event_cb(btnCal, onRecalibrate, LV_EVENT_CLICKED, NULL);

  // Pulsante Farnsworth (sotto btnProtocol)
  btnFarnsworth = lv_btn_create(tabSettings);
  lv_obj_set_size(btnFarnsworth, 140, 32); 
  lv_obj_set_pos(btnFarnsworth, 10, 90);
  lv_obj_set_style_bg_color(btnFarnsworth, lv_color_hex(0x555555), 0); // grigio = OFF
  lv_obj_t *lFarn = lv_label_create(btnFarnsworth); 
  lv_label_set_text(lFarn, "FARNSW: OFF");
  lv_obj_set_style_text_font(lFarn, &lv_font_montserrat_12, 0);
  lv_obj_center(lFarn); 
  lv_obj_add_event_cb(btnFarnsworth, onFarnsworthToggle, LV_EVENT_CLICKED, NULL);

  // --- OROLOGIO LVGL ---
  ui_LabelClock = lv_label_create(tabSettings); 
  lv_obj_set_size(ui_LabelClock, 140, 32);      // Stessa dimensione di btnBle
  lv_obj_set_pos(ui_LabelClock, 10, 130);       // X=10 (sinistra), Y=130 (allineato a BT)
  lv_obj_set_style_bg_color(ui_LabelClock, lv_color_hex(0x000022), 0);
  lv_obj_set_style_bg_opa(ui_LabelClock, 255, 0);
  lv_obj_set_style_border_width(ui_LabelClock, 2, 0);
  lv_obj_set_style_border_color(ui_LabelClock, lv_color_hex(0xFF0000), 0); // Rosso = No Sincro
  lv_obj_set_style_radius(ui_LabelClock, 4, 0);
  lv_obj_set_style_text_align(ui_LabelClock, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(ui_LabelClock, "Sincro...");
  lv_obj_set_style_text_font(ui_LabelClock, &lv_font_montserrat_12, 0);

  // --- INFO PROTOCOLLI (TUTTO COMMENTATO) ---
  // lv_obj_t *lInfo = lv_label_create(tabSettings);
  // lv_label_set_text(lInfo, "...");
  // Queste righe sotto davano errore perché lInfo non esiste più:
  // lv_obj_set_pos(lInfo, 10, 135);                          // <--- COMMENTATA
  // lv_obj_set_style_text_font(lInfo, &lv_font_montserrat_10, 0); // <--- COMMENTATA

  // --- PULSANTE BLE ---
  btnBle = lv_btn_create(tabSettings);
  lv_obj_set_size(btnBle, 140, 32); 
  lv_obj_set_pos(btnBle, 160, 130);  // Allineato orizzontalmente all'orologio
  lv_obj_set_style_bg_color(btnBle, lv_color_hex(0x555555), 0); 
  lv_obj_t *lBle = lv_label_create(btnBle); 
  lv_label_set_text(lBle, "BT: OFF");
  lv_obj_set_style_text_font(lBle, &lv_font_montserrat_12, 0);
  lv_obj_center(lBle); 
  lv_obj_add_event_cb(btnBle, onBleToggle, LV_EVENT_CLICKED, NULL);

} // Fine funzione


 



// Event handler Farnsworth toggle
void onFarnsworthToggle(lv_event_t *e) {
  if (!btnFarnsworth) return;
  
  // Se OFF -> apri popup per impostare WPM
  if (!farnsworthEnabled) {
    showPopupFarnsworth();
  } else {
    // Se ON -> disabilita
    LOCK();
    farnsworthEnabled = false;
    calculateMorseTiming();
    UNLOCK();
    
    lv_obj_t *label = lv_obj_get_child(btnFarnsworth, 0);
    lv_label_set_text(label, "FARNSW: OFF");
    lv_obj_set_style_bg_color(btnFarnsworth, lv_color_hex(0x555555), 0); // grigio
    Serial.println("Farnsworth DISABLED");
  }
}

void showPopupFarnsworth() {
    LOCK(); int currentWPM = wpm; UNLOCK();
    tempFarnsworthWPM = constrain(currentWPM - 8, 5, currentWPM - 1);

    // Creazione popup
    lv_obj_t *box = createPopupBase("Farnsworth WPM");

    // Titolo subito sotto il bordo superiore
    lv_obj_t *titleLabel = lv_obj_get_child(box, 0);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 0);

    // --- Linee centrali gialle ---
    char buf1[32], buf2[32];
    snprintf(buf1, sizeof(buf1), "Effective: %d WPM", tempFarnsworthWPM);
    snprintf(buf2, sizeof(buf2), "Chars: %d WPM", currentWPM);

    // Linea 1 - USA VARIABILE GLOBALE popupFarnsworthLabel1
    popupFarnsworthLabel1 = lv_label_create(box);
    lv_label_set_text(popupFarnsworthLabel1, buf1);
    lv_obj_set_style_text_font(popupFarnsworthLabel1, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(popupFarnsworthLabel1, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(popupFarnsworthLabel1, LV_ALIGN_CENTER, 0, -35);

    // Linea 2 - USA VARIABILE GLOBALE popupFarnsworthLabel2
    popupFarnsworthLabel2 = lv_label_create(box);
    lv_label_set_text(popupFarnsworthLabel2, buf2);
    lv_obj_set_style_text_font(popupFarnsworthLabel2, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(popupFarnsworthLabel2, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(popupFarnsworthLabel2, LV_ALIGN_CENTER, 0, -20);

    // --- Bottoni - / + ---
    lv_obj_t *btnMinus = lv_btn_create(box); 
    lv_obj_set_size(btnMinus, 36, 36);
    lv_obj_align(btnMinus, LV_ALIGN_LEFT_MID, 8, 0);
    lv_label_set_text(lv_label_create(btnMinus), "-");
    lv_obj_center(lv_obj_get_child(btnMinus, 0));
    lv_obj_add_event_cb(btnMinus, onFarnsworthMinus, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btnPlus = lv_btn_create(box); 
    lv_obj_set_size(btnPlus, 36, 36);
    lv_obj_align(btnPlus, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_label_set_text(lv_label_create(btnPlus), "+");
    lv_obj_center(lv_obj_get_child(btnPlus, 0));
    lv_obj_add_event_cb(btnPlus, onFarnsworthPlus, LV_EVENT_CLICKED, NULL);

    // --- Bottoni Save / Cancel ---
    lv_obj_t *btnSave = lv_btn_create(box); 
    lv_obj_set_size(btnSave, 80, 28);
    lv_obj_align(btnSave, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_obj_set_style_bg_color(btnSave, lv_color_hex(0x00AA00), 0);
    lv_label_set_text(lv_label_create(btnSave), "SAVE");
    lv_obj_center(lv_obj_get_child(btnSave, 0));
    lv_obj_add_event_cb(btnSave, onFarnsworthSave, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btnCancel = lv_btn_create(box); 
    lv_obj_set_size(btnCancel, 80, 28);
    lv_obj_align(btnCancel, LV_ALIGN_BOTTOM_RIGHT, -8, -4);
    lv_obj_set_style_bg_color(btnCancel, lv_color_hex(0xAA0000), 0);
    lv_label_set_text(lv_label_create(btnCancel), "CANCEL");
    lv_obj_center(lv_obj_get_child(btnCancel, 0));
    lv_obj_add_event_cb(btnCancel, onPopupCancel, LV_EVENT_CLICKED, NULL);

    // --- Nota "Typical range ..." sotto il fondo del box ---
    lv_obj_t *note = lv_label_create(box);
    lv_label_set_text(note, "(Typical range: 5 - Effective WPM)");
    lv_obj_set_style_text_color(note, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_12, 0);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, 15);
}

void onFarnsworthMinus(lv_event_t *e) {
    LOCK(); int currentWPM = wpm; UNLOCK();
    tempFarnsworthWPM = constrain(tempFarnsworthWPM - 1, 5, currentWPM - 1);
    
    char buf1[32], buf2[32];
    snprintf(buf1, sizeof(buf1), "Effective: %d WPM", tempFarnsworthWPM);
    snprintf(buf2, sizeof(buf2), "Chars: %d WPM", currentWPM);
    
    if (popupFarnsworthLabel1) lv_label_set_text(popupFarnsworthLabel1, buf1);
    if (popupFarnsworthLabel2) lv_label_set_text(popupFarnsworthLabel2, buf2);
}

void onFarnsworthPlus(lv_event_t *e) {
    LOCK(); int currentWPM = wpm; UNLOCK();
    tempFarnsworthWPM = constrain(tempFarnsworthWPM + 1, 5, currentWPM - 1);
    
    char buf1[32], buf2[32];
    snprintf(buf1, sizeof(buf1), "Effective: %d WPM", tempFarnsworthWPM);
    snprintf(buf2, sizeof(buf2), "Chars: %d WPM", currentWPM);
    
    if (popupFarnsworthLabel1) lv_label_set_text(popupFarnsworthLabel1, buf1);
    if (popupFarnsworthLabel2) lv_label_set_text(popupFarnsworthLabel2, buf2);
}

void onFarnsworthSave(lv_event_t *e) {
  LOCK();
  farnsworthWPM = tempFarnsworthWPM;
  farnsworthEnabled = true;
  calculateMorseTiming();
  UNLOCK();
  
  closePopup();
  
  if (btnFarnsworth) {
    lv_obj_t *label = lv_obj_get_child(btnFarnsworth, 0);
    char buf[32];
    sprintf(buf, "FARNSW: %d", farnsworthWPM);
    lv_label_set_text(label, buf);
    lv_obj_set_style_bg_color(btnFarnsworth, lv_color_hex(0x00AA00), 0); // verde = ON
  }
  
  Serial.printf("Farnsworth ENABLED: effective=%d WPM, chars=%d WPM\n", farnsworthWPM, wpm);
}
// ============================================================================
// SD LOGGING - Implementazione
// ============================================================================

// Inizializza SD logging - crea file con nome basato su uptime
void sdLogInit() {
  if (!SD.begin(SD_CS, SPI)) {
    Serial.println("SD Card: init failed, logging disabled");
    sdLoggingEnabled = false;
    if (btnSdLog) {
      lv_obj_t *label = lv_obj_get_child(btnSdLog, 0);
      lv_label_set_text(label, "SD LOG: ERR");
      lv_obj_set_style_bg_color(btnSdLog, lv_color_hex(0xAA0000), 0); // rosso
    }
    return;
  }

  // Calcola "data fittizia" basata su uptime (giorni dall'accensione)
  unsigned long uptimeDays = millis() / 86400000UL;  // giorni
  unsigned long baseDate = 20260220;  // Data base fittizia: 2026-02-20
  
  // Incrementa data base di uptimeDays (semplificato - ignora mesi/anni)
  int day = (baseDate % 100) + uptimeDays;
  int month = (baseDate / 100) % 100;
  int year = baseDate / 10000;
  
  // Aggiusta overflow giorni (semplificato: tutti i mesi hanno 30 giorni)
  while (day > 30) {
    day -= 30;
    month++;
    if (month > 12) {
      month = 1;
      year++;
    }
  }
  // Crea nome file: log_YYYYMMDD.txt
  char fname[32];
  sprintf(fname, "/log_%04d%02d%02d.txt", year, month, day);
  logFileName = String(fname);
  
  // Apri file in append mode
  logFile = SD.open(logFileName.c_str(), FILE_APPEND);
  if (!logFile) {
    Serial.printf("SD Card: cannot create %s, logging disabled\n", fname);
    sdLoggingEnabled = false;
    if (btnSdLog) {
      lv_obj_t *label = lv_obj_get_child(btnSdLog, 0);
      lv_label_set_text(label, "SD LOG: ERR");
      lv_obj_set_style_bg_color(btnSdLog, lv_color_hex(0xAA0000), 0);
    }
    return;
  }
  
  // Scrivi header se file nuovo
  if (logFile.size() == 0) {
    logFile.println("=== VK2IDL Morse Encoder Log ===");
    logFile.printf("Session started: uptime %s\n", getUptimeString().c_str());
    logFile.println();
  } else {
    // File esistente - aggiungi separatore sessione
    logFile.println();
    logFile.println("--- New session ---");
    logFile.printf("Started: uptime %s\n", getUptimeString().c_str());
    logFile.println();
  }
  logFile.flush();
  
  Serial.printf("SD Logging enabled: %s\n", fname);
}

// Scrivi riga nel log (chiamata da vari punti)
void sdLogWrite(String msg) {
  // Se la SD non è pronta, usciamo subito
  if (!sdAvailable) return;

  File logFile = SD.open("/morse_log.txt", FILE_APPEND);
  if (logFile) {
    time_t ora = time(NULL);
    struct tm * t = localtime(&ora);
    
    char tBuf[25];
    // Formato: [02/03/26 20:15:00]
    strftime(tBuf, sizeof(tBuf), "[%d/%m/%y %H:%M:%S] ", t);
    
    logFile.print(tBuf);
    logFile.println(msg);
    logFile.close();
  }
}
// Calcola uptime formattato HH:MM:SS
String getUptimeString() {
  unsigned long uptimeMs = millis() - systemStartTime;
  unsigned long seconds = uptimeMs / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  
  seconds %= 60;
  minutes %= 60;
  hours %= 24;  // wrap a 24h
  
  char buf[16];
  sprintf(buf, "%02lu:%02lu:%02lu", hours, minutes, seconds);
  return String(buf);
}

// Event handler toggle SD Logging
void onSdLogToggle(lv_event_t *e) {
  if (!btnSdLog) return;
  
  sdLoggingEnabled = !sdLoggingEnabled;
  
  lv_obj_t *label = lv_obj_get_child(btnSdLog, 0);
  
  if (sdLoggingEnabled) {
    // Abilita logging
    sdLogInit();
    
    // Aggiorna UI solo se init è andato a buon fine
    if (sdLoggingEnabled) {
      lv_label_set_text(label, "SD LOG: ON");
      lv_obj_set_style_bg_color(btnSdLog, lv_color_hex(0x00AA00), 0); // verde
    }
  } else {
    // Disabilita logging
    if (logFile) {
      logFile.println();
      logFile.println("--- Session ended ---");
      logFile.println();
      logFile.close();
    }
    lv_label_set_text(label, "SD LOG: OFF");
    lv_obj_set_style_bg_color(btnSdLog, lv_color_hex(0x555555), 0); // grigio
    Serial.println("SD Logging disabled");
  }
}
// ============================================================================
// HSCW POPUP SLIDER - Implementazione
// ============================================================================

// Apre popup con slider 60-100 (snap ogni 5 WPM)
void showPopupHscw(int currentWPM) {
  // Inizializza valore temporaneo
  tempHscwWPM = currentWPM;
  if (tempHscwWPM < 60) tempHscwWPM = 60;
  if (tempHscwWPM > 100) tempHscwWPM = 100;
  
  // Arrotonda a multiplo di 5
  tempHscwWPM = (tempHscwWPM / 5) * 5;
  
  // Crea popup base (dimensioni ridotte)
  popupBox = lv_obj_create(lv_scr_act());
  lv_obj_set_size(popupBox, 280, 160);  // ← Ridotto da 180 a 160
  lv_obj_center(popupBox);
  lv_obj_set_style_bg_color(popupBox, lv_color_hex(0x222222), 0);
  lv_obj_set_style_border_color(popupBox, lv_color_hex(0x00AAFF), 0);
  lv_obj_set_style_border_width(popupBox, 2, 0);
  
  // Titolo "HSCW Mode"
  lv_obj_t *titleLabel = lv_label_create(popupBox);
  lv_label_set_text(titleLabel, "HSCW Mode");
  lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(titleLabel, lv_color_hex(0x00AAFF), 0);
  lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 5);
  
  // Label valore WPM (PIÙ IN ALTO per evitare sovrapposizione slider)
  popupHscwLabel = lv_label_create(popupBox);
  char buf[32];
  sprintf(buf, "WPM: %d", tempHscwWPM);
  lv_label_set_text(popupHscwLabel, buf);
  lv_obj_set_style_text_font(popupHscwLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(popupHscwLabel, lv_color_hex(0xFFFF00), 0);
  lv_obj_align(popupHscwLabel, LV_ALIGN_TOP_MID, 0, 28);  // ← Y=28 (era CENTER -10)
  
  // Slider 60-100 (posizione centrale)
  popupHscwSlider = lv_slider_create(popupBox);
  lv_obj_set_size(popupHscwSlider, 240, 15);  // ← Altezza 15 invece di 20
  lv_obj_align(popupHscwSlider, LV_ALIGN_CENTER, 0, 5);  // ← Y=5 (center + offset piccolo)
  lv_slider_set_range(popupHscwSlider, 60, 100);
  lv_slider_set_value(popupHscwSlider, tempHscwWPM, LV_ANIM_OFF);
  lv_obj_add_event_cb(popupHscwSlider, onHscwSliderChange, LV_EVENT_VALUE_CHANGED, NULL);
  
  // Label range sotto slider (FONT PIÙ GRANDE)
  lv_obj_t *rangeLabel = lv_label_create(popupBox);
  lv_label_set_text(rangeLabel, "60        80        100");
  lv_obj_set_style_text_font(rangeLabel, &lv_font_montserrat_12, 0);  // ← Era 10, ora 12
  lv_obj_set_style_text_color(rangeLabel, lv_color_hex(0xAAAAAA), 0);
  lv_obj_align(rangeLabel, LV_ALIGN_CENTER, 0, 25);  // ← Sotto slider
  
  // Info text (più in basso)
  lv_obj_t *infoLabel = lv_label_create(popupBox);
  lv_label_set_text(infoLabel, "High Speed CW");
  lv_obj_set_style_text_font(infoLabel, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(infoLabel, lv_color_hex(0x888888), 0);
  lv_obj_align(infoLabel, LV_ALIGN_CENTER, 0, 42);
  
  // Pulsante SAVE (in basso a sinistra)
  lv_obj_t *btnSave = lv_btn_create(popupBox);
  lv_obj_set_size(btnSave, 80, 28);  // ← Altezza 28 invece di 30
  lv_obj_align(btnSave, LV_ALIGN_BOTTOM_LEFT, 20, -8);
  lv_obj_set_style_bg_color(btnSave, lv_color_hex(0x00AA00), 0);
  lv_obj_t *lSave = lv_label_create(btnSave);
  lv_label_set_text(lSave, "SAVE");
  lv_obj_center(lSave);
  lv_obj_add_event_cb(btnSave, onHscwSave, LV_EVENT_CLICKED, NULL);
  
  // Pulsante CANCEL (in basso a destra)
  lv_obj_t *btnCancel = lv_btn_create(popupBox);
  lv_obj_set_size(btnCancel, 80, 28);
  lv_obj_align(btnCancel, LV_ALIGN_BOTTOM_RIGHT, -20, -8);
  lv_obj_set_style_bg_color(btnCancel, lv_color_hex(0xAA0000), 0);
  lv_obj_t *lCancel = lv_label_create(btnCancel);
  lv_label_set_text(lCancel, "CANCEL");
  lv_obj_center(lCancel);
  lv_obj_add_event_cb(btnCancel, onHscwCancel, LV_EVENT_CLICKED, NULL);
}

// Callback slider: snap a multipli di 5
void onHscwSliderChange(lv_event_t *e) {
  if (!popupHscwSlider || !popupHscwLabel) return;
  
  int rawValue = lv_slider_get_value(popupHscwSlider);
  
  // Snap a multiplo di 5
  int snappedValue = ((rawValue + 2) / 5) * 5;  // +2 per arrotondamento corretto
  snappedValue = constrain(snappedValue, 60, 100);
  
  // Aggiorna solo se cambiato
  if (snappedValue != tempHscwWPM) {
    tempHscwWPM = snappedValue;
    lv_slider_set_value(popupHscwSlider, snappedValue, LV_ANIM_OFF);
    
    char buf[32];
    sprintf(buf, "WPM: %d", snappedValue);
    lv_label_set_text(popupHscwLabel, buf);
  }
}

// Salva valore HSCW
void onHscwSave(lv_event_t *e) {
  LOCK();
  wpm = tempHscwWPM;
  calculateMorseTiming();
  UNLOCK();
  
  // *** FIX CRITICO: resetta riferimenti PRIMA di closePopup ***
  popupHscwSlider = NULL;
  popupHscwLabel = NULL;
  
  closePopup();  // ← Distrugge popupBox
  
  postDisplay(DISP_WPM, tempHscwWPM);
  savePreferences();
  
  // Log e avviso
  Serial.printf("HSCW mode: %d WPM (dit=%.1f ms)\n", tempHscwWPM, 1200.0f / tempHscwWPM);
  sdLogWrite(String("WPM: ") + tempHscwWPM + " (HSCW)");
}

// Annulla cambio HSCW
void onHscwCancel(lv_event_t *e) {
  // *** FIX CRITICO: resetta riferimenti PRIMA di closePopup ***
  popupHscwSlider = NULL;
  popupHscwLabel = NULL;
  
  closePopup();  // ← Distrugge popupBox
  
  // Mantieni WPM corrente
  LOCK(); int currentWPM = wpm; UNLOCK();
  Serial.printf("HSCW popup cancelled, keeping WPM=%d\n", currentWPM);
}

// ============================================================================
// QRSS (Slow Speed CW) - Implementazione
// ============================================================================

// Restituisce stringa descrittiva modalità QRSS
const char* getQrssModeString(QrssMode mode) {
  switch(mode) {
    case QRSS_OFF: return "QRSS: OFF";
    case QRSS_3:   return "QRSS3";
    case QRSS_6:   return "QRSS6";
    case QRSS_10:  return "QRSS10";
    case QRSS_30:  return "QRSS30";
    default:       return "QRSS: OFF";
  }
}

// Aggiorna aspetto pulsante QRSS
void updateQrssButton() {
  if (!btnQrss) return;
  
  lv_obj_t *label = lv_obj_get_child(btnQrss, 0);
  lv_label_set_text(label, getQrssModeString(qrssMode));
  
  if (qrssMode == QRSS_OFF) {
    lv_obj_set_style_bg_color(btnQrss, lv_color_hex(0x555555), 0); // grigio
  } else {
    lv_obj_set_style_bg_color(btnQrss, lv_color_hex(0xFF6600), 0); // arancione
  }
}

// Toggle ciclico QRSS: OFF → 3 → 6 → 10 → 30 → OFF
void onQrssToggle(lv_event_t *e) {
  // Cicla modalità
  switch(qrssMode) {
    case QRSS_OFF: qrssMode = QRSS_3;  break;
    case QRSS_3:   qrssMode = QRSS_6;  break;
    case QRSS_6:   qrssMode = QRSS_10; break;
    case QRSS_10:  qrssMode = QRSS_30; break;
    case QRSS_30:  qrssMode = QRSS_OFF; break;
    default:       qrssMode = QRSS_OFF; break;
  }
  
  // Se attivi QRSS, disabilita HSCW (mutuamente esclusivi)
  if (qrssMode != QRSS_OFF) {
    LOCK();
    // Forza WPM a valore normale se era in HSCW (>60)
    if (wpm > 60) {
      wpm = 20;  // Default sicuro
      postDisplay(DISP_WPM, wpm);
    }
    
    // Calcola timing QRSS
    qrssDitMs = (float)qrssMode * 1000.0f;  // Converti secondi in millisecondi
    calculateMorseTiming();
    UNLOCK();
    
    Serial.printf("QRSS%d enabled: dit=%.0f ms (%.3f WPM)\n", 
                  (int)qrssMode, qrssDitMs, 1200.0f / qrssDitMs);
    sdLogWrite(String("QRSS") + (int)qrssMode + " enabled");
  } else {
    // QRSS disabilitato → torna a timing normale
    LOCK();
    calculateMorseTiming();
    UNLOCK();
    
    Serial.println("QRSS disabled, normal timing restored");
    sdLogWrite("QRSS disabled");
  }
  
  // Aggiorna UI
  updateQrssButton();
}

// ============================================================================
// BLE TASK 
// ============================================================================

static volatile bool bleSetupDone = false;

void bleSetupTask(void *param) {
  BLEDevice::init("VK2IDL_Morse");
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new MyServerCallbacks());

  BLEService *service = bleServer->createService(BLE_SERVICE_UUID);

  bleTxCharacteristic = service->createCharacteristic(
      BLE_CHARACTERISTIC_UUID_TX,
      BLECharacteristic::PROPERTY_NOTIFY);
  bleTxCharacteristic->addDescriptor(new BLE2902());

  bleRxCharacteristic = service->createCharacteristic(
      BLE_CHARACTERISTIC_UUID_RX,
      BLECharacteristic::PROPERTY_WRITE);
  bleRxCharacteristic->setCallbacks(new MyCallbacks());

  service->start();
  Serial.println("BLE: Stack ready");
  bleSetupDone = true;
  vTaskDelete(NULL);
}

void bleSetup() {
  xTaskCreatePinnedToCore(bleSetupTask, "BLESetup", 8192, NULL, 1, NULL, 1);
}
void bleStop() {
  if (!bleEnabled) return;
  BLEDevice::stopAdvertising();
  bleEnabled = false;
  bleClientConnected = false;
  bleUiUpdatePending = true;
  bleLogDisabledPending = true;
  Serial.println("BLE: Stopped");
}

void updateBleButton() {
  if (!btnBle) return;
  lv_obj_t *label = lv_obj_get_child(btnBle, 0);
  if (!label) return;

  if (!bleEnabled) {
    lv_label_set_text(label, "BT: OFF");
    lv_obj_set_style_bg_color(btnBle, lv_color_hex(0x555555), 0);
  } else if (bleClientConnected) {
    lv_label_set_text(label, "BT: ON");
    lv_obj_set_style_bg_color(btnBle, lv_color_hex(0x00AA00), 0);  // verde
  } else {
    lv_label_set_text(label, "BT: WAIT");
    lv_obj_set_style_bg_color(btnBle, lv_color_hex(0x6666FF), 0);
  }
  lv_obj_invalidate(btnBle);
}

void bleInit() {
  if (bleEnabled) return;
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  
  // Imposta nome nel payload advertising principale (non solo scan response)
  BLEAdvertisementData advData;
  advData.setName("VK2IDL_Morse");// ← nome nel pacchetto primario
  advData.setFlags(0x06);// LE General Discoverable + BR/EDR not supported
  adv->setAdvertisementData(advData);
  
  // Scan response con il service UUID
  BLEAdvertisementData scanData;
  scanData.setCompleteServices(BLEUUID(BLE_SERVICE_UUID));
  adv->setScanResponseData(scanData);
  
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  bleEnabled = true;
  bleUiUpdatePending = true;
  bleLogEnabledPending = true;
  Serial.println("BLE: Advertising started");
}
void onBleToggle(lv_event_t *e) {
  if (bleEnabled) {
    bleStop();
    updateBleButton();
  } else {
    if (!bleSetupDone) {
      // Prima volta: reset come Calibra
      Serial.println("BLE: restarting for clean init...");
      delay(500);
      ESP.restart();
    }
    bleInit();
    updateBleButton();
  }
}
/*
void onBleToggle(lv_event_t *e) {
  if (bleEnabled) {
    bleStop();
  } else {
    if (!bleSetupDone) return;  // stack non ancora pronto
    bleInit();
  }
  updateBleButton();
}
*/
void bleSend(const String &data) {
  if (!bleEnabled || !bleClientConnected || !bleTxCharacteristic) return;
  bleTxCharacteristic->setValue(data.c_str());
  bleTxCharacteristic->notify();
}
void bleSendLong(const String &data) {
  for (int i = 0; i < (int)data.length(); i += 20) {
    bleSend(data.substring(i, min(i + 20, (int)data.length())));
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void processBleCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd.charAt(0) == '\\' && cmd.length() >= 2) {
    char cmdChar = toupper(cmd.charAt(1));
    String param = cmd.length() > 2 ? cmd.substring(2) : "";
    param.trim();
    param.toUpperCase();

    switch(cmdChar) {

      case 'S': {
        if (param.length() > 0) {
          int v = constrain(param.toInt(), MIN_WPM, MAX_WPM);
          LOCK(); wpm=v; calculateMorseTiming(); UNLOCK();
          postDisplay(DISP_WPM, v);
          savePreferences();
          bleSend("WPM: " + String(v) + "\n");
        } else {
          LOCK(); int v=wpm; UNLOCK();
          bleSend("WPM: " + String(wpm) + "\n");
        }
        break;
      }

      case 'F': {
        if (param.length() > 0) {
          int v = constrain(param.toInt(), MIN_FREQ, MAX_FREQ);
          LOCK(); audioTone=v; UNLOCK();
          postDisplay(DISP_FREQ, v);
          savePreferences();
          bleSend("Freq: " + String(v) + " Hz\n");
        } else {
          LOCK(); int v=audioTone; UNLOCK();
          bleSend("Freq: " + String(audioTone) + " Hz\n");
        }
        break;
      }

      case 'L': {
        if (param.length() > 0) {
          int v = constrain(param.toInt(), 0, 4);
          LOCK();
          volumeLevel = v;
          isMuted = (v==0);
          switch(v) {
            case 1: toneVol=toneVol4; break;
            case 2: toneVol=toneVol3; break;
            case 3: toneVol=toneVol2; break;
            case 4: toneVol=toneVol1; break;
            default: break;
          }
          UNLOCK();
          postDisplay(DISP_VOL, v);
          savePreferences();
          bleSend("Vol: " + String(v) + "\n");
        } else {
          LOCK(); int v=volumeLevel; UNLOCK();
          bleSend("Vol: " + String(volumeLevel) + "\n");
        }
        break;
      }

      case 'W': {
        if (param.length() > 0) {
          float v = constrain(param.toInt()/100.0f, 0.8f, 1.2f);
          LOCK(); spacing_Weight=v; calculateMorseTiming(); UNLOCK();
          savePreferences();
          bleSend("Weight: " + String(v) + "\n");
        } else {
          LOCK(); float v=spacing_Weight; UNLOCK();
          bleSend("Weight: " + String(spacing_Weight) + "\n");
        }
        break;
      }

      case 'R': {
        if (param.length() > 0) {
          float v = constrain(param.toInt()/100.0f, 2.5f, 4.0f);
          LOCK(); dahRatio=v; calculateMorseTiming(); UNLOCK();
          savePreferences();
          bleSend("Ratio: " + String(v) + "\n");
        } else {
          LOCK(); float v=dahRatio; UNLOCK();
          bleSend("Ratio: " + String(dahRatio) + "\n");
        }
        break;
      }

      case 'T': {
        LOCK(); isTuneMode=!isTuneMode; bool t=isTuneMode; UNLOCK();
        postDisplay(DISP_TUNE, t?1:0);
        bleSend(t ? "Tune ON\n" : "Tune OFF\n");
        break;
      }

      case 'D':
case 'I': {
  LOCK();
  int      v_wpm    = wpm;
  int      v_freq   = audioTone;
  int      v_vol    = volumeLevel;
  bool     v_mute   = isMuted;
  float    v_ratio  = dahRatio;
  float    v_weight = spacing_Weight;
  bool     v_rev    = paddleReversed;
  bool     v_fw     = farnsworthEnabled;
  int      v_fwwpm  = farnsworthWPM;
  QrssMode v_qrss   = qrssMode;
  bool     v_tx     = txEnabled;
  bool     v_echo   = echoEnabled;
  String   v_buf    = buffer;
  UNLOCK();

  String s = "";
  s += "=== STATUS ===\n";
  s += "WPM:      " + String(v_wpm) + "\n";
  s += "Freq:     " + String(v_freq) + " Hz\n";
  s += "Vol:      " + String(v_mute ? 0 : volumeUiLevel(v_vol)) + (v_mute ? " (MUTE)" : "") + "\n";
  s += "Ratio:    " + String(v_ratio) + "\n";
  s += "Weight:   " + String(v_weight) + "\n";
  s += "Paddle:   " + String(v_rev ? "REVERSED" : "NORMAL") + "\n";
  s += "Mode:     " + getModeName() + "\n";
  s += "Farnswth: " + String(v_fw ? "ON eff=" + String(v_fwwpm) + " WPM" : "OFF") + "\n";
  s += "QRSS:     " + String(v_qrss == QRSS_OFF ? "OFF" : "QRSS" + String((int)v_qrss)) + "\n";
  s += "TX out:   " + String(v_tx   ? "ENABLED" : "DISABLED") + "\n";
  s += "Echo:     " + String(v_echo ? "ON" : "OFF") + "\n";
  s += "Buffer:   [" + v_buf + "]\n";
  s += "==============\n";

 bleSendLong(s);
  break;
}

   case 'G': { // Imposta GMT Offset: \G1 o \G2 o \G-5
        gmtOffsetHours = param.toInt();
        preferences.begin("cyd-morse", false);
        preferences.putLong("gmt_off", gmtOffsetHours);
        preferences.end();
        bleSend("GMT impostato a: " + String(gmtOffsetHours) + "\n");
        break;
      }

      case 'Z': { // Comando: \Z 02 03 2026 20 15 00 (tutto attaccato)
        if (param.length() == 14) {
          int gg = param.substring(0, 2).toInt();
          int mm = param.substring(2, 4).toInt();
          int aa = param.substring(4, 8).toInt();
          int hh = param.substring(8, 10).toInt();
          int mi = param.substring(10, 12).toInt();
          int ss = param.substring(12, 14).toInt();

          struct tm t = {0};
          t.tm_year = aa - 1900; 
          t.tm_mon  = mm - 1;    // Gennaio è 0
          t.tm_mday = gg;           
          t.tm_hour = hh;
          t.tm_min  = mi;
          t.tm_sec  = ss;
          t.tm_isdst = -1;

          time_t t_of_day = mktime(&t);
          struct timeval tv = { .tv_sec = t_of_day, .tv_usec = 0 };
          settimeofday(&tv, NULL);
          
          char confirm[50];
          sprintf(confirm, "OK! Data impostata: %02d/%02d/%04d %02d:%02d:%02d\n", gg, mm, aa, hh, mi, ss);
          bleSend(confirm);
          
          sdLogWrite("Orologio sincronizzato"); // Scrive subito il primo log per test
        } else {
          bleSend("Errore! Inserisci 14 cifre: GGMMAAAA HHMMSS\n");
        }
        break;
      }
      case 'A': {
        LOCK(); currentMode=IAMBIC_A; UNLOCK();
        postDisplay(DISP_IAMBIC_MODE,(int32_t)IAMBIC_A);
        bleSend("Mode: Iambic A\n");
        break;
      }

      case 'B': {
        LOCK(); currentMode=IAMBIC_B; UNLOCK();
        postDisplay(DISP_IAMBIC_MODE,(int32_t)IAMBIC_B);
        bleSend("Mode: Iambic B\n");
        break;
      }

      case 'K': {
        LOCK(); currentMode=MANUAL; UNLOCK();
        postDisplay(DISP_IAMBIC_MODE,(int32_t)MANUAL);
        bleSend("Mode: Manual\n");
        break;
      }

      case 'N': {
        if (param.length() > 0) {
          bool rev = (param.toInt() == 1);
          LOCK(); paddleReversed=rev; UNLOCK();
          savePreferences();
          bleSend(rev ? "Paddle: REVERSED\n" : "Paddle: NORMAL\n");
        } else {
          LOCK(); bool rev=paddleReversed; UNLOCK();
          bleSend(paddleReversed ? "Paddle: REVERSED\n" : "Paddle: NORMAL\n");
        }
        break;
      }

      case 'C': {
        clearBuffer();
        postDisplay(DISP_CLEAR);
        bleSend("Buffer cleared\n");
        break;
      }

      case 'U': {
        LOCK(); int len=buffer.length(); UNLOCK();
        bleSend("Buffer chars: " + String(len) + "\n");
        break;
      }

      case 'Q': {
        if (param.length() > 0) {
          int mode = constrain(param.toInt(), 0, 30);
          if (mode==0) qrssMode=QRSS_OFF;
          else if (mode==3)  qrssMode=QRSS_3;
          else if (mode==6)  qrssMode=QRSS_6;
          else if (mode==10) qrssMode=QRSS_10;
          else if (mode==30) qrssMode=QRSS_30;
          else { bleSend("Invalid QRSS (0,3,6,10,30)\n"); break; }
          LOCK(); if (wpm>60) wpm=20;
          if (qrssMode!=QRSS_OFF) qrssDitMs=(float)qrssMode*1000.0f;
          calculateMorseTiming(); UNLOCK();
          updateQrssButton();
          bleSend(qrssMode==QRSS_OFF ? "QRSS disabled\n" : "QRSS" + String(mode) + " enabled\n");
        } else {
          bleSend(qrssMode==QRSS_OFF ? "QRSS: OFF\n" : "QRSS" + String((int)qrssMode) + "\n");
        }
        break;
      }

      case 'Y': {
        if (param.length() > 0) {
          int eff = constrain(param.toInt(), 0, MAX_WPM);
          if (eff == 0) {
            LOCK(); farnsworthEnabled=false; calculateMorseTiming(); UNLOCK();
            bleSend("Farnsworth DISABLED\n");
          } else {
            LOCK();
            if (eff >= wpm) eff = wpm-1;
            farnsworthWPM=eff; farnsworthEnabled=true; calculateMorseTiming();
            UNLOCK();
            bleSend("Farnsworth: effective=" + String(eff) + " WPM\n");
          }
        } else {
          bleSend(farnsworthEnabled ? "Farnsworth: ON eff=" + String(farnsworthWPM) + "\n" : "Farnsworth: OFF\n");
        }
        break;
      }

      case 'P': {
        if (param.length() >= 1) {
          char letter = param.charAt(0);
          if (letter>='A' && letter<='Z') {
            String msg = param.substring(1); msg.trim();
            if (msg.length() == 0) {
              // Query
              bleSend("Memory " + String(letter) + " = [" + k3ngMem[letter-'A'] + "]\n");
            } else {
              // Set
              int idx = letter - 'A';
              k3ngMem[idx] = msg;
              saveMemory(idx);
              bleSend("Memory " + String(letter) + " saved: [" + msg + "]\n");
            }
          } else bleSend("ERR: use A-Z\n");
        } else {
          // \P — lista tutte le memorie non vuote
          String s = "=== Memories ===\n";
          bool any = false;
          for (int i = 0; i < NUM_MEM; i++) {
            if (k3ngMem[i].length() > 0) {
              s += String((char)('A'+i)) + ": [" + k3ngMem[i] + "]\n";
              any = true;
            }
          }
          if (!any) s += "(all empty)\n";
          s += "================\n";
          bleSendLong(s);
        }
        break;
      }
      case 'M': {
        if (param.length() >= 1) {
          char letter = param.charAt(0);
          if (letter>='A' && letter<='Z') {
            String msg = k3ngMem[letter-'A'];
            if (msg.length() > 0) {
              LOCK(); buffer += msg; UNLOCK();
              bleSend("Sending memory " + String(letter) + ": " + msg + "\n");
            } else bleSend("Memory " + String(letter) + " empty\n");
          }
        }
        break;
      }
      
      case 'O': {
        LOCK(); currentMode = AUTO_MODE; UNLOCK();
        postDisplay(DISP_IAMBIC_MODE, (int32_t)AUTO_MODE);
        bleSend("Mode: Auto\n");
        break;
      }

      case 'X': {
        LOCK(); txEnabled = !txEnabled; bool tx = txEnabled; UNLOCK();
        bleSend(tx ? "TX: ENABLED\n" : "TX: DISABLED (sidetone only)\n");
        break;
      }

      case 'E': {
        LOCK(); echoEnabled = !echoEnabled; bool ec = echoEnabled; UNLOCK();
        bleSend(ec ? "Echo: ON\n" : "Echo: OFF\n");
        break;
      }


      case 'V': {
        bleSend("=== " + TITLE + " " + VERSION + " ===\n");
        bleSend("HW: ESP32-CYD ILI9341\nProtocol: K3NG\nAuthor: IW1RMM\n");
        break;
      }

      case 'H': {
        String s = "";
        s += "K3NG Commands:\n";
        s += "\\S=WPM \\F=Freq \\L=Vol\n";
        s += "\\W=Weight \\R=Ratio\n";
        s += "\\A=IambicA \\B=IambicB\n";
        s += "\\K=Manual \\O=Auto\n";
        s += "\\N=Paddle reverse\n";
        s += "\\T=Tune \\C=Clear\n";
        s += "\\Q=QRSS \\Y=Farnsworth\n";
        s += "\\X=TX toggle\n";
        s += "\\E=Echo toggle\n";
        s += "\\P<A><txt>=SaveMem\n";
        s += "\\PA=QueryMem\n";
        s += "\\P=ListMems\n";
        s += "\\M<A>=SendMem\n";
        s += "\\I=Status \\V=Version\n";
        s += "\\H=Help\n";
        s += "\\Z=DateTime \\G=GMT\n";
        s += "text=send as morse\n";
        bleSendLong(s);
        break;
      }

      default:
        bleSend("Unknown cmd: " + cmd + "\n");
        break;
    }
  } else {
    // Testo normale → metti in coda morse
    LOCK(); buffer += cmd; UNLOCK();
    bleSend("TX: " + cmd + "\n");
  }
}

void checkSerialBle() {
  if (!bleEnabled || bleRxBuffer.length() == 0) return;
  static String bleCommand = "";
  while (bleRxBuffer.length() > 0) {
    char c = bleRxBuffer.charAt(0);
    bleRxBuffer.remove(0, 1);
    if (c == '\n' || c == '\r') {
      if (bleCommand.length() > 0) {
        processBleCommand(bleCommand);
        bleCommand = "";
      }
    } else {
      bleCommand += c;
    }
  }
}



void createTabSpare() {
  lv_obj_t *label = lv_label_create(tabSpare);
  lv_label_set_text(label, "Spare Tab\n(Reserved for future features)");
  lv_obj_center(label);
}

// ============================================================================
// DISPLAY QUEUE PROCESSING  Ã¢â‚¬â€  [FIX-1] chiamato SOLO dal lvglTask (Core 0)
// ============================================================================
void processDisplayQueue() {
  DisplayMsg m;
  // Svuota tutta la coda in un colpo solo, poi LVGL ridisegna
  while (xQueueReceive(displayQueue, &m, 0) == pdTRUE) {
    switch (m.type) {

      case DISP_TX_LED:
        lvgl_updateTxLed(m.payload.u8 != 0);
        break;

      case DISP_TX_BAR:
        lvgl_appendTxBar(m.payload.ch);
        break;

      case DISP_CLEAR:
        lvgl_clearAll();
        break;

      case DISP_WPM:
        lv_label_set_text_fmt(labelWpm1, "WPM:%d", (int)m.payload.i32);
        lv_label_set_text_fmt(labelWpm2, "WPM:%d", (int)m.payload.i32);
        break;

      case DISP_VOL: {
        int uiV = volumeUiLevel((int)m.payload.i32);
        if (m.payload.i32 == 0) {
          lv_label_set_text(labelVol1, "VOL:M");
          lv_label_set_text(labelVol2, "VOL:M");
        } else {
          lv_label_set_text_fmt(labelVol1, "VOL:%d", uiV);
          lv_label_set_text_fmt(labelVol2, "VOL:%d", uiV);
        }
        break;
      }

      case DISP_FREQ:
        lv_label_set_text_fmt(labelFreq1, "FRQ:%d", (int)m.payload.i32);
        lv_label_set_text_fmt(labelFreq2, "FRQ:%d", (int)m.payload.i32);
        break;

      case DISP_IAMBIC_MODE: {
        const char *txt; uint32_t col;
        switch ((KeyerMode)m.payload.i32) {
          case IAMBIC_A:  txt="IAMB-A"; col=0x0066CC; break;
          case IAMBIC_B:  txt="IAMB-B"; col=0x00AACC; break;
          case MANUAL:    txt="MANUAL"; col=0xFF8800; break;
          case AUTO_MODE: txt="AUTO";   col=0xAA00AA; break;
          default:        txt="IAMB-B"; col=0x00AACC; break;
        }
        if (btnIambicMode1) {
          lv_label_set_text(lv_obj_get_child(btnIambicMode1,0), txt);
          lv_obj_set_style_bg_color(btnIambicMode1, lv_color_hex(col), 0);
        }
        if (btnIambicMode2) {
          lv_label_set_text(lv_obj_get_child(btnIambicMode2,0), txt);
          lv_obj_set_style_bg_color(btnIambicMode2, lv_color_hex(col), 0);
        }
        break;
      }

      case DISP_KEYER_LABEL: {
        bool bufMode = (m.payload.i32 != 0);
        if (btnKeyerMode) {
          lv_label_set_text(lv_obj_get_child(btnKeyerMode,0),
                            bufMode ? "BUFFER" : "KEYER");
          lv_obj_set_style_bg_color(btnKeyerMode,
            bufMode ? lv_color_hex(0x00AA00) : lv_color_hex(0xCCCC00), 0);
        }
        break;
      }

      case DISP_TUNE:
        if (btnTune)
          lv_obj_set_style_bg_color(btnTune,
            m.payload.u8 ? lv_color_hex(0xFF0000)
                         : lv_palette_main(LV_PALETTE_GREY), 0);
        break;

      case DISP_BEACON:
        if (btnBeacon)
          lv_obj_set_style_bg_color(btnBeacon,
            m.payload.u8 ? lv_color_hex(0xFF0000)
                         : lv_palette_main(LV_PALETTE_GREY), 0);
        break;

      case DISP_FARNSWORTH:
        if (btnFarnsworth) {
          lv_obj_t *lbl = lv_obj_get_child(btnFarnsworth, 0);
          if (m.payload.i32 > 0) {
            char buf[32];
            sprintf(buf, "FARNSW: %d", (int)m.payload.i32);
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_bg_color(btnFarnsworth, lv_color_hex(0x00AA00), 0);
          } else {
            lv_label_set_text(lbl, "FARNSW: OFF");
            lv_obj_set_style_bg_color(btnFarnsworth, lv_color_hex(0x555555), 0);
          }
        }
        break;
    }
  }
}

// Aggiorna LED TX su entrambi i tab
void lvgl_updateTxLed(bool on) {
  lv_color_t col = on ? lv_color_hex(0xFF0000) : lv_color_hex(0x404040);
  if (ledTxCircle1) lv_obj_set_style_bg_color(ledTxCircle1, col, 0);
  if (ledTxCircle2) lv_obj_set_style_bg_color(ledTxCircle2, col, 0);
}

// Appende un carattere al ticker Ã¢â‚¬â€ logica identica a LCDprint_Char del vecchio sketch
// L3 = barra blu (attiva, max buffMainMax=22 chars, allineata a destra)
// L2 = nera bassa (overflow di L3, max buffSecMax=42)
// L1 = nera alta  (overflow di L2, max buffSecMax=42)
// Quando L3 ÃƒÂ¨ piena: char[0] di L3 -> L2, char[0] di L2 -> L1, L3 shift+append
void lvgl_appendTxBar(char c) {
  if ((int)stringBuf_L3.length() == buffMainMax) {
    // L3 piena: propaga il carattere piÃƒÂ¹ vecchio verso l'alto
    if ((int)stringBuf_L2.length() == buffSecMax) {
      // Anche L2 piena
      if ((int)stringBuf_L1.length() == buffSecMax) {
        // Anche L1 piena: scarta il piÃƒÂ¹ vecchio di L1
        stringBuf_L1 = stringBuf_L1.substring(1, buffSecMax);
        stringBuf_L1 = stringBuf_L1 + stringBuf_L2.substring(0, 1);
      } else {
        stringBuf_L1 = stringBuf_L1 + stringBuf_L2.substring(0, 1);
      }
      stringBuf_L2 = stringBuf_L2.substring(1, buffSecMax);
      stringBuf_L2 = stringBuf_L2 + stringBuf_L3.substring(0, 1);
    } else {
      // L2 non ancora piena: aggiunge solo
      stringBuf_L2 = stringBuf_L2 + stringBuf_L3.substring(0, 1);
    }
    // Shift L3 + nuovo char a destra
    stringBuf_L3 = stringBuf_L3.substring(1, buffMainMax);
    stringBuf_L3 = stringBuf_L3 + c;
  } else {
    // L3 non ancora piena: aggiunge a destra
    stringBuf_L3 = stringBuf_L3 + c;
  }
  // Aggiorna tutte le label (allineate a destra via stile LVGL)
  if (labelTx_t1)   lv_label_set_text(labelTx_t1,   stringBuf_L3.c_str());
  if (labelTx_t2)   lv_label_set_text(labelTx_t2,   stringBuf_L3.c_str());
  if (labelLog2_t1) lv_label_set_text(labelLog2_t1, stringBuf_L2.c_str());
  if (labelLog2_t2) lv_label_set_text(labelLog2_t2, stringBuf_L2.c_str());
  if (labelLog1_t1) lv_label_set_text(labelLog1_t1, stringBuf_L1.c_str());
  if (labelLog1_t2) lv_label_set_text(labelLog1_t2, stringBuf_L1.c_str());
}

void lvgl_clearAll() {
  stringBuf_L3 = ""; stringBuf_L2 = ""; stringBuf_L1 = "";
  if (labelTx_t1)   lv_label_set_text(labelTx_t1,   "Ready");
  if (labelTx_t2)   lv_label_set_text(labelTx_t2,   "Ready");
  if (labelLog1_t1) lv_label_set_text(labelLog1_t1, "");
  if (labelLog1_t2) lv_label_set_text(labelLog1_t2, "");
  if (labelLog2_t1) lv_label_set_text(labelLog2_t1, "");
  if (labelLog2_t2) lv_label_set_text(labelLog2_t2, "");
}

// ============================================================================
// EVENT HANDLERS  (tutti gira su Core 0 nel lvglTask)
// ============================================================================
void updateStatusLabels() {
  lv_label_set_text_fmt(labelWpm1, "WPM:%d", wpm);
  lv_label_set_text_fmt(labelWpm2, "WPM:%d", wpm);
  if (isMuted) {
    lv_label_set_text(labelVol1, "VOL:M");
    lv_label_set_text(labelVol2, "VOL:M");
  } else {
    lv_label_set_text_fmt(labelVol1, "VOL:%d", volumeUiLevel(volumeLevel));
    lv_label_set_text_fmt(labelVol2, "VOL:%d", volumeUiLevel(volumeLevel));
  }
  lv_label_set_text_fmt(labelFreq1, "FRQ:%d", audioTone);
  lv_label_set_text_fmt(labelFreq2, "FRQ:%d", audioTone);
  // Aggiorna anche tab SD Ã¢â‚¬â€ freq e vol sono cambiati
  sdUpdateInfoLabel();
  sdUpdateWpmLabel();
}

void onWpmUp(lv_event_t *e) {
  // Se QRSS attivo, impedisci cambio WPM
  if (qrssMode != QRSS_OFF) {
    Serial.println("WPM locked in QRSS mode");
    return;
  }
  LOCK();
  int currentWPM = wpm;
  UNLOCK();
  
  // Se raggiungiamo 60, apri popup HSCW slider
  if (currentWPM == 60) {
    showPopupHscw(60);
    return;
  }
  
  // Altrimenti incrementa normalmente (solo se <60)
  if (currentWPM < 60) {
    LOCK();
    wpm = constrain(wpm + 1, MIN_WPM, 60);
    calculateMorseTiming();
    UNLOCK();
    postDisplay(DISP_WPM, wpm);
    savePreferences();
  }
}

void onWpmDown(lv_event_t *e) {
  // Se QRSS attivo, impedisci cambio WPM
  if (qrssMode != QRSS_OFF) {
    Serial.println("WPM locked in QRSS mode");
    return;
  }
  LOCK();
  int currentWPM = wpm;
  UNLOCK();
  
  // Se siamo in HSCW (>60) e premiamo -, apri popup per tornare a normale
  if (currentWPM > 60) {
    showPopupHscw(currentWPM);
    return;
  }
  // Altrimenti decrementa normalmente
  LOCK();
  wpm = constrain(wpm - 1, MIN_WPM, 60);
  calculateMorseTiming();
  UNLOCK();
  postDisplay(DISP_WPM, wpm);
  savePreferences();
  Serial.print("WPM: "); Serial.println(wpm);
}

void onVolUp(lv_event_t *e) {
  LOCK();
  if (volumeLevel > 0) {
    volumeLevel--;
    if (volumeLevel == 0) {
      isMuted = true;
    } else {
      isMuted = false;
      switch(volumeLevel) {
        case 1: toneVol=toneVol1; break;
        case 2: toneVol=toneVol2; break;
        case 3: toneVol=toneVol3; break;
        case 4: toneVol=toneVol4; break;
      }
    }
  }
  UNLOCK();
  updateStatusLabels();
}
void onVolDown(lv_event_t *e) {
  LOCK();
  if (volumeLevel < 4) {
    volumeLevel++;
    isMuted = false;
    switch(volumeLevel) {
      case 1: toneVol=toneVol1; break;
      case 2: toneVol=toneVol2; break;
      case 3: toneVol=toneVol3; break;
      case 4: toneVol=toneVol4; break;
    }
  }
  UNLOCK();
  updateStatusLabels();
}

void onFreqUp(lv_event_t *e) {
  LOCK(); audioTone = constrain(audioTone+50, MIN_FREQ, MAX_FREQ); UNLOCK();
  updateStatusLabels();
}
void onFreqDown(lv_event_t *e) {
  LOCK(); audioTone = constrain(audioTone-50, MIN_FREQ, MAX_FREQ); UNLOCK();
  updateStatusLabels();
}

// [FIX-1] onIambicMode: aggiorna solo variabile locale, il display
//         viene aggiornato direttamente qui (siamo nel lvglTask/Core 0)
void onIambicMode(lv_event_t *e) {
  LOCK();
  switch(currentMode) {
    case IAMBIC_A:  currentMode=IAMBIC_B;   break;
    case IAMBIC_B:  currentMode=MANUAL;     break;
    case MANUAL:    currentMode=AUTO_MODE;  break;
    case AUTO_MODE: currentMode=IAMBIC_A;   break;
  }
  KeyerMode m = currentMode;
  UNLOCK();

  const char *txt; uint32_t col;
  switch(m) {
    case IAMBIC_A:  txt="IAMB-A"; col=0x0066CC; break;
    case IAMBIC_B:  txt="IAMB-B"; col=0x00AACC; break;
    case MANUAL:    txt="MANUAL"; col=0xFF8800; break;
    case AUTO_MODE: txt="AUTO";   col=0xAA00AA; break;
    default:        txt="IAMB-B"; col=0x00AACC; break;
  }
  if (btnIambicMode1) {
    lv_label_set_text(lv_obj_get_child(btnIambicMode1,0), txt);
    lv_obj_set_style_bg_color(btnIambicMode1, lv_color_hex(col), 0);
  }
  if (btnIambicMode2) {
    lv_label_set_text(lv_obj_get_child(btnIambicMode2,0), txt);
    lv_obj_set_style_bg_color(btnIambicMode2, lv_color_hex(col), 0);
  }
}

static unsigned long msgBtnPressTime[6] = {0};
static bool         msgBtnDown[6]      = {false};

void handleMsgBtn(lv_event_t *e, int idx, String &msg) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    msgBtnPressTime[idx] = millis(); msgBtnDown[idx] = true;
  } else if (code == LV_EVENT_RELEASED && msgBtnDown[idx]) {
    msgBtnDown[idx] = false;
    unsigned long held = millis() - msgBtnPressTime[idx];
    if (held >= 800) {
      // Long press: apre editor messaggio
      showMsgEditor(idx);
    } else {
      // Short press: invia messaggio
      LOCK();
      buffer += " ";   // spazio iniziale sempre, anche se buffer vuoto
      buffer += msg;
      isBufferMode = true;
      UNLOCK();
      postDisplay(DISP_KEYER_LABEL, 1);
    }
  }
}
void onCQ(lv_event_t *e)     { handleMsgBtn(e, 0, cqMessage); }
void onName(lv_event_t *e)   { handleMsgBtn(e, 1, nameMessage); }
void onAntRig(lv_event_t *e) { handleMsgBtn(e, 2, antRigMessage); }
void onQth(lv_event_t *e)  { handleMsgBtn(e, 3, qthMessage); }
void onRst(lv_event_t *e)  { handleMsgBtn(e, 4, rstMessage); }
void onTest(lv_event_t *e) { handleMsgBtn(e, 5, testMessage); }

void onClear(lv_event_t *e) {
  LOCK();
  buffer = ""; stopRequested = true;
  isBufferMode = false;
  UNLOCK();
  postDisplay(DISP_CLEAR);
  if (btnKeyerMode) {
    lv_label_set_text(lv_obj_get_child(btnKeyerMode,0), "KEYER");
    lv_obj_set_style_bg_color(btnKeyerMode, lv_color_hex(0xCCCC00), 0);
  }
}

void onKeyerMode(lv_event_t *e) {
  // Solo informativo - mostra stato corrente, nessun toggle manuale
  // Lo stato ÃƒÂ¨ gestito automaticamente da addToBuffer e clearBuffer
}

void onTune(lv_event_t *e) {
  LOCK(); isTuneMode = !isTuneMode; bool t=isTuneMode; UNLOCK();
  // Audio gestito dal morseTask; qui aggiorna solo colore pulsante
  // (il morseTask lo legge e attiva/disattiva il DAC)
  lv_obj_set_style_bg_color(btnTune,
    t ? lv_color_hex(0xFF0000) : lv_palette_main(LV_PALETTE_GREY), 0);
  Serial.println(t ? "Tune ON" : "Tune OFF");
}

void onMacro(lv_event_t *e) {
  LOCK();
  macroMessage = cqMessage + " " + nameMessage + " " + qthMessage + " " + rstMessage;
  buffer += macroMessage;
  isBufferMode = true;
  UNLOCK();
  lv_label_set_text(lv_obj_get_child(btnKeyerMode,0), "BUFFER");
  lv_obj_set_style_bg_color(btnKeyerMode, lv_color_hex(0x00AA00), 0);
}

void onStop(lv_event_t *e) {
  LOCK();
  stopRequested = true;
  beaconActive  = false;
  isTuneMode    = false;
  buffer        = "";
  ditPaddleFlag = 0; dahPaddleFlag = 0;
  ditOnFlag=false; dahOnFlag=false;
  ditToneFlag=false; dahToneFlag=false;
  manual_ON = false;
  morseNum = 1; paddleChar = false; paddleSpace = false;
  UNLOCK();
  digitalWrite(MORSE_OUT, LOW);
  dac1.disable();
  postDisplay(DISP_TX_LED, 0);
  lv_obj_set_style_bg_color(btnTune,   lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_set_style_bg_color(btnBeacon, lv_palette_main(LV_PALETTE_GREY), 0);
  Serial.println("STOP");
}

void onRecalibrate(lv_event_t *e) {
  touchCalReset();  // resetta solo namespace "touch_cal"
  Serial.println("Calibration reset - restarting...");
  delay(500);
  ESP.restart();
}

void onProtocolToggle(lv_event_t *e) {
  winkeyMode = !winkeyMode;
  
  // Aggiorna UI
  if (btnProtocol) {
    lv_obj_t *label = lv_obj_get_child(btnProtocol, 0);
    if (winkeyMode) {
      lv_label_set_text(label, "Mode: WINKEY");
      lv_obj_set_style_bg_color(btnProtocol, lv_color_hex(0x0066CC), 0);
      Serial.flush();
      Serial.begin(1200);
    } else {
      lv_label_set_text(label, "Mode: K3NG");
      lv_obj_set_style_bg_color(btnProtocol, lv_color_hex(0x00AA00), 0);
      winkeyHostOpen = false;
      Serial.flush();
      Serial.begin(115200);
      Serial.println("\n>>> K3NG mode ACTIVE <<<");
      Serial.println("Type \\H for help");
    }
  }
}



// ============================================================================
// POPUP BEACON / CONTEST Ã¢â‚¬â€ gestori press/release
// ============================================================================
static unsigned long beaconPressTime_btn = 0;
static bool beaconBtnDown = false;

void onBeacon(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    beaconPressTime_btn = millis(); beaconBtnDown = true;
  } else if (code == LV_EVENT_RELEASED && beaconBtnDown) {
    beaconBtnDown = false;
    unsigned long held = millis() - beaconPressTime_btn;
    LOCK(); bool active = beaconActive; UNLOCK();
    if (active) {
      LOCK(); beaconActive=false; stopRequested=true; buffer=""; UNLOCK();
      lv_obj_set_style_bg_color(btnBeacon, lv_palette_main(LV_PALETTE_GREY), 0);
    } else if (held >= 800) {
      LOCK(); beaconActive=true; lastBeaconTime=millis()-beaconInterval; UNLOCK();
      lv_obj_set_style_bg_color(btnBeacon, lv_color_hex(0xFF0000), 0);
    } else {
      showPopupBeacon();
    }
  }
}

static unsigned long contestPressTime_btn = 0;
static bool         contestBtnDown        = false;
static unsigned long contestLastRelease   = 0;  // per doppio tocco
static int          contestTapCount       = 0;

void onContest(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);

  if (code == LV_EVENT_PRESSED) {
    contestPressTime_btn = millis();
    contestBtnDown = true;
  }
  else if (code == LV_EVENT_RELEASED && contestBtnDown) {
    contestBtnDown = false;
    unsigned long held = millis() - contestPressTime_btn;
    unsigned long now  = millis();

    if (held >= 1000) {
      // Ã¢â€â‚¬Ã¢â€â‚¬ LONG PRESS >1s: invia RST + numero e incrementa Ã¢â€â‚¬Ã¢â€â‚¬
      contestTapCount = 0;
      sendContestMsg();
      LOCK();
      contestCounter++;
      if (contestCounter > 999) contestCounter = 1;
      int cc = contestCounter;
      UNLOCK();
      preferences.putInt("contestcnt", cc);
      char buf[12]; snprintf(buf,sizeof(buf),"CTEST\n%03d",cc);
      if (btnContest) lv_label_set_text(lv_obj_get_child(btnContest,0), buf);
      Serial.printf("Contest TX sent, next=%d\n", cc);

    } else if (held >= 400) {
      // Ã¢â€â‚¬Ã¢â€â‚¬ PRESS 400-1000ms: apri popup setting Ã¢â€â‚¬Ã¢â€â‚¬
      contestTapCount = 0;
      showPopupContest();

    } else {
      // Ã¢â€â‚¬Ã¢â€â‚¬ SHORT TAP <400ms: conta per doppio tocco Ã¢â€â‚¬Ã¢â€â‚¬
      if (now - contestLastRelease < 400) {
        // Secondo tocco rapido = RESET
        contestTapCount = 0;
        LOCK(); contestCounter = 1; UNLOCK();
        preferences.putInt("contestcnt", 1);
        if (btnContest) lv_label_set_text(lv_obj_get_child(btnContest,0), "CONTEST");
        Serial.println("Contest reset to 1");
        contestLastRelease = 0;  // impedisce terzo tocco
      } else {
        // Primo tocco: aspetta il secondo
        contestTapCount = 1;
        contestLastRelease = now;
      }
    }
  }
}

// ============================================================================
// POPUP HELPERS
// ============================================================================
lv_obj_t* createPopupBase(const char* title) {
  lv_obj_t *overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(overlay, screenWidth, screenHeight);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  popupBox = overlay;
  lv_obj_t *box = lv_obj_create(overlay);
  lv_obj_set_size(box, 240, 140);
  lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x1A1A2E), 0);
  lv_obj_set_style_border_color(box, lv_color_hex(0x0066CC), 0);
  lv_obj_set_style_border_width(box, 2, 0);
  lv_obj_set_style_radius(box, 8, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *lT = lv_label_create(box);
  lv_label_set_text(lT, title);
  lv_obj_set_style_text_color(lT, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lT, &lv_font_montserrat_14, 0);
  lv_obj_align(lT, LV_ALIGN_TOP_MID, 0, 0); //0,0 era 0, 6
  return box;
}

void closePopup() {
  if (popupBox) { lv_obj_del(popupBox); popupBox=NULL; popupValueLabel=NULL; }
}

void onPopupCancel(lv_event_t *e) { closePopup(); }

// ============================================================================
// EDITOR MESSAGGI  — tastiera LVGL, long press su CQ/NAME/ecc
// ============================================================================
static const char* msgNames[] = {"CQ","NAME","ANT/RIG","QTH","RST","TEST"};

void onMsgEditorSave(lv_event_t *e) {
  if (!msgEditorTA || msgEditIndex < 0) return;

  String newMsg = String(lv_textarea_get_text(msgEditorTA));
  newMsg.toUpperCase();

  // Aggiorna la variabile globale corretta
  char key[8];
  switch(msgEditIndex) {
    case 0: cqMessage     = newMsg; snprintf(key, 8, "msg0"); break;
    case 1: nameMessage   = newMsg; snprintf(key, 8, "msg1"); break;
    case 2: antRigMessage = newMsg; snprintf(key, 8, "msg2"); break;
    case 3: qthMessage    = newMsg; snprintf(key, 8, "msg3"); break;
    case 4: rstMessage    = newMsg; snprintf(key, 8, "msg4"); break;
    case 5: testMessage   = newMsg; snprintf(key, 8, "msg5"); break;
    default: return;
  }

  // Salvataggio NVS con flush garantito
  preferences.begin("cyd-morse", false);
  bool ok = preferences.putString(key, newMsg);
  preferences.end();

  Serial.printf("[SAVE] msg%d = \"%s\" -> %s\n",
                msgEditIndex, newMsg.c_str(), ok ? "OK" : "FAILED");

  // Chiudi editor
  if (msgEditorKb)  { lv_obj_del(msgEditorKb);  msgEditorKb  = NULL; }
  if (msgEditorBox) { lv_obj_del(msgEditorBox); msgEditorBox = NULL; msgEditorTA = NULL; }
}


void onMsgEditorCancel(lv_event_t *e) {
  if (msgEditorKb)  { lv_obj_del(msgEditorKb);  msgEditorKb=NULL; }
  if (msgEditorBox) { lv_obj_del(msgEditorBox); msgEditorBox=NULL; msgEditorTA=NULL; }
}

void showMsgEditor(int idx) {

  msgEditIndex = idx;

  String *msgs[] = {
    &cqMessage, &nameMessage, &antRigMessage,
    &qthMessage, &rstMessage, &testMessage
  };

  String current = (idx>=0 && idx<6) ? *msgs[idx] : "";

  // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
  // Overlay fullscreen 320x240
  // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
  lv_obj_t *overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(overlay, 320, 240);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(0x0A0A1A), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_set_style_pad_all(overlay, 0, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  msgEditorBox = overlay;

  // 
  // Titolo
  // 
  char title[32];
  snprintf(title, sizeof(title), "Edit: %s",
           idx<6 ? msgNames[idx] : "MSG");

  lv_obj_t *lTitle = lv_label_create(overlay);
  lv_label_set_text(lTitle, title);
  lv_obj_set_style_text_color(lTitle, lv_color_hex(0xFFFF00), 0);
  lv_obj_set_style_text_font(lTitle, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(lTitle, 6, 2);

  // 
  // Textarea
  // 
  lv_obj_t *ta = lv_textarea_create(overlay);
  lv_obj_set_size(ta, 308, 34);
  lv_obj_set_pos(ta, 6, 16);
  lv_textarea_set_text(ta, current.c_str());
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_max_length(ta, 60);
  lv_obj_set_style_text_font(ta, &lv_font_montserrat_12, 0);
  lv_obj_set_style_pad_all(ta, 4, 0);
  msgEditorTA = ta;

  // 
  // Bottoni centrati
  // 
  int btnW = 90;
  int gap  = 12;
  int totalW = btnW*2 + gap;
  int startX = (320 - totalW) / 2;

  lv_obj_t *btnSave = lv_btn_create(overlay);
  lv_obj_set_size(btnSave, btnW, 24);
  lv_obj_set_pos(btnSave, startX, 52);
  lv_obj_set_style_bg_color(btnSave, lv_color_hex(0x007700), 0);

  lv_obj_t *lS = lv_label_create(btnSave);
  lv_label_set_text(lS, "SAVE");
  lv_obj_set_style_text_font(lS, &lv_font_montserrat_12, 0);
  lv_obj_center(lS);

  lv_obj_add_event_cb(btnSave, onMsgEditorSave,
                      LV_EVENT_CLICKED, NULL);

  lv_obj_t *btnCnl = lv_btn_create(overlay);
  lv_obj_set_size(btnCnl, btnW, 24);
  lv_obj_set_pos(btnCnl, startX + btnW + gap, 52);
  lv_obj_set_style_bg_color(btnCnl, lv_color_hex(0x770000), 0);

  lv_obj_t *lC = lv_label_create(btnCnl);
  lv_label_set_text(lC, "CANCEL");
  lv_obj_set_style_text_font(lC, &lv_font_montserrat_12, 0);
  lv_obj_center(lC);

  lv_obj_add_event_cb(btnCnl, onMsgEditorCancel,
                      LV_EVENT_CLICKED, NULL);

  // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
  // Tastiera 4 righe compatta
  // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
  lv_obj_t *kb = lv_keyboard_create(overlay);
  lv_obj_set_width(kb, 320);
  lv_obj_set_height(kb, 150);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);

  lv_obj_set_style_pad_top(kb, 1, LV_PART_ITEMS);
  lv_obj_set_style_pad_bottom(kb, 1, LV_PART_ITEMS);
  lv_obj_set_style_pad_left(kb, 2, LV_PART_ITEMS);
  lv_obj_set_style_pad_right(kb, 2, LV_PART_ITEMS);
  lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);

  lv_keyboard_set_textarea(kb, ta);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_UPPER);

  msgEditorKb = kb;
}

// --- RATIO ---
void onRatioMinus(lv_event_t *e) {
  if (tempRatio>2.5f) tempRatio-=0.1f;
  if (tempRatio<2.5f) tempRatio=2.5f;
  char b[16]; snprintf(b,sizeof(b),"%.1f",tempRatio);
  lv_label_set_text(popupValueLabel,b);
}
void onRatioPlus(lv_event_t *e) {
  if (tempRatio<4.0f) tempRatio+=0.1f;
  if (tempRatio>4.0f) tempRatio=4.0f;
  char b[16]; snprintf(b,sizeof(b),"%.1f",tempRatio);
  lv_label_set_text(popupValueLabel,b);
}
void onRatioSave(lv_event_t *e) {
  LOCK(); dahRatio=tempRatio; calculateMorseTiming(); UNLOCK();
  savePreferences(); closePopup();
}
void onRatio(lv_event_t *e) { showPopupRatio(); }
void showPopupRatio() {
  tempRatio=dahRatio;
  lv_obj_t *box=createPopupBase("DAH:DIT RATIO");
  lv_obj_t *bM=lv_btn_create(box); lv_obj_set_size(bM,36,36);
  lv_obj_align(bM,LV_ALIGN_LEFT_MID,8,0);
  lv_label_set_text(lv_label_create(bM),"-"); lv_obj_center(lv_obj_get_child(bM,0));
  lv_obj_add_event_cb(bM,onRatioMinus,LV_EVENT_CLICKED,NULL);
  popupValueLabel=lv_label_create(box);
  char b[16]; snprintf(b,sizeof(b),"%.1f",tempRatio);
  lv_label_set_text(popupValueLabel,b);
  lv_obj_set_style_text_font(popupValueLabel,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_color(popupValueLabel,lv_color_hex(0xFFFF00),0);
  lv_obj_align(popupValueLabel,LV_ALIGN_CENTER,0,0);
  lv_obj_t *bP=lv_btn_create(box); lv_obj_set_size(bP,36,36);
  lv_obj_align(bP,LV_ALIGN_RIGHT_MID,-8,0);
  lv_label_set_text(lv_label_create(bP),"+"); lv_obj_center(lv_obj_get_child(bP,0));
  lv_obj_add_event_cb(bP,onRatioPlus,LV_EVENT_CLICKED,NULL);
  lv_obj_t *note=lv_label_create(box); lv_label_set_text(note,"(2.5 - 4.0)");
  lv_obj_set_style_text_color(note,lv_color_hex(0xAAAAAA),0);
  lv_obj_set_style_text_font(note,&lv_font_montserrat_12,0);
  lv_obj_align(note,LV_ALIGN_BOTTOM_MID,0,-30);
  lv_obj_t *bS=lv_btn_create(box); lv_obj_set_size(bS,80,28);
  lv_obj_align(bS,LV_ALIGN_BOTTOM_LEFT,8,-4);
  lv_obj_set_style_bg_color(bS,lv_color_hex(0x00AA00),0);
  lv_label_set_text(lv_label_create(bS),"SAVE"); lv_obj_center(lv_obj_get_child(bS,0));
  lv_obj_add_event_cb(bS,onRatioSave,LV_EVENT_CLICKED,NULL);
  lv_obj_t *bC=lv_btn_create(box); lv_obj_set_size(bC,80,28);
  lv_obj_align(bC,LV_ALIGN_BOTTOM_RIGHT,-8,-4);
  lv_obj_set_style_bg_color(bC,lv_color_hex(0xAA0000),0);
  lv_label_set_text(lv_label_create(bC),"CANCEL"); lv_obj_center(lv_obj_get_child(bC,0));
  lv_obj_add_event_cb(bC,onPopupCancel,LV_EVENT_CLICKED,NULL);
}

// --- WEIGHT ---
void onWeightMinus(lv_event_t *e) {
  if (tempWeight>0.8f) tempWeight-=0.05f;
  if (tempWeight<0.8f) tempWeight=0.8f;
  char b[16]; snprintf(b,sizeof(b),"%.2f",tempWeight);
  lv_label_set_text(popupValueLabel,b);
}
void onWeightPlus(lv_event_t *e) {
  if (tempWeight<1.2f) tempWeight+=0.05f;
  if (tempWeight>1.2f) tempWeight=1.2f;
  char b[16]; snprintf(b,sizeof(b),"%.2f",tempWeight);
  lv_label_set_text(popupValueLabel,b);
}
void onWeightSave(lv_event_t *e) {
  LOCK(); spacing_Weight=tempWeight; calculateMorseTiming(); UNLOCK();
  savePreferences(); closePopup();
}
void onWeight(lv_event_t *e) { showPopupWeight(); }
void showPopupWeight() {
  tempWeight=spacing_Weight;
  lv_obj_t *box=createPopupBase("SPACING WEIGHT");
  lv_obj_t *bM=lv_btn_create(box); lv_obj_set_size(bM,36,36);
  lv_obj_align(bM,LV_ALIGN_LEFT_MID,8,0);
  lv_label_set_text(lv_label_create(bM),"-"); lv_obj_center(lv_obj_get_child(bM,0));
  lv_obj_add_event_cb(bM,onWeightMinus,LV_EVENT_CLICKED,NULL);
  popupValueLabel=lv_label_create(box);
  char b[16]; snprintf(b,sizeof(b),"%.2f",tempWeight);
  lv_label_set_text(popupValueLabel,b);
  lv_obj_set_style_text_font(popupValueLabel,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_color(popupValueLabel,lv_color_hex(0xFFFF00),0);
  lv_obj_align(popupValueLabel,LV_ALIGN_CENTER,0,0);
  lv_obj_t *bP=lv_btn_create(box); lv_obj_set_size(bP,36,36);
  lv_obj_align(bP,LV_ALIGN_RIGHT_MID,-8,0);
  lv_label_set_text(lv_label_create(bP),"+"); lv_obj_center(lv_obj_get_child(bP,0));
  lv_obj_add_event_cb(bP,onWeightPlus,LV_EVENT_CLICKED,NULL);
  lv_obj_t *note=lv_label_create(box); lv_label_set_text(note,"(0.80 - 1.20)");
  lv_obj_set_style_text_color(note,lv_color_hex(0xAAAAAA),0);
  lv_obj_set_style_text_font(note,&lv_font_montserrat_12,0);
  lv_obj_align(note,LV_ALIGN_BOTTOM_MID,0,-30);
  lv_obj_t *bS=lv_btn_create(box); lv_obj_set_size(bS,80,28);
  lv_obj_align(bS,LV_ALIGN_BOTTOM_LEFT,8,-4);
  lv_obj_set_style_bg_color(bS,lv_color_hex(0x00AA00),0);
  lv_label_set_text(lv_label_create(bS),"SAVE"); lv_obj_center(lv_obj_get_child(bS,0));
  lv_obj_add_event_cb(bS,onWeightSave,LV_EVENT_CLICKED,NULL);
  lv_obj_t *bC=lv_btn_create(box); lv_obj_set_size(bC,80,28);
  lv_obj_align(bC,LV_ALIGN_BOTTOM_RIGHT,-8,-4);
  lv_obj_set_style_bg_color(bC,lv_color_hex(0xAA0000),0);
  lv_label_set_text(lv_label_create(bC),"CANCEL"); lv_obj_center(lv_obj_get_child(bC,0));
  lv_obj_add_event_cb(bC,onPopupCancel,LV_EVENT_CLICKED,NULL);
}

// --- PAD.REV ---
void onPadRevToggle(lv_event_t *e) {
  tempPadRev=!tempPadRev;
  lv_label_set_text(popupValueLabel, tempPadRev ? "REVERSED" : "NORMAL");
  lv_obj_set_style_text_color(popupValueLabel,
    tempPadRev ? lv_color_hex(0xFF6600) : lv_color_hex(0x00FF00), 0);
}
void onPadRevSave(lv_event_t *e) {
  LOCK(); paddleReversed=tempPadRev; UNLOCK();
  savePreferences();
  lv_obj_set_style_bg_color(btnPadRev,
    paddleReversed ? lv_color_hex(0xFF6600) : lv_palette_main(LV_PALETTE_GREY), 0);
  closePopup();
}
void onPadRev(lv_event_t *e) { showPopupPadRev(); }
void showPopupPadRev() {
  tempPadRev=paddleReversed;
  lv_obj_t *box=createPopupBase("PADDLE REVERSE");
  lv_obj_t *bT=lv_btn_create(box); lv_obj_set_size(bT,140,40);
  lv_obj_align(bT,LV_ALIGN_CENTER,0,0);
  lv_obj_set_style_bg_color(bT,lv_color_hex(0x0055AA),0);
  lv_label_set_text(lv_label_create(bT),"TOGGLE"); lv_obj_center(lv_obj_get_child(bT,0));
  lv_obj_add_event_cb(bT,onPadRevToggle,LV_EVENT_CLICKED,NULL);
  popupValueLabel=lv_label_create(box);
  lv_label_set_text(popupValueLabel, tempPadRev ? "REVERSED" : "NORMAL");
  lv_obj_set_style_text_font(popupValueLabel,&lv_font_montserrat_16,0);
  lv_obj_set_style_text_color(popupValueLabel,
    tempPadRev ? lv_color_hex(0xFF6600) : lv_color_hex(0x00FF00),0);
  lv_obj_align(popupValueLabel,LV_ALIGN_TOP_MID,0,26);
  lv_obj_t *bS=lv_btn_create(box); lv_obj_set_size(bS,80,28);
  lv_obj_align(bS,LV_ALIGN_BOTTOM_LEFT,8,-4);
  lv_obj_set_style_bg_color(bS,lv_color_hex(0x00AA00),0);
  lv_label_set_text(lv_label_create(bS),"SAVE"); lv_obj_center(lv_obj_get_child(bS,0));
  lv_obj_add_event_cb(bS,onPadRevSave,LV_EVENT_CLICKED,NULL);
  lv_obj_t *bC=lv_btn_create(box); lv_obj_set_size(bC,80,28);
  lv_obj_align(bC,LV_ALIGN_BOTTOM_RIGHT,-8,-4);
  lv_obj_set_style_bg_color(bC,lv_color_hex(0xAA0000),0);
  lv_label_set_text(lv_label_create(bC),"CANCEL"); lv_obj_center(lv_obj_get_child(bC,0));
  lv_obj_add_event_cb(bC,onPopupCancel,LV_EVENT_CLICKED,NULL);
}

// --- BEACON ---
void onBeaconMinus(lv_event_t *e) {
  if (tempBeacon>10) tempBeacon-=10;
  char b[16]; snprintf(b,sizeof(b),"%d s",tempBeacon);
  lv_label_set_text(popupValueLabel,b);
}
void onBeaconPlus(lv_event_t *e) {
  if (tempBeacon<300) tempBeacon+=10;
  char b[16]; snprintf(b,sizeof(b),"%d s",tempBeacon);
  lv_label_set_text(popupValueLabel,b);
}
void onBeaconSave(lv_event_t *e) {
  LOCK(); beaconIntervalSec=tempBeacon; beaconInterval=(unsigned long)tempBeacon*1000UL; UNLOCK();
  savePreferences(); closePopup();
}
void showPopupBeacon() {
  tempBeacon=beaconIntervalSec;
  lv_obj_t *box=createPopupBase("BEACON INTERVAL");
  lv_obj_t *bM=lv_btn_create(box); lv_obj_set_size(bM,36,36);
  lv_obj_align(bM,LV_ALIGN_LEFT_MID,8,0);
  lv_label_set_text(lv_label_create(bM),"-"); lv_obj_center(lv_obj_get_child(bM,0));
  lv_obj_add_event_cb(bM,onBeaconMinus,LV_EVENT_CLICKED,NULL);
  popupValueLabel=lv_label_create(box);
  char b[16]; snprintf(b,sizeof(b),"%d s",tempBeacon);
  lv_label_set_text(popupValueLabel,b);
  lv_obj_set_style_text_font(popupValueLabel,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_color(popupValueLabel,lv_color_hex(0xFFFF00),0);
  lv_obj_align(popupValueLabel,LV_ALIGN_CENTER,0,0);
  lv_obj_t *bP=lv_btn_create(box); lv_obj_set_size(bP,36,36);
  lv_obj_align(bP,LV_ALIGN_RIGHT_MID,-8,0);
  lv_label_set_text(lv_label_create(bP),"+"); lv_obj_center(lv_obj_get_child(bP,0));
  lv_obj_add_event_cb(bP,onBeaconPlus,LV_EVENT_CLICKED,NULL);
  lv_obj_t *note=lv_label_create(box); lv_label_set_text(note,"(10 - 300 sec)");
  lv_obj_set_style_text_color(note,lv_color_hex(0xAAAAAA),0);
  lv_obj_set_style_text_font(note,&lv_font_montserrat_12,0);
  lv_obj_align(note,LV_ALIGN_BOTTOM_MID,0,-30);
  lv_obj_t *bS=lv_btn_create(box); lv_obj_set_size(bS,80,28);
  lv_obj_align(bS,LV_ALIGN_BOTTOM_LEFT,8,-4);
  lv_obj_set_style_bg_color(bS,lv_color_hex(0x00AA00),0);
  lv_label_set_text(lv_label_create(bS),"SAVE"); lv_obj_center(lv_obj_get_child(bS,0));
  lv_obj_add_event_cb(bS,onBeaconSave,LV_EVENT_CLICKED,NULL);
  lv_obj_t *bC=lv_btn_create(box); lv_obj_set_size(bC,80,28);
  lv_obj_align(bC,LV_ALIGN_BOTTOM_RIGHT,-8,-4);
  lv_obj_set_style_bg_color(bC,lv_color_hex(0xAA0000),0);
  lv_label_set_text(lv_label_create(bC),"CANCEL"); lv_obj_center(lv_obj_get_child(bC,0));
  lv_obj_add_event_cb(bC,onPopupCancel,LV_EVENT_CLICKED,NULL);
}

// --- CONTEST ---
void onContestMinus(lv_event_t *e) {
  if (tempContest>1) tempContest--;
  char b[8]; snprintf(b,sizeof(b),"%d",tempContest);
  lv_label_set_text(popupValueLabel,b);
}
void onContestPlus(lv_event_t *e) {
  if (tempContest<999) tempContest++;
  char b[8]; snprintf(b,sizeof(b),"%d",tempContest);
  lv_label_set_text(popupValueLabel,b);
}
void onContestSave(lv_event_t *e) {
  LOCK(); contestCounter=tempContest; UNLOCK();
  preferences.putInt("contestcnt",contestCounter);
  closePopup();
}
void showPopupContest() {
  LOCK(); tempContest=contestCounter; UNLOCK();
  lv_obj_t *box=createPopupBase("CONTEST COUNTER");
  lv_obj_t *bM=lv_btn_create(box); lv_obj_set_size(bM,36,36);
  lv_obj_align(bM,LV_ALIGN_LEFT_MID,8,0);
  lv_label_set_text(lv_label_create(bM),"-"); lv_obj_center(lv_obj_get_child(bM,0));
  lv_obj_add_event_cb(bM,onContestMinus,LV_EVENT_CLICKED,NULL);
  popupValueLabel=lv_label_create(box);
  char b[8]; snprintf(b,sizeof(b),"%d",tempContest);
  lv_label_set_text(popupValueLabel,b);
  lv_obj_set_style_text_font(popupValueLabel,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_color(popupValueLabel,lv_color_hex(0xFFFF00),0);
  lv_obj_align(popupValueLabel,LV_ALIGN_CENTER,0,0);
  lv_obj_t *bP=lv_btn_create(box); lv_obj_set_size(bP,36,36);
  lv_obj_align(bP,LV_ALIGN_RIGHT_MID,-8,0);
  lv_label_set_text(lv_label_create(bP),"+"); lv_obj_center(lv_obj_get_child(bP,0));
  lv_obj_add_event_cb(bP,onContestPlus,LV_EVENT_CLICKED,NULL);
  lv_obj_t *note=lv_label_create(box); lv_label_set_text(note,"(1 - 999)");
  lv_obj_set_style_text_color(note,lv_color_hex(0xAAAAAA),0);
  lv_obj_set_style_text_font(note,&lv_font_montserrat_12,0);
  lv_obj_align(note,LV_ALIGN_BOTTOM_MID,0,-30);
  lv_obj_t *bS=lv_btn_create(box); lv_obj_set_size(bS,80,28);
  lv_obj_align(bS,LV_ALIGN_BOTTOM_LEFT,8,-4);
  lv_obj_set_style_bg_color(bS,lv_color_hex(0x00AA00),0);
  lv_label_set_text(lv_label_create(bS),"SAVE"); lv_obj_center(lv_obj_get_child(bS,0));
  lv_obj_add_event_cb(bS,onContestSave,LV_EVENT_CLICKED,NULL);
  lv_obj_t *bC=lv_btn_create(box); lv_obj_set_size(bC,80,28);
  lv_obj_align(bC,LV_ALIGN_BOTTOM_RIGHT,-8,-4);
  lv_obj_set_style_bg_color(bC,lv_color_hex(0xAA0000),0);
  lv_label_set_text(lv_label_create(bC),"CANCEL"); lv_obj_center(lv_obj_get_child(bC,0));
  lv_obj_add_event_cb(bC,onPopupCancel,LV_EVENT_CLICKED,NULL);
}

// ============================================================================
// MORSE LOGIC  Ã¢â‚¬â€  tutto in morseTask (Core 1); NESSUNA chiamata lv_*
// ============================================================================

void addToBuffer(String msg) {
  LOCK(); buffer += msg; isBufferMode=true; UNLOCK();
  postDisplay(DISP_KEYER_LABEL, 1);
}

void clearBuffer() {
  LOCK(); buffer=""; stopRequested=false; isBufferMode=false; UNLOCK();
  postDisplay(DISP_KEYER_LABEL, 0);
}

void sendContestMsg() {
  char numStr[5]; snprintf(numStr,sizeof(numStr)," %03d",contestCounter);
  String msg = rstMessage + String(numStr);
  addToBuffer(msg);
  LOCK(); contestActive=true; UNLOCK();
}

// [FIX-2] morseToneOn/Off usano morseDelay invece di delay()
void morseToneOn(int ms) {
  LOCK(); bool muted=isMuted; dac_cosine_atten_t vol=toneVol; int freq=audioTone; bool txOn=txEnabled; UNLOCK();
  if (txOn) digitalWrite(MORSE_OUT, HIGH);          // \X: key fisico solo se TX abilitato
  if (!muted) { dac1.outputCW(freq, vol); dac1.enable(); }   // sidetone sempre
  postDisplay(DISP_TX_LED, 1);
  morseDelay(ms);
  if (txOn) digitalWrite(MORSE_OUT, LOW);
  dac1.disable();
  postDisplay(DISP_TX_LED, 0);
}


void morseToneOff(int ms) {
  morseDelay(ms);
}

void sendDit(int ditLen) {
  morseToneOn(ditLen);
  LOCK(); int es=(int)elemSpace; UNLOCK();
  morseToneOff(es);
}

void sendDah(int dahLen) {
  morseToneOn(dahLen);
  LOCK(); int es=(int)elemSpace; UNLOCK();
  morseToneOff(es);
}

void sendFromBuffer() {
  LOCK();
  bool stop = stopRequested;
  int  blen = buffer.length();
  UNLOCK();

  if (stop) {
    LOCK(); buffer=""; stopRequested=false; isBufferMode=false; UNLOCK();
    postDisplay(DISP_KEYER_LABEL, 0);
    return;
  }
  if (blen == 0) return;

  LOCK();
char c = buffer.charAt(0);

// Controlla se è un prosign <XX>
if (c == '<') {
  int closeIdx = buffer.indexOf('>');
  if (closeIdx > 1 && closeIdx <= 4) {
    // Estrai codice prosign es. "AR" da "<AR>"
    String prosignCode = buffer.substring(1, closeIdx);
    prosignCode.toUpperCase();
    buffer.remove(0, closeIdx + 1);
    blen = buffer.length();
    UNLOCK();
    Serial.printf("<%s>", prosignCode.c_str());
    sendProsign(prosignCode);
    if (blen == 0) {
      LOCK(); isBufferMode = false; UNLOCK();
      postDisplay(DISP_KEYER_LABEL, 0);
    }
    return;
  }
}

buffer.remove(0, 1);
blen = buffer.length();
UNLOCK();
Serial.print(c);
sendMorseChar(c);

  if (blen == 0) {
    LOCK(); isBufferMode=false; UNLOCK();
    postDisplay(DISP_KEYER_LABEL, 0);
  }
}

// ============================================================================
// PROSIGN — invia sequenza senza inter-char space
// Convenzione: <AR> <SK> <BT> <KN> <AS> <SN> <HH> <CL>
// ============================================================================
void sendProsign(const String &code) {
  LOCK();
  int ditLen = (int)ditLength;
  int dahLen = (int)dahLength;
  int cs     = (int)charSpace;
  UNLOCK();

  // Mappa prosign -> sequenza dit/dah
  if      (code == "AR") { sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); }
  else if (code == "SK") { sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); sendDah(dahLen); }
  else if (code == "BT") { sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); }
  else if (code == "KN") { sendDah(dahLen); sendDit(ditLen); sendDah(dahLen); sendDah(dahLen); sendDit(ditLen); }
  else if (code == "AS") { sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); }
  else if (code == "SN") { sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); }
  else if (code == "HH") { sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); }
  else if (code == "CL") { sendDah(dahLen); sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); }
  else {
    // Prosign non riconosciuto: invia le lettere normalmente con spazio
    Serial.printf("[PROSIGN] Unknown: <%s> - sending as letters\n", code.c_str());
    for (int i = 0; i < code.length(); i++) {
      sendMorseChar(code.charAt(i));
    }
    return;
  }

  morseToneOff(cs);  // inter-char space dopo il prosign
  // Mostra sul display come <XX>
  String disp = "<" + code + ">";
  for (int i = 0; i < disp.length(); i++) postDisplayChar(disp.charAt(i));
}




void sendMorseChar(char c) {
  c = toupper(c);
  LOCK(); int ditLen=(int)ditLength; int dahLen=(int)dahLength; int cs=(int)charSpace; int ws=(int)(wordSpace_-charSpace); UNLOCK();

  if (c == ' ') {
    morseToneOff(ws);
    postDisplayChar(c);
    return;
  }

  switch(c) {
    case 'A': sendDit(ditLen); sendDah(dahLen); break;
    case 'B': sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); break;
    case 'C': sendDah(dahLen); sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); break;
    case 'D': sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); break;
    case 'E': sendDit(ditLen); break;
    case 'F': sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); break;
    case 'G': sendDah(dahLen); sendDah(dahLen); sendDit(ditLen); break;
    case 'H': sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); break;
    case 'I': sendDit(ditLen); sendDit(ditLen); break;
    case 'J': sendDit(ditLen); sendDah(dahLen); sendDah(dahLen); sendDah(dahLen); break;
    case 'K': sendDah(dahLen); sendDit(ditLen); sendDah(dahLen); break;
    case 'L': sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); break;
    case 'M': sendDah(dahLen); sendDah(dahLen); break;
    case 'N': sendDah(dahLen); sendDit(ditLen); break;
    case 'O': sendDah(dahLen); sendDah(dahLen); sendDah(dahLen); break;
    case 'P': sendDit(ditLen); sendDah(dahLen); sendDah(dahLen); sendDit(ditLen); break;
    case 'Q': sendDah(dahLen); sendDah(dahLen); sendDit(ditLen); sendDah(dahLen); break;
    case 'R': sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); break;
    case 'S': sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); break;
    case 'T': sendDah(dahLen); break;
    case 'U': sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); break;
    case 'V': sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); break;
    case 'W': sendDit(ditLen); sendDah(dahLen); sendDah(dahLen); break;
    case 'X': sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); break;
    case 'Y': sendDah(dahLen); sendDit(ditLen); sendDah(dahLen); sendDah(dahLen); break;
    case 'Z': sendDah(dahLen); sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); break;
    case '0': sendDah(dahLen); sendDah(dahLen); sendDah(dahLen); sendDah(dahLen); sendDah(dahLen); break;
    case '1': sendDit(ditLen); sendDah(dahLen); sendDah(dahLen); sendDah(dahLen); sendDah(dahLen); break;
    case '2': sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); sendDah(dahLen); sendDah(dahLen); break;
    case '3': sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); sendDah(dahLen); break;
    case '4': sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); break;
    case '5': sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); break;
    case '6': sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); break;
    case '7': sendDah(dahLen); sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); break;
    case '8': sendDah(dahLen); sendDah(dahLen); sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); break;
    case '9': sendDah(dahLen); sendDah(dahLen); sendDah(dahLen); sendDah(dahLen); sendDit(ditLen); break;
    case '.': sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); sendDah(dahLen); break;
    case ',': sendDah(dahLen); sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); sendDah(dahLen); break;
    case '?': sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); break;
    case '/': sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); break;
    case '=': sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); break;
    case '@': sendDit(ditLen); sendDah(dahLen); sendDah(dahLen); sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); break;
    case '<': sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); sendDah(dahLen); sendDit(ditLen); break; // AR
    case '>': sendDah(dahLen); sendDit(ditLen); sendDit(ditLen); sendDit(ditLen); sendDah(dahLen); break; // BT
    default:  morseToneOff(ditLen*2); break;
  }
  morseToneOff(cs);        // inter-char space
  postDisplayChar(c);      // aggiorna barra blu tramite queue
}

// ============================================================================
// PADDLE KEYER
// ============================================================================
void paddle_morse_On() {
  LOCK(); bool muted=isMuted; dac_cosine_atten_t vol=toneVol; int freq=audioTone; bool txOn=txEnabled; UNLOCK();
  if (txOn) digitalWrite(MORSE_OUT, HIGH);          // \X: key fisico solo se TX abilitato
  if (!muted) { dac1.outputCW(freq, vol); dac1.enable(); }   // sidetone sempre
  postDisplay(DISP_TX_LED, 1);
}
void paddle_morse_Off() {
  LOCK(); bool txOn=txEnabled; UNLOCK();
  if (txOn) digitalWrite(MORSE_OUT, LOW);
  dac1.disable();
  postDisplay(DISP_TX_LED, 0);
}

void paddle_beginDIT() {
  paddleStartTime=millis(); paddle_morse_On();
  ditOnFlag=true; ditToneFlag=true;
  // morseNum aggiornato in paddleTimer quando il tono finisce
}
void paddle_beginDAH() {
  paddleStartTime=millis(); paddle_morse_On();
  dahOnFlag=true; dahToneFlag=true;
  // morseNum aggiornato in paddleTimer quando il tono finisce
}

void paddleTimer() {
  paddleCountTime = millis();
  LOCK(); int dLen=(int)ditLength; int dALen=(int)dahLength; int eLen=(int)elemSpace; UNLOCK();
  if (ditToneFlag && (paddleCountTime-paddleStartTime >= (unsigned long)dLen)) {
    paddleEndTime=paddleCountTime; paddle_morse_Off(); ditToneFlag=false;
    paddleOutTime = paddleCountTime;
    // DIT finito: shift left senza +1 (uguale a vecchio sketch: E=. -> index 2 = 1<<1)
    morseNum = morseNum << 1;
    paddleChar = true; paddleSpace = false;
  }
  if (dahToneFlag && (paddleCountTime-paddleStartTime >= (unsigned long)dALen)) {
    paddleEndTime=paddleCountTime; paddle_morse_Off(); dahToneFlag=false;
    paddleOutTime = paddleCountTime;
    // DAH finito: shift left + 1 (uguale a vecchio sketch: T=- -> index 3 = (1<<1)+1)
    morseNum = (morseNum << 1) + 1;
    paddleChar = true; paddleSpace = false;
  }
  if (ditOnFlag && !ditToneFlag && (paddleCountTime-paddleEndTime >= (unsigned long)eLen))
    ditOnFlag=false;
  if (dahOnFlag && !dahToneFlag && (paddleCountTime-paddleEndTime >= (unsigned long)eLen))
    dahOnFlag=false;
}

// Decodifica paddle Ã¢â‚¬â€ identico a printDitDah() del vecchio sketch
// Chiamata ogni ciclo morseTask; usa timing reale dit/dah per decodifica
void printDitDah() {
  LOCK(); float dLen = ditLength; UNLOCK();
  unsigned long elementL = (unsigned long)dLen;  // lunghezza dit in ms

  // Se il carattere ÃƒÂ¨ pronto e non c'ÃƒÂ¨ tono in corso
  if (paddleChar && !ditToneFlag && !dahToneFlag) {
    paddleInTime = millis();
    if ((paddleInTime - paddleOutTime) >= elementL * 2) {
      // Inter-character space: decodifica
      if (morseNum >= 0 && morseNum < mySetSize && mySet[morseNum] != 0) {
        char decoded = mySet[morseNum];
        postDisplayChar(decoded);
        // \E Echo paddle: rimanda carattere decodificato su Serial e BLE
        LOCK(); bool echo = echoEnabled; UNLOCK();
        if (echo) {
          Serial.print(decoded);
          bleSend(String(decoded));
        }
        // SD Logging paddle (opzionale - commentare se troppo verboso)
        // sdLogWrite(String("PADDLE: ") + decoded);
        paddleChar  = false;
        paddleSpace = true;
        morseNum    = 1;
      } else {
        morseNum = 1;  // fuori range, reset
        paddleChar = false;
      }
    }
  }
  // Word space
  else if (paddleSpace && !ditToneFlag && !dahToneFlag) {
    paddleInTime = millis();
    if ((paddleInTime - paddleOutTime) >= elementL * 7) {
      postDisplayChar(' ');
      paddleSpace = false;
    }
  }
}

void checkPaddle() {
  // Disabilita paddle in modalità QRSS (troppo lento per uso manuale)
  if (qrssMode != QRSS_OFF) return;
  int rawDah = digitalRead(PADDLE_DAH);
  int rawDit = digitalRead(PADDLE_DIT);
  LOCK(); bool rev=paddleReversed; KeyerMode mode=currentMode; UNLOCK();
  int dahState = rev ? rawDit : rawDah;
  int ditState = rev ? rawDah : rawDit;

  switch(mode) {
    case MANUAL:
      if (ditState==LOW && !manual_ON) {
        paddle_morse_On();
        manual_ON       = true;
        paddleStartTime = millis();   // inizia misurazione durata elemento
        ditToneFlag     = true;       // mantiene tono attivo per paddleTimer
      }
      else if (ditState==HIGH && manual_ON) {
        paddle_morse_Off();
        manual_ON   = false;
        ditToneFlag = false;                        // tono finito
        paddleEndTime = millis();
        paddleOutTime = paddleEndTime;              // base timing inter-char/word space
        LOCK(); int dLen=(int)ditLength; UNLOCK();
        long dur = (long)(paddleEndTime - paddleStartTime);
        if (dur < dLen * 2) {
          morseNum = morseNum << 1;                 // DIT: shift left
        } else {
          morseNum = (morseNum << 1) + 1;           // DAH: shift left + 1
        }
        paddleChar  = true;
        paddleSpace = false;
      }
      // NON chiamare paddleTimer() in MANUAL: timing gestito sopra
      break;

    case AUTO_MODE:
      if (ditState==LOW && dahState==HIGH && !dahOnFlag && !ditOnFlag) paddle_beginDIT();
      else if (dahState==LOW && ditState==HIGH && !dahOnFlag && !ditOnFlag) paddle_beginDAH();
      paddleTimer();
      break;

    case IAMBIC_A:
      if (dahState==LOW && dahPaddleFlag==0 && !dahOnFlag) {
        if (ditPaddleFlag==0) dahPaddleFlag=1;
        else if (ditPaddleFlag==1) dahPaddleFlag=2;
      }
      if (ditState==LOW && ditPaddleFlag==0 && !ditOnFlag) {
        if (dahPaddleFlag==0) ditPaddleFlag=1;
        else if (dahPaddleFlag==1) ditPaddleFlag=2;
      }
      if (dahState==HIGH && ditState==HIGH &&
          (dahPaddleFlag==1 || ditPaddleFlag==1)) {
        dahPaddleFlag=0; ditPaddleFlag=0;
      } else if ((dahPaddleFlag==1||ditPaddleFlag==1) && !dahOnFlag && !ditOnFlag) {
        if (dahPaddleFlag==1) {
          paddle_beginDAH(); dahPaddleFlag=0;
          if (ditPaddleFlag==2) ditPaddleFlag=1;
        } else {
          paddle_beginDIT(); ditPaddleFlag=0;
          if (dahPaddleFlag==2) dahPaddleFlag=1;
        }
      }
      paddleTimer();
      break;

    case IAMBIC_B:
      if (dahState==LOW && dahPaddleFlag==0 && !dahOnFlag) {
        if (ditPaddleFlag==0) dahPaddleFlag=1;
        else if (ditPaddleFlag==1) dahPaddleFlag=2;
      }
      if (ditState==LOW && ditPaddleFlag==0 && !ditOnFlag) {
        if (dahPaddleFlag==0) ditPaddleFlag=1;
        else if (dahPaddleFlag==1) ditPaddleFlag=2;
      }
      if ((dahPaddleFlag==1||ditPaddleFlag==1) && !dahOnFlag && !ditOnFlag) {
        if (dahPaddleFlag==1) {
          paddle_beginDAH(); dahPaddleFlag=0;
          if (ditPaddleFlag==2) ditPaddleFlag=1;
        } else {
          paddle_beginDIT(); ditPaddleFlag=0;
          if (dahPaddleFlag==2) dahPaddleFlag=1;
        }
      }
      paddleTimer();
      break;
  }
}

// ============================================================================
// SERIAL / K3NG
// ============================================================================
// ============================================================================
// SERIAL / K3NG - versione completa v58
// ============================================================================

// Legge parametro numerico da stringa (es. "15" da "\S15")
static int serialParamInt(const String& s) { return s.toInt(); }

void checkSerial() {
 // Prima leggi da BLE (se abilitato)
  checkSerialBle();
  
  // Switcha tra K3NG (ASCII) e Winkey (binario) in base a winkeyMode
  if (winkeyMode) {
    checkSerialWinkey();
    return;
  } 
  // ModalitÃ  K3NG (originale)

  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\\') {
      // Attende il carattere comando
      unsigned long t0 = millis();
      while (Serial.available() == 0 && millis()-t0 < 200) { delay(1); }
      if (Serial.available() == 0) break;
      char cmd = toupper(Serial.read());

      // Legge parametro opzionale (tutto fino a newline o timeout)
      String param = "";
      t0 = millis();
      while (millis()-t0 < 100) {
        if (Serial.available()) { char pc = Serial.read(); if (pc=='\r'||pc=='\n') break; param += pc; t0=millis(); }
      }
      param.trim();
      param.toUpperCase();

      switch(cmd) {

        // --- VELOCITA ---
        case 'S': {
          if (param.length() > 0) {
            int v = constrain(serialParamInt(param), MIN_WPM, MAX_WPM);
            LOCK(); wpm=v; calculateMorseTiming(); UNLOCK();
            postDisplay(DISP_WPM, v);
            savePreferences();
            Serial.printf("WPM: %d\n", v);
            sdLogWrite(String("CMD: \\S") + v);  // ← Cambia newSpeed in v
          } else {
            LOCK(); int v=wpm; UNLOCK();
            Serial.printf("WPM: %d\n", v);
          }
          break;
        }

        // --- FREQUENZA ---
        case 'F': {
          if (param.length() > 0) {
            int v = constrain(serialParamInt(param), MIN_FREQ, MAX_FREQ);
            LOCK(); audioTone=v; UNLOCK();
            postDisplay(DISP_FREQ, v);
            savePreferences();
            Serial.printf("Freq: %d Hz\n", v);
            sdLogWrite(String("CMD: \\F") + v);  // ← Cambia newFreq in v
          } else {
            LOCK(); int v=audioTone; UNLOCK();
            Serial.printf("Freq: %d Hz\n", v);
          }
          break;
        }

        // --- VOLUME (0=mute, 1-4) ---
        case 'L': {
          if (param.length() > 0) {
            int v = constrain(serialParamInt(param), 0, 4);
            LOCK();
            volumeLevel = v;
            isMuted = (v==0);
            switch(v) {
              case 1: toneVol=toneVol4; break;
              case 2: toneVol=toneVol3; break;
              case 3: toneVol=toneVol2; break;
              case 4: toneVol=toneVol1; break;
              default: break;
            }
            UNLOCK();
            postDisplay(DISP_VOL, v);
            savePreferences();
            Serial.printf("Vol: %d\n", v);
          } else {
            LOCK(); int v=volumeLevel; UNLOCK();
            Serial.printf("Vol: %d\n", v);
          }
          break;
        }

        // --- WEIGHT ---
        case 'W': {
          if (param.length() > 0) {
            float v = constrain(serialParamInt(param)/100.0f, 0.8f, 1.2f);
            LOCK(); spacing_Weight=v; calculateMorseTiming(); UNLOCK();
            savePreferences();
            Serial.printf("Weight: %.2f\n", v);
          } else {
            LOCK(); float v=spacing_Weight; UNLOCK();
            Serial.printf("Weight: %.2f\n", v);
          }
          break;
        }

        // --- RATIO ---
        case 'R': {
          if (param.length() > 0) {
            float v = constrain(serialParamInt(param)/100.0f, 2.5f, 4.0f);
            LOCK(); dahRatio=v; calculateMorseTiming(); UNLOCK();
            savePreferences();
            Serial.printf("Ratio: %.2f\n", v);
          } else {
            LOCK(); float v=dahRatio; UNLOCK();
            Serial.printf("Ratio: %.2f\n", v);
          }
          break;
        }

        // --- TUNE ---
        case 'T': {
          LOCK(); isTuneMode=!isTuneMode; bool t=isTuneMode; UNLOCK();
          postDisplay(DISP_TUNE, t?1:0);
          Serial.println(t ? "Tune ON" : "Tune OFF");
          sdLogWrite("CMD: \\T (TUNE)");
          break;
        }

        case 'J': {
        touchCalReset();
        Serial.println("\\J - Touch cal reset. Riavvio...");
        delay(500);
        ESP.restart();
        break;
        }


        // --- INFO / STATUS ---
        case 'I': sendStatus(); break;

        // --- IAMBIC A ---
        case 'A': {
          LOCK(); currentMode=IAMBIC_A; UNLOCK();
          postDisplay(DISP_IAMBIC_MODE,(int32_t)IAMBIC_A);
          Serial.println("Mode: Iambic A");
          break;
        }

        // --- IAMBIC B ---
        case 'B': {
          LOCK(); currentMode=IAMBIC_B; UNLOCK();
          postDisplay(DISP_IAMBIC_MODE,(int32_t)IAMBIC_B);
          Serial.println("Mode: Iambic B");
          break;
        }

        // --- MANUAL KEY ---
        case 'K': {
          LOCK(); currentMode=MANUAL; UNLOCK();
          postDisplay(DISP_IAMBIC_MODE,(int32_t)MANUAL);
          Serial.println("Mode: Manual");
          break;
        }

        // --- PADDLE REVERSE (\N0=normal, \N1=reversed) ---
        case 'N': {
          if (param.length() > 0) {
            bool rev = (serialParamInt(param) == 1);
            LOCK(); paddleReversed=rev; UNLOCK();
            savePreferences();
            Serial.printf("Paddle: %s\n", rev ? "REVERSED" : "NORMAL");
          } else {
            LOCK(); bool rev=paddleReversed; UNLOCK();
            Serial.printf("Paddle: %s\n", rev ? "REVERSED" : "NORMAL");
          }
          break;
        }

        // --- CLEAR BUFFER ---
        case 'C': {
          clearBuffer();
          postDisplay(DISP_CLEAR);
          Serial.println("Buffer cleared");
          sdLogWrite("CMD: \\C (CLEAR)");
          break;
        }

        // --- QUERY BUFFER ---
        case 'U': {
          LOCK(); int len=buffer.length(); UNLOCK();
          Serial.printf("Buffer chars: %d\n", len);
          break;
        }

        case 'Q': {  // QRSS mode
          if (param.length() > 0) {
            int mode = constrain(serialParamInt(param), 0, 30);
            // Valida: 0=OFF, 3=QRSS3, 6=QRSS6, 10=QRSS10, 30=QRSS30
            if (mode == 0) {
              qrssMode = QRSS_OFF;
            } else if (mode == 3) {
              qrssMode = QRSS_3;
            } else if (mode == 6) {
              qrssMode = QRSS_6;
            } else if (mode == 10) {
              qrssMode = QRSS_10;
            } else if (mode == 30) {
              qrssMode = QRSS_30;
            } else {
              Serial.println("Invalid QRSS mode (use 0, 3, 6, 10, or 30)");
              break;
            }
            
            // Applica timing
            if (qrssMode != QRSS_OFF) {
              LOCK();
              if (wpm > 60) wpm = 20;  // Reset HSCW
              qrssDitMs = (float)qrssMode * 1000.0f;
              calculateMorseTiming();
              UNLOCK();
              Serial.printf("QRSS%d enabled\n", (int)qrssMode);
              sdLogWrite(String("CMD: \\Q") + (int)qrssMode);
            } else {
              LOCK(); calculateMorseTiming(); UNLOCK();
              Serial.println("QRSS disabled");
              sdLogWrite("CMD: \\Q0 (QRSS off)");
            }
            
            updateQrssButton();
          } else {
            // Query status
            if (qrssMode == QRSS_OFF) {
              Serial.println("QRSS: OFF");
            } else {
              Serial.printf("QRSS%d: dit=%.0f ms (%.3f WPM)\n", 
                           (int)qrssMode, qrssDitMs, 1200.0f / qrssDitMs);
            }
          }
          break;
        }



        // --- PROGRAM MEMORY (\PA<testo> = salva testo in memoria A) ---
        case 'P': {
          if (param.length() >= 1) {
            char letter = param.charAt(0);
            if (letter >= 'A' && letter <= 'Z') {
              String msg = param.substring(1);
              msg.trim();
              if (msg.length() == 0) {
                // Query: nessun testo → mostra contenuto senza toccare memoria
                Serial.printf("Memory %c = [%s]\n", letter, k3ngMem[letter-'A'].c_str());
              } else {
                // Set: salva testo
                int idx = letter - 'A';
                k3ngMem[idx] = msg;
                saveMemory(idx);
                Serial.printf("Memory %c saved: [%s]\n", letter, msg.c_str());
              }
            } else {
              Serial.println("ERR: use A-Z");
            }
          } else {
            // \PL — lista tutte le memorie non vuote
            Serial.println("\n=== Memories ===");
            bool any = false;
            for (int i = 0; i < NUM_MEM; i++) {
              if (k3ngMem[i].length() > 0) {
                Serial.printf("  %c: [%s]\n", 'A' + i, k3ngMem[i].c_str());
                any = true;
              }
            }
            if (!any) Serial.println("  (all empty)");
            Serial.println("================\n");
          }
          break;
        }

        // --- SEND FROM MEMORY (\MA = invia memoria A) ---
        case 'M': {
          if (param.length() >= 1) {
            char letter = param.charAt(0);
            if (letter >= 'A' && letter <= 'Z') {
              String msg = k3ngMem[letter-'A'];
              if (msg.length() > 0) {
                LOCK(); buffer += msg; UNLOCK();
                Serial.printf("Sending memory %c: %s\n", letter, msg.c_str());
                sdLogWrite(String("CMD: \\M") + param + " = " + msg);
              } else {
                Serial.printf("Memory %c empty\n", letter);
              }
            }
          }
          break;
        }

        // --- DISPLAY SETTINGS COMPLETO ---
        case 'D': sendStatus(); break;

        // --- VERSION ---
        case 'V': {
        String s = "";
        s += "=== " + TITLE + " ===\n";
        s += VERSION + "\n";
        s += "HW: ESP32-CYD ILI9341\n";
        s += "Protocol: K3NG+Winkey\n";
        s += "Author: IW1RMM\n";
        bleSendLong(s);
        break;
      }
          //         - aggiungere nel case switch di checkSerial()
// ============================================================================

// Inserire questo case dopo gli altri comandi in checkSerial():

        // --- FARNSWORTH WPM (\\Ynn = set effective WPM, \\Y0 = disable) ---
        case 'Y': {
          if (param.length() > 0) {
            int effectiveWPM = constrain(serialParamInt(param), 0, MAX_WPM);
            if (effectiveWPM == 0) {
              // Disabilita Farnsworth
              LOCK(); farnsworthEnabled = false; calculateMorseTiming(); UNLOCK();
              Serial.println("Farnsworth DISABLED");
              
              // Aggiorna UI se esiste
              if (btnFarnsworth) {
                lv_obj_t *label = lv_obj_get_child(btnFarnsworth, 0);
                lv_label_set_text(label, "FARNSW: OFF");
                lv_obj_set_style_bg_color(btnFarnsworth, lv_color_hex(0x555555), 0);
              }
            } else {
              // Abilita Farnsworth
              LOCK();
              int currentWPM = wpm;
              if (effectiveWPM >= currentWPM) {
                // Effective WPM deve essere < character WPM
                effectiveWPM = currentWPM - 1;
              }
              farnsworthWPM = effectiveWPM;
              farnsworthEnabled = true;
              calculateMorseTiming();
              UNLOCK();
              
              Serial.printf("Farnsworth ENABLED: effective=%d WPM, chars=%d WPM\n", effectiveWPM, currentWPM);
              
              // Aggiorna UI se esiste
              if (btnFarnsworth) {
                lv_obj_t *label = lv_obj_get_child(btnFarnsworth, 0);
                char buf[32];
                sprintf(buf, "FARNSW: %d", effectiveWPM);
                lv_label_set_text(label, buf);
                lv_obj_set_style_bg_color(btnFarnsworth, lv_color_hex(0x00AA00), 0);
              }
            }
          } else {
            // Query current status
            LOCK();
            bool enabled = farnsworthEnabled;
            int eff = farnsworthWPM;
            int ch = wpm;
            UNLOCK();
            if (enabled) {
              Serial.printf("Farnsworth: ON - effective=%d WPM, chars=%d WPM\n", eff, ch);
            } else {
              Serial.println("Farnsworth: OFF");
            }
          }
          break;
        }

// --- AUTO MODE (\O) ---
        case 'O': {
          LOCK(); currentMode = AUTO_MODE; UNLOCK();
          postDisplay(DISP_IAMBIC_MODE, (int32_t)AUTO_MODE);
          Serial.println("Mode: Auto");
          sdLogWrite("CMD: \\O (AUTO)");
          break;
        }

        // --- TX ENABLE/DISABLE (\X) ---
        case 'X': {
          LOCK(); txEnabled = !txEnabled; bool tx = txEnabled; UNLOCK();
          Serial.printf("TX: %s\n", tx ? "ENABLED" : "DISABLED (sidetone only)");
          sdLogWrite(tx ? "CMD: \\X (TX ON)" : "CMD: \\X (TX OFF)");
          break;
        }

        // --- PADDLE ECHO (\E) ---
        case 'E': {
          LOCK(); echoEnabled = !echoEnabled; bool ec = echoEnabled; UNLOCK();
          Serial.printf("Echo: %s\n", ec ? "ON" : "OFF");
          break;
        }

        // --- SET GMT OFFSET (\Gn) ---
        case 'G': {
          if (param.length() > 0) {
            gmtOffsetHours = param.toInt();
            preferences.begin("cyd-morse", false);
            preferences.putLong("gmt_off", gmtOffsetHours);
            preferences.end();
            Serial.printf("GMT offset: %+d\n", (int)gmtOffsetHours);
          } else {
            Serial.printf("GMT offset: %+d\n", (int)gmtOffsetHours);
          }
          break;
        }

        // --- SET DATETIME (\Z GGMMAAAAHHMMSS — 14 cifre) ---
        case 'Z': {
          if (param.length() == 14) {
            int gg = param.substring(0,2).toInt();
            int mm = param.substring(2,4).toInt();
            int aa = param.substring(4,8).toInt();
            int hh = param.substring(8,10).toInt();
            int mi = param.substring(10,12).toInt();
            int ss = param.substring(12,14).toInt();
            struct tm t = {0};
            t.tm_year = aa - 1900; t.tm_mon = mm - 1; t.tm_mday = gg;
            t.tm_hour = hh; t.tm_min = mi; t.tm_sec = ss; t.tm_isdst = -1;
            time_t epoch = mktime(&t);
            struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            char buf[50];
            snprintf(buf, sizeof(buf), "DateTime: %02d/%02d/%04d %02d:%02d:%02d", gg, mm, aa, hh, mi, ss);
            Serial.println(buf);
            sdLogWrite(buf);
          } else {
            Serial.println("ERR: use \\Z GGMMAAAAHHMMSS (14 digits)");
          }
          break;
        }







        // --- HELP ---
        case 'H': sendHelp(); break;

        default: {
          Serial.print("Unknown: \\");
          Serial.println(cmd);
          break;
        }
      }
    } else {
      // Testo libero -> aggiunge al buffer morse
      if (c != '\r' && c != '\n') {
        LOCK(); buffer += String(c); UNLOCK();
      }
    }
  }
}

void sendStatus() {
  LOCK();
  int    v_wpm     = wpm;
  int    v_freq    = audioTone;
  int    v_vol     = volumeLevel;
  bool   v_mute    = isMuted;
  float  v_ratio   = dahRatio;
  float  v_weight  = spacing_Weight;
  bool   v_padrev  = paddleReversed;
  String v_buf     = buffer;
  bool   v_fw      = farnsworthEnabled;
  int    v_fwwpm   = farnsworthWPM;
  QrssMode v_qrss  = qrssMode;
  bool   v_tx      = txEnabled;
  bool   v_echo    = echoEnabled;
  UNLOCK();

  char buf[512];
  snprintf(buf, sizeof(buf),
    "\n=== Status ===\n"
    "WPM:      %d\n"
    "Freq:     %d Hz\n"
    "Vol:      %d%s\n"
    "Ratio:    %.2f\n"
    "Weight:   %.2f\n"
    "Paddle:   %s\n"
    "Mode:     %s\n"
    "Farnswth: %s\n"
    "QRSS:     %s\n"
    "TX out:   %s\n"
    "Echo:     %s\n"
    "Buffer:   [%s]\n"
    "==============\n",
    v_wpm,
    v_freq,
    v_mute ? 0 : volumeUiLevel(v_vol), v_mute ? " (MUTE)" : "",
    v_ratio,
    v_weight,
    v_padrev ? "REVERSED" : "NORMAL",
    getModeName().c_str(),
    v_fw ? ("ON eff=" + String(v_fwwpm) + " WPM").c_str() : "OFF",
    v_qrss == QRSS_OFF ? "OFF" : ("QRSS" + String((int)v_qrss)).c_str(),
    v_tx   ? "ENABLED" : "DISABLED",
    v_echo ? "ON" : "OFF",
    v_buf.c_str()
  );
  Serial.print(buf);
  Serial.flush();   // attende che il buffer TX sia svuotato prima di continuare
}


void sendHelp() {
  Serial.println("\n=== K3NG Commands ===");
  Serial.println("--- Speed/Audio ---");
  Serial.println("\\Snn   - Set/Get WPM (5-60)");
  Serial.println("\\Ynn   - Farnsworth effective WPM (0=off)");
  Serial.println("\\Fnnn  - Set/Get Freq Hz (300-1200)");
  Serial.println("\\Ln    - Set/Get Vol (0=mute, 1-4)");
  Serial.println("\\Wnn   - Set/Get Weight (80-120)");
  Serial.println("\\Rnnn  - Set/Get Ratio (250-400)");
  Serial.println("--- Mode ---");
  Serial.println("\\A     - Iambic A");
  Serial.println("\\B     - Iambic B");
  Serial.println("\\K     - Manual key");
  Serial.println("\\N0/1  - Paddle normal/reversed");
  Serial.println("--- Memory ---");
  Serial.println("\\PA<txt> - Save memory A (A-Z)");
  Serial.println("\\PA     - Query memory A (no text = read)");
  Serial.println("\\P      - List all memories (non-empty)");
  Serial.println("\\MA     - Send memory A (A-Z)");
  Serial.println("--- Control ---");
  Serial.println("\\T     - Tune toggle");
  Serial.println("\\C     - Clear buffer");
  Serial.println("\\O     - Auto mode");
  Serial.println("\\X     - TX enable/disable toggle");
  Serial.println("\\E     - Paddle echo toggle");
  Serial.println("\\Q     - QRSS");
  Serial.println("\\U     - Query buffer length");
  Serial.println("\\G+n   - Set GMT offset (es. \\G1 \\G-5)");
  Serial.println("\\Z     - Set datetime (14 digits: GGMMAAAAHHMMSS)");
  Serial.println("--- Info ---");
  Serial.println("\\I     - Status");
  Serial.println("\\V     - Version");
  Serial.println("\\H     - This help");
  Serial.println("--- Text ---");
  Serial.println("chars  - Send as morse");
  Serial.println("\\J     - ***Touch Calibration***");
  Serial.println("=====================\n");
}

/*
void sendHelp() {
  Serial.println("\n=== K3NG Commands ===");
  Serial.println("\\T - Tune  \\I - Info  \\A - IambicA");
  Serial.println("\\B - IambicB  \\C - Clear  \\H - Help");
  Serial.println("Text chars - send as morse");
  Serial.println("=====================\n");
}
*/
// ============================================================================
// WINKEY WK3 PROTOCOL - Implementation
// ============================================================================

// ---------------------------------------------------------------------------
// winkeyBuildStatus() - costruisce il status byte WK3-compliant
//   Tag MSB = 110 -> base 0xC0
//   Bit 4: WAIT (buffer > 2/3 pieno)
//   Bit 3: 0 (WK2 status identifier, non pushbutton)
//   Bit 2: BUSY (buffer non vuoto o key down)
//   Bit 1: BREAKIN (paddle attivo)
//   Bit 0: XOFF (buffer > 2/3 pieno)
// ---------------------------------------------------------------------------
static uint8_t winkeyBuildStatus() {
  uint8_t status = 0xC0;  // tag 110 in bit 7,6,5
  LOCK();
  bool bufferFull = (buffer.length() > 106);  // >2/3 di 160
  bool keyDown    = (ditOnFlag || dahOnFlag);
  bool busy       = (buffer.length() > 0) || keyDown;
  bool breakin    = (ditOnFlag || dahOnFlag);
  UNLOCK();
  if (bufferFull) status |= 0x10;  // bit 4 WAIT
  // bit 3 = 0: identifica WK status byte (non pushbutton)
  if (busy)       status |= 0x04;  // bit 2 BUSY
  if (breakin)    status |= 0x02;  // bit 1 BREAKIN
  if (bufferFull) status |= 0x01;  // bit 0 XOFF
  return status;
}

// ---------------------------------------------------------------------------
// winkeyUpdateStatus() - invia status se cambiato o ogni 500ms
// ---------------------------------------------------------------------------
void winkeyUpdateStatus() {
  uint8_t status = winkeyBuildStatus();
  unsigned long now = millis();
  if (status != winkeyStatusByte || (now - winkeyLastStatus > 500)) {
    Serial.write(status);
    winkeyStatusByte = status;
    winkeyLastStatus = now;
  }
}

// ---------------------------------------------------------------------------
// winkeyApplyModeReg() - applica il mode register ai parametri interni
//   WK3 Table 12:
//   Bit 7: Disable paddle watchdog
//   Bit 6: Paddle echoback
//   Bit 5,4: Key mode: 00=IambicB, 01=IambicA, 10=Ultimatic, 11=Bug
//   Bit 3: Paddle swap
//   Bit 2: Serial echoback
//   Bit 1: Autospace
//   Bit 0: CT spacing
// ---------------------------------------------------------------------------
static void winkeyApplyModeReg(uint8_t reg) {
  winkeyModeReg = reg;
  uint8_t keyerMode = (reg >> 4) & 0x03;  // bit 5,4
  LOCK();
  switch (keyerMode) {
    case 0: currentMode = IAMBIC_B; break;
    case 1: currentMode = IAMBIC_A; break;
    default: currentMode = MANUAL;  break;  // Ultimatic/Bug -> MANUAL
  }
  paddleReversed = (reg & 0x08) != 0;  // bit 3
  UNLOCK();
  postDisplay(DISP_IAMBIC_MODE, (int32_t)currentMode);
}

// ---------------------------------------------------------------------------
// winkeyApplySidetone() - formula WK3: freq = 62500/nn
//   bit 7 = paddle only sidetone (ignorato, nessun HW dedicato)
//   Tabella fissa WK1/WK2 per val 1-10 (spec Table 7)
// ---------------------------------------------------------------------------
static void winkeyApplySidetone(uint8_t nn) {
  uint8_t val = nn & 0x7F;  // strip paddle-only bit
  int freq = 600;
  if (val == 0) {
    freq = MIN_FREQ;
  } else {
    switch (val) {
      case 1: freq = 4000; break;
      case 2: freq = 2000; break;
      case 3: freq = 1333; break;
      case 4: freq = 1000; break;
      case 5: freq =  800; break;
      case 6: freq =  666; break;
      case 7: freq =  571; break;
      case 8: freq =  500; break;
      case 9: freq =  444; break;
      case 10: freq = 400; break;
      default:
        // WK3 mode continuo: 62500/val
        freq = (int)(62500 / val);
        break;
    }
  }
  freq = constrain(freq, MIN_FREQ, MAX_FREQ);
  LOCK(); audioTone = freq; UNLOCK();
  postDisplay(DISP_FREQ, freq);
}

// ---------------------------------------------------------------------------
// winkeyHandleAdmin() - comandi Admin <00><subcmd>
// ---------------------------------------------------------------------------
void winkeyHandleAdmin(uint8_t subcmd) {
  switch (subcmd) {
    case 0x00:  // Calibrate - ignorato in WK2/WK3
      break;
    case 0x01:  // Reset - non inviato durante init normale
      break;
    case 0x02:  // Host Open -> risponde con version byte SOLO (nessuna stringa debug!)
      winkeyHostOpen = true;
      Serial.write(winkeyVersion);  // 0x23 = WK2 rev 2.3
      winkeyLastStatus = millis();
      break;
    case 0x03:  // Host Close
      winkeyHostOpen = false;
      break;
    case 0x04:  // Echo test - gestito nel parser
      break;
    case 0x05:  // Paddle A2D - non supportato WK3, restituisce 0
      Serial.write((uint8_t)0x00);
      break;
    case 0x06:  // Speed A2D - non supportato WK3, restituisce 0
      Serial.write((uint8_t)0x00);
      break;
    case 0x07:  // Get Values - non supportato WK3, restituisce 0
      Serial.write((uint8_t)0x00);
      break;
    case 0x09:  // Get FW Major Rev
      Serial.write((uint8_t)0x02);  // WK2
      break;
    case 0x0A:  // Set WK1 Mode - disabilita pushbutton reporting
      break;
    case 0x0B:  // Set WK2 Mode - abilita pushbutton reporting
      break;
    case 0x0F:  // Load X1MODE con parametro extra - gestito nel parser
      break;
    case 0x17:  // Get WK3 version (legacy)
      Serial.write((uint8_t)0x31);
      break;
    case 0x18:  // Get Calibration (WK3) — risponde con valore calibrazione (default 0)
      Serial.write((uint8_t)0x00);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// wkParamCount[] - numero parametri per comando 0x00-0x1F
//   CORREZIONI v70 vs v69:
//   0x04 PTT Lead/Tail: 2 (era 1)
//   0x05 Speed Pot Setup: 3 (era 1)
//   0x0F Load Defaults: 15 (era 0) - CORREZIONE PRINCIPALE
// ---------------------------------------------------------------------------
static const uint8_t wkParamCount[32] = {
  1,  // 0x00 Admin (subcmd)
  1,  // 0x01 Sidetone
  1,  // 0x02 Set WPM
  1,  // 0x03 Set Weighting
  2,  // 0x04 PTT Lead/Tail [CORRETTO: era 1]
  3,  // 0x05 Setup Speed Pot [CORRETTO: era 1]
  1,  // 0x06 Set Pause State
  0,  // 0x07 Get Speed Pot -> risponde subito
  0,  // 0x08 Backspace
  1,  // 0x09 Set PinConfig
  0,  // 0x0A Clear Buffer
  1,  // 0x0B Key Immediate
  1,  // 0x0C Set HSCW
  1,  // 0x0D Set Farnsworth WPM
  1,  // 0x0E Set WinKeyer Mode
  15, // 0x0F Load Defaults [CORRETTO: era 0]
  1,  // 0x10 Set 1st Extension
  1,  // 0x11 Set KeyComp
  1,  // 0x12 Set Paddle Switchpoint
  0,  // 0x13 Null Command (NOP)
  1,  // 0x14 Software Paddle
  0,  // 0x15 Request Status
  1,  // 0x16 Pointer Command
  1,  // 0x17 Set Dit/Dah Ratio
  1,  // 0x18 PTT On/Off (buffered)
  1,  // 0x19 Key Buffered
  1,  // 0x1A Wait for nn seconds
  2,  // 0x1B Merge Letters
  1,  // 0x1C Buffered Speed Change
  1,  // 0x1D HSCW Speed Change (buffered)
  0,  // 0x1E Cancel Buffered Speed Change
  0   // 0x1F Buffered NOP
};

// ---------------------------------------------------------------------------
// winkeyExecCmd() - esegue comando con parametri gia' ricevuti
// ---------------------------------------------------------------------------
void winkeyExecCmd(uint8_t cmd, uint8_t* p, uint8_t n) {
  switch (cmd) {

    case 0x00:  // Admin
      winkeyHandleAdmin(p[0]);
      break;

    case 0x01:  // Sidetone frequency
      winkeyApplySidetone(p[0]);
      break;

    case 0x02:  // Set WPM
    {
      int newSpeed = constrain((int)p[0], MIN_WPM, MAX_WPM);
      LOCK(); wpm = newSpeed; calculateMorseTiming(); UNLOCK();
      postDisplay(DISP_WPM, newSpeed);
      break;
    }

    case 0x03:  // Set Weighting (10-90, 50=normal)
    {
      uint8_t w = constrain(p[0], 10, 90);
      float newWeight = w / 50.0f;
      LOCK(); spacing_Weight = newWeight; calculateMorseTiming(); UNLOCK();
      break;
    }

    case 0x04:  // PTT Lead/Tail (2 bytes: lead x10ms, tail x10ms)
      wkPttLead     = p[0];
      wkPttTailTime = p[1];
      break;

    case 0x05:  // Setup Speed Pot (3 bytes: minWPM, range, unused)
      // Nessun pot fisico: salviamo i valori per coerenza
      wkSpeedPotMin   = p[0];
      wkSpeedPotRange = p[1];
      // p[2] ignorato (backward compat)
      break;

    case 0x06:  // Set Pause State (non implementato)
      break;

    case 0x07:  // Get Speed Pot
      // Nessun pot fisico: risponde con 0x80|0 (tag corretto, valore=0)
      Serial.write((uint8_t)0x80);
      break;

    case 0x08:  // Backspace
      LOCK();
      if (buffer.length() > 0) buffer.remove(buffer.length() - 1);
      UNLOCK();
      break;

    case 0x09:  // Set PinConfig
      wkPinCfg = p[0];
      break;

    case 0x0A:  // Clear Buffer
      clearBuffer();
      postDisplay(DISP_CLEAR);
      break;

    case 0x0B:  // Key Immediate (tune)
      if (p[0] == 1) {
        LOCK(); isTuneMode = true;  UNLOCK();
        postDisplay(DISP_TUNE, 1);
      } else {
        LOCK(); isTuneMode = false; UNLOCK();
        postDisplay(DISP_TUNE, 0);
      }
      break;

    case 0x0C:  // Set HSCW WPM (60-100)
    {
      int newSpeed = constrain((int)p[0], 60, MAX_WPM);
      LOCK(); wpm = newSpeed; calculateMorseTiming(); UNLOCK();
      postDisplay(DISP_WPM, newSpeed);
      break;
    }

    case 0x0D:  // Set Farnsworth WPM
    {
      LOCK();
      if (p[0] == 0) {
        farnsworthEnabled = false;
        calculateMorseTiming();
      } else {
        farnsworthWPM = constrain((int)p[0], 5, wpm - 1);
        farnsworthEnabled = true;
        calculateMorseTiming();
      }
      UNLOCK();
      // Aggiorna UI via queue (thread-safe) invece di chiamate LVGL dirette
      postDisplay(DISP_FARNSWORTH, farnsworthEnabled ? (int32_t)farnsworthWPM : 0);
      break;
    }

    case 0x0E:  // Set WinKeyer Mode register
      winkeyApplyModeReg(p[0]);
      break;

    // --- 0x0F Load Defaults (15 parametri, ordine WK3 spec Table 13) ---
    // 1)Mode 2)WPM 3)Sidetone 4)Weight 5)LeadIn 6)Tail
    // 7)MinWPM 8)WPMRange 9)X2Mode 10)KeyComp 11)Farns
    // 12)PaddleSP 13)Ratio 14)PinCfg 15)X1Mode
    case 0x0F:
    {
      winkeyApplyModeReg(p[0]);                         // 1) Mode
      {
        int newSpeed = constrain((int)p[1], MIN_WPM, MAX_WPM);
        LOCK(); wpm = newSpeed; calculateMorseTiming(); UNLOCK();
        postDisplay(DISP_WPM, newSpeed);
      }                                                  // 2) WPM
      winkeyApplySidetone(p[2]);                        // 3) Sidetone
      {
        uint8_t w = constrain(p[3], 10, 90);
        LOCK(); spacing_Weight = w / 50.0f; calculateMorseTiming(); UNLOCK();
      }                                                  // 4) Weight
      wkPttLead       = p[4];                           // 5) LeadIn
      wkPttTailTime   = p[5];                           // 6) Tail
      wkSpeedPotMin   = p[6];                           // 7) MinWPM
      wkSpeedPotRange = p[7];                           // 8) WPMRange
      wkX2Mode        = p[8];                           // 9) X2Mode
      wkKeyComp       = p[9];                           // 10) KeyComp
      {
        int fwpm = (int)p[10];
        LOCK();
        if (fwpm == 0) { farnsworthEnabled = false; }
        else { farnsworthWPM = constrain(fwpm, 5, wpm-1); farnsworthEnabled = true; }
        calculateMorseTiming();
        UNLOCK();
        postDisplay(DISP_FARNSWORTH, farnsworthEnabled ? (int32_t)farnsworthWPM : 0);
      }                                                  // 11) Farnsworth
      wkPaddleSP = p[11];                               // 12) PaddleSP
      {
        uint8_t r = constrain(p[12], 33, 66);
        float newRatio = 3.0f * (r / 50.0f);
        LOCK(); dahRatio = newRatio; calculateMorseTiming(); UNLOCK();
      }                                                  // 13) Ratio
      wkPinCfg = p[13];                                 // 14) PinCfg
      wkX1Mode = p[14];                                 // 15) X1Mode
      break;
    }

    case 0x10:  // Set 1st Extension
      wkFirstExtension = p[0];
      break;

    case 0x11:  // Set Key Compensation
      wkKeyComp = p[0];
      break;

    case 0x12:  // Set Paddle Switchpoint
      wkPaddleSP = p[0];
      break;

    case 0x13:  // Null / NOP
      break;

    case 0x14:  // Software Paddle (non implementato)
      break;

    case 0x15:  // Request WinKeyer Status
      winkeyUpdateStatus();
      break;

    case 0x16:  // Pointer Command (non implementato)
      break;

    case 0x17:  // Set Dit/Dah Ratio (33-66, 50=normale 1:3)
    {
      uint8_t r = constrain(p[0], 33, 66);
      float newRatio = 3.0f * (r / 50.0f);
      LOCK(); dahRatio = newRatio; calculateMorseTiming(); UNLOCK();
      break;
    }

    case 0x18:  // PTT On/Off buffered (non implementato)
      break;

    case 0x19:  // Key Buffered (non implementato)
      break;

    case 0x1A:  // Wait for nn seconds buffered
      wkPttTail = p[0];
      break;

    case 0x1B:  // Merge Letters (prosign) - non implementato
      break;

    case 0x1C:  // Buffered Speed Change
    {
      int newSpeed = constrain((int)p[0], MIN_WPM, MAX_WPM);
      LOCK(); wpm = newSpeed; calculateMorseTiming(); UNLOCK();
      postDisplay(DISP_WPM, newSpeed);
      break;
    }

    case 0x1D:  // HSCW Speed Change buffered (60-100 WPM)
    {
      int newSpeed = constrain((int)p[0], 60, MAX_WPM);
      LOCK(); wpm = newSpeed; calculateMorseTiming(); UNLOCK();
      postDisplay(DISP_WPM, newSpeed);
      break;
    }

    case 0x1E:  // Cancel Buffered Speed Change
      break;

    case 0x1F:  // Buffered NOP
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// checkSerialWinkey() - parser a macchina a stati WK3-compliant
// ---------------------------------------------------------------------------
void checkSerialWinkey() {
  static uint8_t  wkCmd       = 0xFF;
  static uint8_t  wkParams[16];   // max 15 per Load Defaults
  static uint8_t  wkParamIdx  = 0;
  static uint8_t  wkParamNeed = 0;

  // ---- Fase PRE-OPEN: accetta solo Admin <00><xx> ----
  if (!winkeyHostOpen) {
    static uint8_t wkPre = 0xFF;
    while (Serial.available()) {
      uint8_t b = Serial.read();
      if (b == 0x13) { wkPre = 0xFF; continue; }  // NOP
      if (wkPre == 0x00) {
        if (b == 0x02) {
          winkeyHandleAdmin(0x02);  // Host Open
          wkPre = 0xFF;
          return;
        }
        if (b == 0x04) {
          // Echo test pre-open
          unsigned long t = millis();
          while (!Serial.available() && millis() - t < 200);
          if (Serial.available()) {
            uint8_t echo = Serial.read();
            delay(10);
            Serial.write(echo);
          }
          wkPre = 0xFF;
          return;
        }
        wkPre = 0xFF;
        continue;
      }
      wkPre = b;
    }
    return;
  }

  // ---- Fase POST-OPEN: parser completo ----
  while (Serial.available()) {
    uint8_t b = Serial.read();

    if (wkCmd == 0xFF) {
      // Byte ASCII -> testo CW da trasmettere (0x20-0x7E)
      if (b >= 0x20 && b <= 0x7E) {
        LOCK(); buffer += (char)b; UNLOCK();
        continue;
      }
      // Byte >= 0x80 -> prosign/special (WK legacy encoding)
      if (b >= 0x80) {
        switch (b) {
          case 0x80: LOCK(); buffer += " ";       UNLOCK(); break;
          case 0x81: LOCK(); buffer += "  ";      UNLOCK(); break;
          case 0x82: LOCK(); buffer += "+";       UNLOCK(); break;
          case 0x83: LOCK(); buffer += "=";       UNLOCK(); break;
          case 0x84: LOCK(); buffer += "...---";  UNLOCK(); break;
          case 0x85: LOCK(); buffer += "(";       UNLOCK(); break;
          default: break;
        }
        continue;
      }
      // Byte comando (0x00-0x1F)
      wkCmd       = b;
      wkParamIdx  = 0;
      wkParamNeed = wkParamCount[b];
      if (wkParamNeed == 0) {
        winkeyExecCmd(wkCmd, wkParams, 0);
        wkCmd = 0xFF;
      }
      continue;
    }

    // Gestione speciale Admin Echo <00><04><byte>
    if (wkCmd == 0x00 && wkParamIdx == 0 && b == 0x04) {
      wkParams[0] = b;
      unsigned long t = millis();
      while (!Serial.available() && millis() - t < 200);
      if (Serial.available()) {
        uint8_t echo = Serial.read();
        Serial.write(echo);
      }
      wkCmd = 0xFF;
      continue;
    }

    // Raccolta parametri normale
    if (wkParamIdx < wkParamNeed) {
      wkParams[wkParamIdx++] = b;
    }
    if (wkParamIdx >= wkParamNeed) {
      winkeyExecCmd(wkCmd, wkParams, wkParamNeed);
      wkCmd = 0xFF;
    }
  }

  // Invio periodico status (ogni 500ms)
  if (millis() - winkeyLastStatus > 500) {
    winkeyUpdateStatus();
  }
}



// ============================================================================
// TASKS
// ============================================================================
void morseTask(void *parameter) {
  Serial.println("MorseTask started Core 1");
  vTaskDelay(2000 / portTICK_PERIOD_MS);  // attendi stabilizzazione pin paddle
  while (true) {
    LOCK();
    bool beacon  = beaconActive;
    bool bufEmpty = (buffer.length() == 0);
    bool tuneOn  = isTuneMode;
    UNLOCK();

    // TUNE MODE: attiva tono una sola volta, poi mantieni
    static bool tuneWasOn = false;
    if (tuneOn && !tuneWasOn) {
      // Prima volta che tune si attiva: avvia DAC
      LOCK(); bool muted=isMuted; dac_cosine_atten_t vol=toneVol; int freq=audioTone; UNLOCK();
      digitalWrite(MORSE_OUT, HIGH);  // PTT ON
      if (!muted) { dac1.outputCW(freq, vol); dac1.enable(); }
      postDisplay(DISP_TX_LED, 1);
      tuneWasOn = true;
    } else if (!tuneOn && tuneWasOn) {
      // Tune appena disattivato: spegni DAC
      dac1.disable();
      digitalWrite(MORSE_OUT, LOW);   // PTT OFF
      postDisplay(DISP_TX_LED, 0);
      tuneWasOn = false;
    }
    if (tuneOn) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }

    // PADDLE (solo se buffer vuoto e non beacon)
    if (bufEmpty && !beacon) {
      checkPaddle();
      printDitDah();   // decodifica e mostra carattere sul display
    }

    // BEACON
    if (beacon && bufEmpty) {
      unsigned long now=millis();
      LOCK(); unsigned long interval=beaconInterval; unsigned long last=lastBeaconTime; UNLOCK();
      if (now-last >= interval) {
        LOCK(); buffer+=cqMessage; lastBeaconTime=now; UNLOCK();
      }
    }

    // BUFFER TX
    sendFromBuffer();

    vTaskDelay(2 / portTICK_PERIOD_MS);
  }
}

// lvglTask RIMOSSO: LVGL ora gira nel loop() su Core 1 (Arduino framework)
// Questo garantisce che lv_timer_handler() giri sempre sullo stesso core
// dove setup() ha creato tutti gli oggetti LVGL.

void serialTask(void *parameter) {
  Serial.println("SerialTask started Core 0");
  while (true) {
    checkSerial();
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

// ============================================================================
// TIMER ISR
// ============================================================================
void IRAM_ATTR onMorseTimer() {
  morseTimerTick++;
}

// ============================================================================
// UTILITY
// ============================================================================
String getModeName() {
  switch(currentMode) {
    case IAMBIC_A:  return "Iambic A";
    case IAMBIC_B:  return "Iambic B";
    case MANUAL:    return "Manual";
    case AUTO_MODE: return "Auto";
    default:        return "Unknown";
  }
}

void calculateMorseTiming() {
  // Chiamata con LOCK già tenuto dall'esterno
  
  // PRIORITÀ 1: QRSS (modalità ultra-lenta)
  if (qrssMode != QRSS_OFF) {
    // QRSS: timing basato su qrssDitMs (già calcolato in secondi)
    ditLength = qrssDitMs;
    dahLength = ditLength * 3.0f;        // DAH = 3x DIT (standard morse)
    elemSpace = ditLength * 1.0f;        // Spazio elemento = 1 DIT
    charSpace = ditLength * 3.0f;        // Spazio carattere = 3 DIT
    wordSpace_ = ditLength * 7.0f;       // Spazio parola = 7 DIT
    
   // Serial.printf("Timing QRSS%d: dit=%.0f ms (%.3f WPM)\n", 
   //               (int)qrssMode, ditLength, 1200.0f / ditLength);
   // Serial.printf("  dit=%.0f dah=%.0f el=%.0f ch=%.0f wrd=%.0f ms\n",
   //               ditLength, dahLength, elemSpace, charSpace, wordSpace_);
    return;
  }
  
  // PRIORITÀ 2: FARNSWORTH (caratteri veloci, spazi lenti)
  if (farnsworthEnabled && farnsworthWPM < wpm) {
    ditLength = 1200.0f / wpm;
    dahLength = ditLength * dahRatio;
    elemSpace = ditLength * spacing_Weight;
    
    float slowDit = 1200.0f / farnsworthWPM;
    charSpace = slowDit * 3.0f * spacing_Weight;
    wordSpace_ = slowDit * 7.0f * spacing_Weight;
    
    //Serial.printf("Timing FARNSWORTH: char=%d WPM, spacing=%d WPM\n", wpm, farnsworthWPM);
    //Serial.printf("  dit=%.0f dah=%.0f el=%.0f ch=%.0f wrd=%.0f\n",
   //               ditLength, dahLength, elemSpace, charSpace, wordSpace_);
    return;
  }
  
  // PRIORITÀ 3: NORMALE o HSCW (timing standard WPM)
  ditLength  = 1200.0f / wpm;
  dahLength  = ditLength * dahRatio;
  charSpace  = ditLength * 3.0f * spacing_Weight;
  elemSpace  = ditLength * spacing_Weight;
  wordSpace_ = ditLength * 7.0f * spacing_Weight;
  
  //Serial.printf("Timing: dit=%.0f dah=%.0f el=%.0f ch=%.0f wrd=%.0f\n",
  //              ditLength, dahLength, elemSpace, charSpace, wordSpace_);
}

int calculateDitDuration() { return (int)ditLength; }

// ============================================================================
// PREFERENCES
// ============================================================================

void updateClockLVGL() {
    if (ui_LabelClock == NULL) return; 

    time_t ora = time(NULL);
    struct tm * t = localtime(&ora);
    
    char buf[64];
    // Formato con colori: Data (Azzurro), Ora (Giallo)
    sprintf(buf, "#ADD8E6 %02d/%02d#\n#FFFF00 %02d:%02d:%02d#", 
            t->tm_mday, t->tm_mon + 1, 
            t->tm_hour, t->tm_min, t->tm_sec);

    lv_label_set_text(ui_LabelClock, buf);
    lv_label_set_recolor(ui_LabelClock, true); 

    // Se sincronizzato, bordo VERDE SMERALDO, altrimenti ROSSO
    if (t->tm_year + 1900 > 1970) {
        lv_obj_set_style_border_color(ui_LabelClock, lv_color_hex(0x00FF7F), 0);
    } else {
        lv_obj_set_style_border_color(ui_LabelClock, lv_color_hex(0xFF4500), 0);
    }
}
// ============================================================================
// MAIN LOOP LVGL gira qui, su Core 1 garantito da Arduino framework
// Questo ÃƒÂ¨ l'unico posto sicuro per lv_timer_handler() e processDisplayQueue()
// perché setup() crea tutti gli oggetti LVGL su Core 1 (loopTask Arduino)
// ============================================================================
void loop() {

  // ===============================
// BLE deferred operations (SAFE)
// ===============================
if (bleUiUpdatePending) {
  bleUiUpdatePending = false;
    updateBleButton();
}

if (bleLogEnabledPending) {
  bleLogEnabledPending = false;
  sdLogWrite("BLE enabled");
}

if (bleLogDisabledPending) {
  bleLogDisabledPending = false;
  sdLogWrite("BLE disabled");
}

  processDisplayQueue();   // consuma messaggi da morseTask
  lv_timer_handler();      // LVGL ridisegna e processa touch

  // SD Player: inietta prossimo carattere nel buffer morse quando in play
  sdPlayerTask();

  // Aggiorna label freq/vol/wpm nel tab SD ogni 2 secondi
  static uint32_t lastSdInfoMs = 0;
  if (millis() - lastSdInfoMs > 2000) {
    sdUpdateInfoLabel();
    sdUpdateWpmLabel();
    lastSdInfoMs = millis(); 
  }

 if (millis() - lastClockUpdate >= 1000) {
    lastClockUpdate = millis();
    updateClockLVGL();
  }
  delay(5);                // 5ms Ã¢â‚¬â€ valore raccomandato LVGL
}
