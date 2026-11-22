/*
 * conductor.ino — Uno 지휘자 v1 (센서 악기 합주)  — v0 + I2C로 세 보드 줄 받기 + 간단 채점
 *
 * v1에서 바뀐 것 (11/22, USB 허브 없이 Uno 하나만 노트북에 꽂는 구조)
 *   - Uno = I2C 슬레이브 0x08. 세 STM32가 I2C 마스터로 게임 줄(W,ON,MI,72,21,+12 등)을 보낸다
 *   - 받은 줄을 그대로 USB 시리얼로 넘긴다 → 게임은 Uno 포트 하나만 연다
 *   - 곡이 끝나면 악기별로 받은 줄을 세어 #EVAL 줄로 낸다 (Uno가 연주를 평가)
 *   - 배선: A4(SDA)·A5(SCL) ↔ 세 보드 D14(PB9)·D15(PB8), 4.7kΩ 풀업 두 개를 5V로, GND 공통
 *   - 박자 펄스(D8)·START·COUNT·BEAT·END는 v0 그대로
 *
 * 하는 일
 *   - 박자 펄스: D8에서 박마다 10ms HIGH. 관악기·하프·드럼 STM32가 EXTI(올라가는 순간)로 받는다.
 *   - 시리얼(115200bps, USB)로 PC 리듬게임에 BEAT,<박번호>를 보낸다.
 *
 * 흐름
 *   PC가 "START" 또는 "START,<bpm>" 한 줄을 보냄 (또는 D2 버튼, 또는 AUTO_START_MS)
 *     -> COUNT,4 COUNT,3 COUNT,2 COUNT,1   (카운트 4박: 시리얼만, 펄스 없음)
 *     -> BEAT,0 ... BEAT,63                (16마디 x 4박: 펄스 + 시리얼)
 *     -> END
 *   "STOP" 한 줄이면 바로 멈추고 END,STOP.
 *
 * 카운트에 펄스를 내지 않는 이유
 *   STM32 쪽은 "2초 넘게 쉬었다 오는 첫 펄스 = 0번 박"으로 박 번호를 센다(관악기 v2 기준).
 *   카운트에도 펄스를 내면 카운트 첫 박이 0번이 되어 박 번호가 4개씩 밀린다.
 *   같은 이유로 BEAT,0 직전에는 펄스가 QUIET_MS 이상 없도록 시작을 미룬다.
 *
 * 아직 안 하는 것 (연결 구조가 정해지면 추가)
 *   - I2C로 세 보드의 연주 기록 모으기, Uno에서 채점·점수 표시
 *   지금은 각 보드가 자기 USB 시리얼로 PC에 직접 보내는 구조(허브)에서 바로 쓸 수 있다.
 *
 * 배선
 *   D8 -> 각 STM32 박자 입력 핀 (관악기는 PB5).  Uno는 5V라서 STM32 쪽에 1kΩ+2kΩ 분압 권장.
 *   GND는 모든 보드 공통.
 *   D2 (선택) -- 버튼 -- GND   : 누르면 시작 (내부 풀업 사용)
 *
 * 시리얼 형식은 docs/serial-protocol.md. '#'으로 시작하는 줄은 사람이 읽는 메모(게임은 무시).
 */

#include <Wire.h>

/* ===== 바꿀 수 있는 값 ===== */
const uint8_t  I2C_ADDR       = 0x08;  // STM32 쪽 LINK_I2C_ADDR와 같게
const uint8_t  PIN_BEAT       = 8;     // 박자 펄스 출력
const uint8_t  PIN_LED        = 13;    // 보드 LED: 박마다 깜빡
const uint8_t  PIN_START_BTN  = 2;     // 시작 버튼 (GND로 누름)
const bool     USE_START_BTN  = true;

const uint16_t BPM_DEFAULT    = 90;    // 게임 기본 템포와 같게
const uint16_t BPM_MIN        = 60;
const uint16_t BPM_MAX        = 140;
const uint8_t  COUNT_IN       = 4;     // 카운트 박 수
const uint8_t  SONG_BEATS     = 64;    // 나비야 16마디 x 4박
const uint16_t PULSE_MS       = 10;    // 펄스 폭
const uint16_t LED_MS         = 80;    // LED 켜진 시간 (눈에 보이게)
const uint16_t QUIET_MS       = 2200;  // BEAT,0 직전에 펄스가 없어야 하는 시간 (STM32 기준 2초 + 여유)
const uint32_t AUTO_START_MS  = 0;     // 0이 아니면 리셋 후 이 시간 뒤 자동 시작 (게임 없이 시험할 때, 예: 3000)

