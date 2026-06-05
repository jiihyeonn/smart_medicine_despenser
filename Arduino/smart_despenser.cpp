/*
 * 무인 스마트 알약 디스펜서 - 아두이노 UNO 입력 단말 (수정본)
 *
 * 모듈: RFID(MFRC522) + 4x4 키패드 + I2C LCD(16x2) + WiFi(ESP-01/8266)
 * 역할: iot_server에 "ARD" ID로 로그인하는 클라이언트.
 *
 * ─────────────────────────────────────────────────────────────
 * [증상 흐름]
 *   ARD -> [DOC]SYMPTOM@<id>@<증상>
 *   DOC -> [ARD]PRESCRIPTION@<약이름>@<개수>   → LCD "Dispensing..."  (배출 시작)
 *   DOC -> [ARD]PICKUP_DONE@<약이름>@<개수>    → LCD "Dispense Done"  (배출 완료)
 *   또는 FAIL_* 코드                          → LCD "Denied ..."
 *
 * [키패드 픽업 흐름]
 *   ARD -> [DOC]PICKUP@<id>
 *   DOC -> [ARD]PICKUP@<약이름>@<개수>         → LCD "Dispensing..."  (배출 시작)
 *   DOC -> [ARD]PICKUP_DONE@<약이름>@<개수>    → LCD "Dispense Done"  (배출 완료)
 *   또는 FAIL_* 코드                          → LCD "Denied ..."
 *
 * ※ ARD는 절대 STM에 직접 보내지 않음. 모든 배출명령은 DOC가 STM에 전달.
 *   LCD의 Dispensing / Done 은 DOC가 보내는 두 단계 메시지에 동기화됨.
 * ─────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>

#undef CLOSED

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "WiFiEsp.h"
#include "SoftwareSerial.h"

/* ─────────── WiFi / 서버 설정 ─────────── */
#define AP_SSID "KCCI601"
#define AP_PASS "@kcci601@"
#define SERVER_NAME "10.10.16.87"
#define SERVER_PORT 5000
#define LOGID "ARD"
#define PASSWD "PASSWD"

/* ─────────── 핀 배치 (UNO) ─────────── */
#define RFID_SS 10
#define RFID_RST 9
#define WIFI_RX 7
#define WIFI_TX 6
#define LED_BUSY A3
// SPI: D13=SCK D12=MISO D11=MOSI / I2C: A4=SDA A5=SCL

#define CMD_SIZE 64
#define ARR_CNT 6

/* ─── 전역변수 추가 ─── (06.02 추가) */ 
unsigned long showMedUntil = 0;   // 약 정보 표시 종료 시각
bool showingMed = false;          // 약 정보 표시 중 여부

char gLine[CMD_SIZE];   // 수신 라인 누적 버퍼
byte gLineLen = 0;

//bool dispDone = false;   // PICKUP_DONE 중복 처리 방지

SoftwareSerial wifiSerial(WIFI_RX, WIFI_TX);
WiFiEspClient client;
MFRC522 rfid(RFID_SS, RFID_RST);
LiquidCrystal_I2C lcd(0x27, 16, 2);

/* ─────────── 4x4 키패드 ─────────── */
const byte ROWS = 4, COLS = 4;
char keymap[ROWS][COLS] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '*', '0', '#', 'D' }
};
byte rowPins[ROWS] = { 2, 3, 4, 5 };
byte colPins[COLS] = { 8, A0, A1, A2 };
Keypad keypad = Keypad(makeKeymap(keymap), rowPins, colPins, ROWS, COLS);

/* ─────────── 상태 머신 ─────────── */
enum State { ST_IDLE, ST_INPUT_ID, ST_SYMPTOM, ST_WAIT, ST_WAIT_DISPENSE, ST_RESULT };
State state = ST_IDLE;

char userID[12] = "";
byte idLen = 0;
unsigned long waitStart = 0;       // ST_WAIT(서버 응답 대기) 시작 시각
unsigned long dispenseStart = 0;   // ST_WAIT_DISPENSE(배출 대기) 시작 시각
unsigned long resultUntil = 0;

const unsigned long WAIT_TIMEOUT     = 9000;   // 서버 응답 대기 (6000→8000)
const unsigned long DISPENSE_TIMEOUT = 30000;  // 배출 완료 대기 (신규)
const unsigned long RESULT_HOLD      = 3000;

/* 현재 배출 중인 약 정보 (Dispensing 화면 표시용) */
char dispMed[16] = "";
int  dispQty = 0;

