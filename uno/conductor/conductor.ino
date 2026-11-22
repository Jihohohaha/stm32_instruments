/*
 * conductor.ino — Uno 지휘자 v0 (센서 악기 합주)
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

/* ===== 바꿀 수 있는 값 ===== */
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
uint8_t  lineLen = 0;
bool     autoStarted = false;
bool     btnPrev = true;
uint32_t btnChangeMs = 0;

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
  Serial.print(F("#start bpm="));
  Serial.println(bpm);
}

void stopSong(bool byUser)
{
  state = IDLE;
  digitalWrite(PIN_BEAT, LOW);
  pulseHigh = false;
  Serial.println(byUser ? F("END,STOP") : F("END"));
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
  Serial.println(F("#conductor v0 ready: send START or START,<bpm>"));
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
}