/* ===== 상태 ===== */
enum State { IDLE, COUNTING, PLAYING };
State    state = IDLE;
uint16_t bpm = BPM_DEFAULT;
uint32_t periodUs;          // 한 박 길이
uint32_t t0Us;              // 카운트 첫 박 시각. k번째 박 = t0Us + k * periodUs (오차가 쌓이지 않게)
uint8_t  stepIdx;           // 0..COUNT_IN-1 = 카운트, 그 뒤 = 곡 박

bool     pulseHigh = false;
uint32_t pulseOnUs;
bool     ledOn = false;
uint32_t ledOnMs;
bool     everPulsed = false;
uint32_t lastPulseMs;

char     lineBuf[24];
uint32_t endedMs = 0;
bool     evalPending = false;
uint8_t  lineLen = 0;
bool     autoStarted = false;
bool     btnPrev = true;
uint32_t btnChangeMs = 0;

/* ===== I2C로 받은 줄 (수신 인터럽트 → 고리 버퍼 → loop에서 USB로) ===== */
const uint8_t  RX_LINES = 12;
const uint8_t  RX_LEN   = 32;          // Wire 버퍼와 같은 크기
volatile char    rxBuf[RX_LINES][RX_LEN + 1];
volatile uint8_t rxLen[RX_LINES];
volatile uint8_t rxHead = 0, rxTail = 0;   // head = 넣을 칸, tail = 꺼낼 칸
volatile uint16_t rxDrop = 0;
volatile uint16_t rxCount = 0, rxW = 0, rxH = 0, rxD = 0;   // 진단: 받은 줄 수(악기별은 #S·연주 줄의 첫 글자로)

// 진단: SDA(A4=PC4)·SCL(A5=PC5) 선이 바뀐 횟수. 누가 I2C로 보내려고 시도라도 하는지 본다.
// 핀 변화 인터럽트(PCINT12·13). 연주 중에는 꺼서 박자 타이밍에 영향 없게.
volatile uint16_t sclEdges = 0, sdaEdges = 0;
volatile uint8_t  pcLast = 0;
ISR(PCINT1_vect)
{
  uint8_t p = PINC, ch = p ^ pcLast;
  pcLast = p;
  if (ch & _BV(5)) sclEdges++;
  if (ch & _BV(4)) sdaEdges++;
}
void edgeWatch(bool on)
{
  if (on) { pcLast = PINC; PCMSK1 |= _BV(PCINT12) | _BV(PCINT13); PCICR |= _BV(PCIE1); }
  else    { PCMSK1 &= ~(_BV(PCINT12) | _BV(PCINT13)); }
}

void onI2cReceive(int n)               // TWI 인터럽트 안: 복사만 하고 바로 끝
{
  uint8_t next = (rxHead + 1) % RX_LINES;
  if (next == rxTail) { rxDrop++; while (Wire.available()) Wire.read(); return; }
  uint8_t k = 0;
  while (Wire.available()) {
    char c = (char)Wire.read();
    if (k < RX_LEN && c != '\r' && c != '\n') rxBuf[rxHead][k++] = c;
  }
  if (k == 0) return;
  rxBuf[rxHead][k] = '\0';
  rxLen[rxHead] = k;
  rxCount++;
  char c0 = rxBuf[rxHead][0] == '#' && k > 3 ? rxBuf[rxHead][3] : rxBuf[rxHead][0];   // "#S,W,..." → W
  if (c0 == 'W') rxW++; else if (c0 == 'H') rxH++; else if (c0 == 'D') rxD++;
  rxHead = next;
  (void)n;
}

/* ===== Uno 채점: 악기별로 박마다 첫 ON 줄만 세서 P/G/B, 안 온 박은 M (채보 노트 수 기준 근사) ===== */
const uint8_t  N_INST = 3;
const char     INST_CH[N_INST]    = {'W', 'H', 'D'};
const uint8_t  CHART_NOTES[N_INST] = {54, 32, 63};          // 나비야 채보 (게임과 같음)
const int16_t  WIN_P[N_INST] = {100, 50, 50}, WIN_G[N_INST] = {200, 120, 120}, WIN_B[N_INST] = {400, 250, 250};
uint16_t evalP[N_INST], evalG[N_INST], evalB[N_INST], evalX[N_INST];
uint32_t seenLo[N_INST], seenHi[N_INST];                    // 0~63번 박 중 이미 센 박
char     modeLabel[8] = "?";

void evalReset()
{
  for (uint8_t i = 0; i < N_INST; i++) { evalP[i] = evalG[i] = evalB[i] = evalX[i] = 0; seenLo[i] = seenHi[i] = 0; }
}