/* ─────────── RFID UID -> UserID 매핑 ─────────── */
struct CardMap {
  const char* uid;
  const char* user;
};
CardMap cardTable[] = {
  { "3148D216", "11111" },
  { "E2B9F105", "66666" },
};
const byte cardCount = sizeof(cardTable) / sizeof(cardTable[0]);

/* ─────────── 증상 번호 -> 이름 ─────────── */
const char* symptomName(char k) {
  switch (k) {
    case '1': return "Headache";
    case '2': return "Stomachache";
    case '3': return "Indigestion";
    case '4': return "Cramps";
    case '5': return "Fever";
    case '6': return "Insomnia";
  }
  return NULL;
}

void wifiSetup();
void serverConnect();
void checkRFID();
void requestPickup();
void requestSymptom(const char* s);
void handleKey(char k);
// void socketEvent();
void pumpSerial();
void processLine(char* line);
void handleStart(char** a, int n);   // PRESCRIPTION / PICKUP : 배출 시작
void handleDone(char** a, int n);    // PICKUP_DONE          : 배출 완료
void showFail(const char* code);     // FAIL_* 코드 → LCD
void goIdle();
void showMsg(const __FlashStringHelper* l1, const __FlashStringHelper* l2);
void enterResult();
void showSymptomMenu();

/* ═══════════════════════ SETUP ═══════════════════════ */
void setup() {
  Serial.begin(115200);
  pinMode(LED_BUSY, OUTPUT);
  digitalWrite(LED_BUSY, LOW);

  Wire.begin();
  lcd.init();
  lcd.backlight();

  SPI.begin();
  rfid.PCD_Init();

  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print(F("RFID FW Version: 0x"));
  Serial.println(v, HEX);

  wifiSetup();
    // ✅ 수신 타임아웃 늘리기
    client.setTimeout(5000);  // 5초
  goIdle();
}

/* ═══════════════════════ LOOP ═══════════════════════ */
void loop() {
    // if (client.available()) socketEvent();
    pumpSerial();

    // ✅ delay() 대신 비블로킹 타이머로 "Dispensing..." 전환
    if (showingMed && millis() > showMedUntil) {
        showingMed = false;
        lcd.clear();
        lcd.print(F("Dispensing..."));
        lcd.setCursor(0, 1);
        lcd.print(dispMed);
    }

    // DISPENSE_TIMEOUT 30초로 늘림
    if (state == ST_WAIT_DISPENSE &&
        millis() - dispenseStart > DISPENSE_TIMEOUT) {
        showMsg(F("Dispense Done"), F(""));
        //showMsg(F("Timeout"), F("Check machine"));   // 타임아웃인지 정상완료인지 구분
        digitalWrite(LED_BUSY, LOW);
        enterResult();
    }

    if (state == ST_WAIT &&
        millis() - waitStart > WAIT_TIMEOUT) {
        showMsg(F("Server Timeout"), F("Try again"));
        enterResult();
    }

    if (state == ST_RESULT && millis() > resultUntil) goIdle();
    if (state == ST_IDLE || state == ST_INPUT_ID) checkRFID();

    char k = keypad.getKey();
    if (k) handleKey(k);

    static unsigned long lastTry = 0;
    if (!client.connected() && millis() - lastTry > 3000) {
        lastTry = millis();
        serverConnect();
    }
}

/* ─────────── 키패드 처리 ─────────── */
void handleKey(char k) {
  if (state == ST_IDLE || state == ST_INPUT_ID) {
    if (k >= '0' && k <= '9') {
      if (idLen < sizeof(userID) - 1) {
        userID[idLen++] = k;
        userID[idLen] = '\0';
      }
      state = ST_INPUT_ID;
      lcd.clear();
      lcd.print(F("ID:"));
      lcd.print(userID);
      lcd.setCursor(0, 1);
      lcd.print(F("# OK   * Clear"));
    } else if (k == '*') goIdle();
    else if (k == '#' && idLen > 0) requestPickup();
  } else if (state == ST_SYMPTOM) {
    const char* s = symptomName(k);
    if (s) requestSymptom(s);
    else if (k == '*') goIdle();
  }
}

/* ─────────── RFID 처리 ─────────── */
void checkRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  char uid[16] = "";
  for (byte i = 0; i < rfid.uid.size && i < 7; i++) {
    char b[3];
    sprintf(b, "%02X", rfid.uid.uidByte[i]);
    strcat(uid, b);
  }
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  Serial.print(F("Card UID: "));
  Serial.println(uid);

  const char* u = NULL;
  for (byte i = 0; i < cardCount; i++)
    if (!strcmp(uid, cardTable[i].uid)) {
      u = cardTable[i].user;
      break;
    }

  if (!u) {
    showMsg(F("Unknown Card"), F(""));
    enterResult();
    return;
  }

  strncpy(userID, u, sizeof(userID) - 1);
  userID[sizeof(userID) - 1] = '\0';
  idLen = strlen(userID);
  state = ST_SYMPTOM;
  showSymptomMenu();
}

/* ─────────── 흐름 1: 예약 픽업 요청 ─────────── */
void requestPickup() {
  // 1. 화면 업데이트를 먼저
  digitalWrite(LED_BUSY, HIGH);
  lcd.clear();
  lcd.print(F("Checking..."));
  lcd.setCursor(0, 1);
  lcd.print(userID);

  // 2. 패킷을 만들고 서버에 전송
  char buf[CMD_SIZE];
  snprintf(buf, sizeof(buf), "[DOC]PICKUP@%s\n", userID);
  Serial.print(F("send: "));
  Serial.print(buf);
  client.write(buf, strlen(buf));

  state = ST_WAIT;
  waitStart = millis();
}

/* ─────────── 흐름 2: 증상 처방 요청 ─────────── */
void requestSymptom(const char* s) {
  // 1. 화면 업데이트 먼저!
  digitalWrite(LED_BUSY, HIGH);
  lcd.clear();
  lcd.print(F("Consulting..."));
  lcd.setCursor(0, 1);
  lcd.print(s);

  // 2. 서버 전송은 마지막
  char buf[CMD_SIZE];
  snprintf(buf, sizeof(buf), "[DOC]SYMPTOM@%s@%s\n", userID, s);
  Serial.print(F("send: "));
  Serial.print(buf);
  client.write(buf, strlen(buf));

  state = ST_WAIT;
  waitStart = millis();
}

// /* ─────────── 서버 수신 처리 ─────────── */
// void socketEvent() {
//   char recvBuf[CMD_SIZE] = { 0 };
//   int len = client.readBytesUntil('\n', recvBuf, CMD_SIZE - 1);
//   if (len <= 0) return;
//   recvBuf[len] = '\0';

//   Serial.print(F("recv: "));
//   Serial.println(recvBuf);   // 원본 로그 출력

//   char* pArray[ARR_CNT] = { 0 };
//   int i = 0;
//   char* p = strtok(recvBuf, "[@]");
//   while (p && i < ARR_CNT) {
//     pArray[i++] = p;
//     p = strtok(NULL, "[@]");
//   }

//   if (i < 2) return;

//   // 시스템 연결/인증 관련 서버 메시지 처리
//   if (!strncmp(pArray[1], " New", 4)) { Serial.println(F("connected")); return; }
//   if (!strncmp(pArray[1], " Alr", 4)) { client.stop(); serverConnect(); return; }
//   if (!strncmp(pArray[1], " Aut", 4)) { Serial.println(F("AUTH FAIL: add ARD to idpasswd.txt")); return; }

//   // DOC(중앙 서버/매니저)가 보낸 메시지가 아니라면 무시
//   if (strcmp(pArray[0], "DOC") != 0) return;

//   // 1️⃣ [최종 배출 완료 통지] 
//   // 서버가 STM32의 완료 신호를 받고 DB 갱신 후 ARD로 완료 패킷을 보냈을 때
//   if (!strcmp(pArray[1], "PICKUP_DONE")) {
//       handleDone(pArray, i);  // LCD에 "Dispense Done" 표시 및 아이들 상태 전환
//   }
  
//   // 2️⃣ [실제 배출 승인 및 시작 통지] 
//   // 키패드로 # 승인 후 서버가 "모터 구동 시작한다"고 아두이노에 알릴 때
//   else if (!strcmp(pArray[1], "PICKUP")) {
//       handleStart(pArray, i); // LCD에 "Dispensing..." 표시 및 ST_WAIT_DISPENSE 진입
//   }
  
//   // 3️⃣ [증상 처방에 따른 예약 완료 통지]
//   else if (!strcmp(pArray[1], "PRESCRIPTION")) {
//       if (i >= 3 && !strncmp(pArray[2], "FAIL", 4)) {
//           digitalWrite(LED_BUSY, LOW);
//           showFail(pArray[2]);
//           enterResult();
//       } else {
//           // 예약 완료 인지 단계 -> 유저에게 안내 후 # 버튼 대기
//           strncpy(dispMed, pArray[2], sizeof(dispMed) - 1);
//           dispMed[sizeof(dispMed) - 1] = '\0';
//           dispQty = (i >= 4) ? atoi(pArray[3]) : 0;
          