void evalLine(const char *s, uint8_t len)
{
  if (len >= 8 && !strncmp(s, "MODE,DMA_", 9)) { strncpy(modeLabel, s + 9, sizeof(modeLabel) - 1); modeLabel[sizeof(modeLabel) - 1] = 0; return; }
  int8_t inst = -1;
  for (uint8_t i = 0; i < N_INST; i++) if (s[0] == INST_CH[i]) inst = i;
  if (inst < 0 || len < 12 || strncmp(s + 1, ",ON,", 4) != 0) return;
  // 악기,ON,음,세기,박,오차[,등급] → 쉼표 위치로 박과 오차 꺼내기
  const char *f[7]; uint8_t nf = 0; f[nf++] = s;
  for (uint8_t i = 0; i < len && nf < 7; i++) if (s[i] == ',') f[nf++] = s + i + 1;
  if (nf < 6) return;
  int beat = atoi(f[4]);
  int off  = atoi(f[5][0] == '+' ? f[5] + 1 : f[5]);
  if (beat < 0 || beat > 63) return;
  uint32_t bit = 1UL << (beat & 31);
  uint32_t *seen = beat < 32 ? &seenLo[inst] : &seenHi[inst];
  int a = off < 0 ? -off : off;
  if ((*seen & bit) || a > WIN_B[inst]) { evalX[inst]++; return; }   // 같은 박 두 번째·너무 멂 = 헛음
  *seen |= bit;
  if (a <= WIN_P[inst]) evalP[inst]++; else if (a <= WIN_G[inst]) evalG[inst]++; else evalB[inst]++;
}

void evalPrint()
{
  for (uint8_t i = 0; i < N_INST; i++) {
    uint16_t hit = evalP[i] + evalG[i] + evalB[i];
    uint16_t miss = CHART_NOTES[i] > hit ? CHART_NOTES[i] - hit : 0;
    Serial.print(F("#EVAL,")); Serial.print(INST_CH[i]); Serial.print(','); Serial.print(modeLabel);
    Serial.print(F(",P=")); Serial.print(evalP[i]); Serial.print(F(",G=")); Serial.print(evalG[i]);
    Serial.print(F(",B=")); Serial.print(evalB[i]); Serial.print(F(",M=")); Serial.print(miss);
    Serial.print(F(",X=")); Serial.println(evalX[i]);
  }
  if (rxDrop) { Serial.print(F("#EVAL,rxDrop=")); Serial.println(rxDrop); }
}

void forwardLines()                    // 박자 출력이 밀리지 않게, USB 송신 버퍼에 여유가 있을 때만 한 줄씩
{
  while (rxTail != rxHead) {
    uint8_t len = rxLen[rxTail];
    if (Serial.availableForWrite() < len + 1 + 16) return;   // BEAT 줄 자리 16바이트는 남겨 둠
    char line[RX_LEN + 1];
    noInterrupts();
    for (uint8_t i = 0; i <= len; i++) line[i] = rxBuf[rxTail][i];
    interrupts();
    rxTail = (rxTail + 1) % RX_LINES;
    Serial.write((const uint8_t *)line, len);
    Serial.write('\n');
    if (state != IDLE || evalPending || line[0] == 'M') evalLine(line, len);   // 연주 중·끝난 직후 줄만 채점 (MODE는 언제든)
  }
}

/* ===== 시작 / 멈춤 ===== */
void startSong(uint16_t newBpm)
{
  if (state != IDLE) {
    Serial.println(F("#busy: STOP first"));
    return;
  }
  if (newBpm < BPM_MIN) newBpm = BPM_MIN;
  if (newBpm > BPM_MAX) newBpm = BPM_MAX;
  bpm = newBpm;
  periodUs = 60000000UL / bpm;

  // BEAT,0 직전 QUIET_MS 동안 펄스가 없도록, 필요하면 카운트 시작을 미룬다
  uint32_t countMs = (uint32_t)COUNT_IN * (periodUs / 1000UL);
  uint32_t waitMs = 100;  // 첫 카운트는 0.1초 뒤
  if (everPulsed) {
    uint32_t since = millis() - lastPulseMs;
    if (since + waitMs + countMs < QUIET_MS) waitMs = QUIET_MS - since - countMs;
  }

  t0Us = micros() + waitMs * 1000UL;
  stepIdx = 0;
  state = COUNTING;
  evalReset();
  edgeWatch(false);
  Serial.print(F("#start bpm="));
  Serial.println(bpm);
}

void stopSong(bool byUser)
{
  state = IDLE;
  digitalWrite(PIN_BEAT, LOW);
  pulseHigh = false;
  Serial.println(byUser ? F("END,STOP") : F("END"));
  endedMs = millis();
  evalPending = true;
  edgeWatch(true);                  // 마지막 박 뒤 늦게 오는 줄(최대 반 박 + 전송)을 기다렸다가 출력
}