//           digitalWrite(LED_BUSY, LOW);
//           lcd.clear();
//           lcd.print(F("Reserved!"));
//           lcd.setCursor(0, 1);
//           lcd.print(dispMed);
//           lcd.print(F(" x"));
//           lcd.print(dispQty);
          
//           state = ST_INPUT_ID; // # 키를 누르면 PICKUP 요청으로 전송할 수 있도록 유도
//       }
//   }
// }

/* 들어온 바이트를 전부 비블로킹으로 읽어 라인 단위로 쪼갬 */
void pumpSerial() {
  while (client.available()) {
    char c = (char)client.read();
    if (c == '\n' || c == '\r') {
      if (gLineLen > 0) {
        gLine[gLineLen] = '\0';
        processLine(gLine);
        gLineLen = 0;
      }
    } else if (gLineLen < CMD_SIZE - 1) {
      gLine[gLineLen++] = c;
    } else {
      gLineLen = 0;   // 버퍼 초과 = 깨진 라인 → 통째로 폐기
    }
  }
}

/* 완성된 한 줄 파싱/처리 (기존 socketEvent 본문에서 read 부분만 뺀 것) */
void processLine(char* recvBuf) {
  Serial.print(F("recv: "));
  Serial.println(recvBuf);

  char* pArray[ARR_CNT] = { 0 };
  int i = 0;
  char* p = strtok(recvBuf, "[@]");
  while (p && i < ARR_CNT) {
    pArray[i++] = p;
    p = strtok(NULL, "[@]");
  }
  if (i < 2) return;

  if (!strncmp(pArray[1], " New", 4)) { Serial.println(F("connected")); return; }
  if (!strncmp(pArray[1], " Alr", 4)) { client.stop(); serverConnect(); return; }
  if (!strncmp(pArray[1], " Aut", 4)) { Serial.println(F("AUTH FAIL")); return; }

  if (strcmp(pArray[0], "DOC") != 0) return;   // WAKEUP 등은 여기서 자연스럽게 무시됨

  if (!strcmp(pArray[1], "PICKUP_DONE")) {
      handleDone(pArray, i);
  }
  else if (!strcmp(pArray[1], "PICKUP_FAIL")) {        // ★ 신규 핸들러
      digitalWrite(LED_BUSY, LOW);
      showMsg(F("Dispense Fail"), F("Try again"));
      enterResult();
  }
  else if (!strcmp(pArray[1], "PICKUP")) {
      handleStart(pArray, i);
  }
  else if (!strcmp(pArray[1], "PRESCRIPTION")) {
      if (i >= 3 && !strncmp(pArray[2], "FAIL", 4)) {
          digitalWrite(LED_BUSY, LOW);
          showFail(pArray[2]);
          enterResult();
      } else {
          strncpy(dispMed, pArray[2], sizeof(dispMed) - 1);
          dispMed[sizeof(dispMed) - 1] = '\0';
          dispQty = (i >= 4) ? atoi(pArray[3]) : 0;
          digitalWrite(LED_BUSY, LOW);
          lcd.clear();
          lcd.print(F("Reserved!"));
          lcd.setCursor(0, 1);
          lcd.print(dispMed);
          lcd.print(F(" x"));
          lcd.print(dispQty);
          state = ST_INPUT_ID;
      }
  }
}

/* ─────────── 배출 시작 (PRESCRIPTION / PICKUP) ───────────
 * a[2]=약이름 또는 FAIL_*, a[3]=수량
 * 성공 → LCD에 약 정보 → "Dispensing..." → ST_WAIT_DISPENSE
 * 실패 → showFail
 */
void handleStart(char** a, int n) {
    if (n >= 3 && !strncmp(a[2], "FAIL", 4)) {
        digitalWrite(LED_BUSY, LOW);
        if (!strcmp(a[2], "FAIL_EMPTY")) {
            state = ST_SYMPTOM;
            showSymptomMenu();
            return;
        }
        showFail(a[2]);
        enterResult();
        return;
    }

    strncpy(dispMed, a[2], sizeof(dispMed) - 1);
    dispMed[sizeof(dispMed) - 1] = '\0';
    dispQty = (n >= 4) ? atoi(a[3]) : 0;

  // 약 이름 + 수량 잠깐 표시
  digitalWrite(LED_BUSY, HIGH);
  lcd.clear();
  lcd.print(F("ID:"));
  lcd.print(userID);
  lcd.setCursor(0, 1);
  lcd.print(dispMed);
  lcd.print(F(" x"));
  lcd.print(dispQty);
  // delay(1200);

      // 1.2초 후 "Dispensing..." 표시할 시각 기록
    showMedUntil = millis() + 1200;
    showingMed = true;

  // // Dispensing 화면 (STM 모터 동작 중)
  // lcd.clear();
  // lcd.print(F("Dispensing..."));
  // lcd.setCursor(0, 1);
  // lcd.print(dispMed);

  state = ST_WAIT_DISPENSE;
  dispenseStart = millis();
}

/* ─────────── 배출 완료 (PICKUP_DONE) ───────────
 * DOC가 STM PICKUP_DONE + DB 갱신까지 끝낸 뒤 보내는 최종 통지
 */
void handleDone(char** a, int n) {
  digitalWrite(LED_BUSY, LOW);
  lcd.clear();
  lcd.print(F("Dispense Done"));
  lcd.setCursor(0, 1);
  if (n >= 3 && strncmp(a[2], "FAIL", 4) != 0) {
    lcd.print(a[2]);          // 약 이름
    if (n >= 4) { lcd.print(F(" x")); lcd.print(atoi(a[3])); }
  } else {
    lcd.print(dispMed);
  }
  enterResult();
}

/* ─────────── FAIL_* 코드 → LCD ─────────── */
void showFail(const char* code) {
  if (!strcmp(code, "FAIL_24H_LIMIT"))            showMsg(F("Denied"), F("24h limit"));
  else if (!strcmp(code, "FAIL_TOTAL_LIMIT"))     showMsg(F("Denied"), F("Max 30 reached"));
  else if (!strcmp(code, "FAIL_OVERDOSE"))        showMsg(F("Denied"), F("Overdose risk"));
  else if (!strcmp(code, "FAIL_AGE_LIMIT"))       showMsg(F("Denied"), F("Age restricted"));
  else if (!strcmp(code, "FAIL_ALLERGY"))         showMsg(F("Denied"), F("Allergy risk"));
  else if (!strcmp(code, "FAIL_EMPTY"))           showMsg(F("No reservation"), F("Already taken"));
  else if (!strcmp(code, "FAIL_STOCK"))           showMsg(F("Denied"), F("Out of stock"));
  else if (!strcmp(code, "FAIL_ALREADY_RESERVED"))showMsg(F("Denied"), F("2 reserved max"));
  else if (!strcmp(code, "FAIL_SYMPTOM"))         showMsg(F("Denied"), F("No matching med"));
  else if (!strcmp(code, "FAIL_DISPENSE"))        showMsg(F("Dispense Fail"), F("Try again"));
  else if (!strcmp(code, "FAIL_DB"))              showMsg(F("Denied"), F("Not registered"));
  else {
    Serial.print(F("Unknown FAIL code: "));
    Serial.println(code);
    showMsg(F("Denied"), F("No medicine"));
  }
}

/* ─────────── LCD/상태 보조 ─────────── */
void goIdle() {
  //dispDone = false; // 추가
  state = ST_IDLE;
  idLen = 0;
  userID[0] = '\0';
  dispMed[0] = '\0';
  dispQty = 0;
  digitalWrite(LED_BUSY, LOW);
  lcd.clear();
  lcd.print(F("Smart Dispenser"));
  lcd.setCursor(0, 1);
  lcd.print(F("ID/# or Tap card"));
}
void showMsg(const __FlashStringHelper* l1, const __FlashStringHelper* l2) {
  lcd.clear();
  lcd.print(l1);
  lcd.setCursor(0, 1);
  lcd.print(l2);
}
void enterResult() {
  state = ST_RESULT;
  resultUntil = millis() + RESULT_HOLD;
}
void showSymptomMenu() {
  lcd.clear();
  lcd.print(F("Select symptom:"));
  lcd.setCursor(0, 1);
  lcd.print(F("1H2S3I4C5F6N"));
  // 1=Headache 2=Stomachache 3=Indigestion 4=Cramps 5=Fever 6=Insomnia
}

/* ─────────── WiFi ─────────── */
void wifiSetup() {
  wifiSerial.begin(38400);
  do {
    WiFi.init(&wifiSerial);
    if (WiFi.status() != WL_NO_SHIELD) break;
    Serial.println(F("ESP not found, retry"));
  } while (1);
  while (WiFi.begin(AP_SSID, AP_PASS) != WL_CONNECTED)
    Serial.println(F("WiFi connecting..."));
  serverConnect();
}
void serverConnect() {
  if (client.connect(SERVER_NAME, SERVER_PORT))
    client.print("[" LOGID ":" PASSWD "]");
}