/* ===== 박 하나 처리 ===== */
void onStep()
{
  if (stepIdx < COUNT_IN) {
    // 카운트: 시리얼만
    Serial.print(F("COUNT,"));
    Serial.println(COUNT_IN - stepIdx);
  } else {
    // 곡 박: 펄스 먼저, 그다음 시리얼 (펄스 시각이 기준)
    uint8_t beat = stepIdx - COUNT_IN;
    digitalWrite(PIN_BEAT, HIGH);
    pulseHigh = true;
    pulseOnUs = micros();
    everPulsed = true;
    lastPulseMs = millis();
    Serial.print(F("BEAT,"));
    Serial.println(beat);
  }
  digitalWrite(PIN_LED, HIGH);
  ledOn = true;
  ledOnMs = millis();

  stepIdx++;
  if (stepIdx == COUNT_IN) state = PLAYING;
}

/* ===== PC에서 온 한 줄 ===== */
void handleLine(char *s)
{
  for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;  // 대문자로

  if (strcmp(s, "START") == 0) {
    startSong(bpm);
  } else if (strncmp(s, "START,", 6) == 0) {
    int v = atoi(s + 6);
    startSong(v > 0 ? (uint16_t)v : bpm);
  } else if (strcmp(s, "STOP") == 0) {
    if (state != IDLE) stopSong(true);
  } else if (s[0] != '\0') {
    Serial.print(F("#unknown: "));
    Serial.println(s);
  }
}

void readSerial()
{
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      lineBuf[lineLen] = '\0';
      handleLine(lineBuf);
      lineLen = 0;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

void readButton()
{
  if (!USE_START_BTN) return;
  bool now = digitalRead(PIN_START_BTN);  // 풀업: 안 누르면 HIGH
  if (now != btnPrev && millis() - btnChangeMs > 50) {
    btnChangeMs = millis();
    btnPrev = now;
    if (now == LOW && state == IDLE) startSong(bpm);
  }
}

void setup()
{
  pinMode(PIN_BEAT, OUTPUT);
  digitalWrite(PIN_BEAT, LOW);
  pinMode(PIN_LED, OUTPUT);
  if (USE_START_BTN) pinMode(PIN_START_BTN, INPUT_PULLUP);
  Serial.begin(115200);
  Wire.begin(I2C_ADDR);                // 슬레이브. 내부 풀업도 켜지지만 외부 4.7kΩ(5V)을 꼭 다는 것을 권장
  Wire.onReceive(onI2cReceive);
  edgeWatch(true);
  Serial.println(F("#conductor v1 ready (I2C 0x08): send START or START,<bpm>, or press D2"));
}

void loop()
{
  readSerial();
  readButton();

  if (AUTO_START_MS > 0 && !autoStarted && millis() >= AUTO_START_MS) {
    autoStarted = true;
    startSong(bpm);
  }

  if (state != IDLE) {
    uint32_t due = t0Us + (uint32_t)stepIdx * periodUs;
    if ((int32_t)(micros() - due) >= 0) {
      if (stepIdx >= COUNT_IN + SONG_BEATS) stopSong(false);  // 마지막 박 뒤 한 박 쉬고 END
      else onStep();
    }
  }

  if (pulseHigh && micros() - pulseOnUs >= PULSE_MS * 1000UL) {
    digitalWrite(PIN_BEAT, LOW);
    pulseHigh = false;
  }
  if (ledOn && millis() - ledOnMs >= LED_MS) {
    digitalWrite(PIN_LED, LOW);
    ledOn = false;
  }

  forwardLines();                      // I2C로 받은 줄 → USB

  // 진단: 쉬는 동안 2초마다 Uno 쪽 I2C 상태. sda/scl은 선 전압(1 = HIGH 정상, 0 = LOW로 붙음)
  static uint32_t diagMs = 0;
  if (state == IDLE && !evalPending && millis() - diagMs >= 2000 && Serial.availableForWrite() > 50) {
    diagMs = millis();
    Serial.print(F("#UNO,rx=")); Serial.print(rxCount);
    Serial.print(F(",W=")); Serial.print(rxW); Serial.print(F(",H=")); Serial.print(rxH);
    Serial.print(F(",D=")); Serial.print(rxD); Serial.print(F(",drop=")); Serial.print(rxDrop);
    noInterrupts();
    uint16_t se = sclEdges, de = sdaEdges; sclEdges = 0; sdaEdges = 0;
    interrupts();
    Serial.print(F(",sda=")); Serial.print((PINC >> 4) & 1);
    Serial.print(F(",scl=")); Serial.print((PINC >> 5) & 1);
    Serial.print(F(",sclEdge=")); Serial.print(se);          // 2초 동안 SCL이 바뀐 횟수 (65535에서 멈춤 없이 넘칠 수 있음)
    Serial.print(F(",sdaEdge=")); Serial.println(de);
  }
  if (evalPending && millis() - endedMs >= 1500) {   // END 뒤 1.5초 기다렸다가 채점 출력
    evalPending = false;
    forwardLines();
    evalPrint();
  }
}
