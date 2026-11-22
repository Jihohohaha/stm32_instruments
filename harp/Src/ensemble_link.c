/*
 * ensemble_link.c - 센서 악기 합주 공통 연결 모듈 (설명은 ensemble_link.h, README.md)
 * 주변장치 설정은 레지스터 직접 (GPIO CRL/CRH·IDR·ODR, AFIO EXTICR, EXTI, DWT, USART2)
 */
#include "ensemble_link.h"

#ifndef LINK_HOST_TEST
#include "stm32f1xx.h"             /* CMSIS 레지스터 정의 (HAL 없어도 됨) */
extern uint32_t HAL_GetTick(void) __attribute__((weak));   /* HAL이 있으면 씀, 없으면 link_millis를 보드에서 정의 */
#define LINK_IRQ_SAVE()    uint32_t _pm = __get_PRIMASK(); __disable_irq()
#define LINK_IRQ_RESTORE() __set_PRIMASK(_pm)
#define WEAK __attribute__((weak))
#else
#include "host_mock.h"             /* 호스트 시험용 */
#endif

/* ---------------- 박자 기록 ---------------- */
#define HIST 8
typedef struct { int32_t n; uint32_t t; } pulse_t;
static volatile pulse_t hist[HIST];        /* 최근 펄스 (이번 곡만) */
static volatile uint8_t  hist_len, hist_head;   /* head = 다음에 쓸 칸 */
static volatile uint32_t beat_ms = LINK_DEFAULT_BEAT_MS;
static volatile uint32_t last_pulse_t;
static volatile uint8_t  any_pulse, new_song_flag;

/* ---------------- 이벤트 큐 ---------------- */
#define QLEN 32
typedef struct { uint32_t t; char pitch[6]; uint8_t vel; uint8_t on; } ev_t;
static volatile ev_t     q[QLEN];
static volatile uint8_t  q_head, q_tail;   /* head = 꺼낼 칸, tail = 넣을 칸 */
static volatile uint32_t q_drop;

/* ---------------- 모드·통계 ---------------- */
static volatile uint8_t  dma_on = LINK_START_DMA_ON;
static volatile uint32_t busy_cyc, busy_start, busy_depth, miss_cnt;
static uint32_t stat_t0, btn_t;
static uint8_t  btn_stable = 1, btn_raw = 1;

/* =========================================================== 출력 */
static int put_str(char *p, const char *s) { int n = 0; while (s[n]) { p[n] = s[n]; n++; } return n; }
static int put_int(char *p, int32_t v, int plus)
{
    char tmp[12]; int n = 0, k = 0;
    uint32_t u = v < 0 ? (uint32_t)(-v) : (uint32_t)v;
    if (v < 0) p[n++] = '-'; else if (plus) p[n++] = '+';
    do { tmp[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (k) p[n++] = tmp[--k];
    return n;
}
static void i2c_send(const char *s, int n);
static void send(const char *s, int n)
{
#if LINK_UART_ENABLE
    link_uart_write(s, n);
#endif
#if LINK_I2C_ENABLE && !defined(LINK_HOST_TEST)
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;   /* 줄 끝은 Uno가 붙인다 */
    i2c_send(s, n > 31 ? 31 : n);                                   /* Uno Wire 버퍼 32바이트 */
#endif
#if !LINK_UART_ENABLE && !(LINK_I2C_ENABLE && !defined(LINK_HOST_TEST))
    (void)s; (void)n;
#endif
}

static void print_mode(void)
{
    char b[24]; int n = put_str(b, dma_on ? "MODE,DMA_ON\r\n" : "MODE,DMA_OFF\r\n");
    send(b, n);
}

void link_debug(const char *msg)
{
    char b[96]; int n = 0;
    b[n++] = '#';
    while (*msg && n < 92) b[n++] = *msg++;
    b[n++] = '\r'; b[n++] = '\n';
    send(b, n);
}

/* 악기,이벤트,음,세기,박번호,±오차 */
static void print_event(const ev_t *e, int32_t beat, int32_t off)
{
    char b[48]; int n = 0;
    b[n++] = (char)LINK_INST; b[n++] = ',';
    n += put_str(b + n, e->on ? "ON," : "OFF,");
    n += put_str(b + n, e->pitch); b[n++] = ',';
    n += put_int(b + n, e->on ? e->vel : 0, 0); b[n++] = ',';
    n += put_int(b + n, beat, 0); b[n++] = ',';
    n += put_int(b + n, off, 1);
    b[n++] = '\r'; b[n++] = '\n';
    send(b, n);
}
static void print_dropped(const ev_t *e, const char *why)
{
    char b[64]; int n = 0;
    b[n++] = '#'; b[n++] = (char)LINK_INST; b[n++] = ',';
    n += put_str(b + n, e->on ? "ON," : "OFF,");
    n += put_str(b + n, e->pitch); b[n++] = ',';
    n += put_str(b + n, why);
    b[n++] = '\r'; b[n++] = '\n';
    send(b, n);
}

/* =========================================================== 박자 펄스 */
static void on_pulse(uint32_t t)
{
    if (any_pulse && (t - last_pulse_t) < LINK_MIN_BEAT_MS) return;     /* 잡음 */
    if (!any_pulse || (t - last_pulse_t) >= LINK_SONG_GAP_MS) {         /* 새 곡: 0번 박 */
        hist_len = 0; hist_head = 0;
        beat_ms = LINK_DEFAULT_BEAT_MS;
        hist[0].n = 0; hist[0].t = t;
        hist_head = 1; hist_len = 1;
        new_song_flag = 1;
    } else {
        uint8_t last = (uint8_t)((hist_head + HIST - 1) % HIST);
        uint32_t iv = t - last_pulse_t;
        beat_ms = (beat_ms * 3 + iv) / 4;                                /* 부드럽게 */
        if (hist_len == 1) beat_ms = iv;                                 /* 첫 간격은 그대로 */
        hist[hist_head].n = hist[last].n + 1;
        hist[hist_head].t = t;
        hist_head = (uint8_t)((hist_head + 1) % HIST);
        if (hist_len < HIST) hist_len++;
    }
    last_pulse_t = t;
    any_pulse = 1;
}

/* =========================================================== 이벤트 */
static void push(const char *pitch, uint8_t vel, uint8_t on)
{
    uint32_t t = link_millis() - LINK_LATENCY_MS;
    LINK_IRQ_SAVE();
    uint8_t nt = (uint8_t)((q_tail + 1) % QLEN);
    if (nt == q_head) { q_drop++; LINK_IRQ_RESTORE(); return; }
    volatile ev_t *e = &q[q_tail];
    e->t = t; e->vel = vel > 100 ? 100 : vel; e->on = on;
    int i = 0;
    for (; i < 5 && pitch[i]; i++) e->pitch[i] = pitch[i];
    e->pitch[i] = 0;
    q_tail = nt;
    LINK_IRQ_RESTORE();
}
void link_note_on(const char *pitch, uint8_t vel) { push(pitch, vel, 1); }
void link_note_off(const char *pitch)            { push(pitch, 0, 0); }

/* 이벤트 시각 te를 박 번호 + 오차로 바꾼다.
 *  돌려주는 값: 1 = 확정(beat, off에 담음), 0 = 다음 펄스를 더 기다려야 함, -1 = 버림
 *  ON : 가장 가까운 박 기준. 박 사이 뒤쪽 절반이면 다음 펄스를 기다렸다가 음수로.
 *  OFF: 바로 앞 박 기준(항상 +). */
static int resolve(uint32_t te, uint8_t on, uint32_t now, int32_t *beat, int32_t *off)
{
    pulse_t h[HIST]; uint8_t len, head; uint32_t bms;
    {
        LINK_IRQ_SAVE();
        len = hist_len; head = hist_head; bms = beat_ms;
        for (uint8_t i = 0; i < len; i++) {           /* 오래된 것부터 h[0..len-1] */
            uint8_t k = (uint8_t)((head + HIST - len + i) % HIST);
            h[i].n = hist[k].n; h[i].t = hist[k].t;
        }
        LINK_IRQ_RESTORE();
    }
    int prev = -1, next = -1;
    for (int i = 0; i < len; i++) {
        if ((int32_t)(h[i].t - te) <= 0) prev = i;
        else { next = i; break; }
    }
    if (prev >= 0 && (te - h[prev].t) >= LINK_SONG_GAP_MS) prev = -1;  /* 지난 곡의 박은 기준이 아님 */

    if (prev < 0) {                                   /* 곡 시작 전(또는 기록보다 오래 전) */
        if (on && next >= 0 && (h[next].t - te) <= bms / 2) { *beat = h[next].n; *off = -(int32_t)(h[next].t - te); return 1; }
        if (next < 0 && (now - te) <= bms / 2 + 20) return 0;    /* 첫 펄스가 곧 올 수도 있음 */
        return -1;
    }
    uint32_t el = te - h[prev].t;
    if (!on || el <= bms / 2) { *beat = h[prev].n; *off = (int32_t)el; return 1; }
    if (next >= 0) { *beat = h[next].n; *off = -(int32_t)(h[next].t - te); return 1; }
    if ((now - h[prev].t) > bms + bms / 2) { *beat = h[prev].n; *off = (int32_t)el; return 1; }  /* 다음 박이 안 옴(곡 끝) */
    return 0;
}

static void drain_events(void)
{
    uint32_t now = link_millis();
    while (q_head != q_tail) {
        ev_t e;
        e.t = q[q_head].t; e.vel = q[q_head].vel; e.on = q[q_head].on;
        for (int i = 0; i < 6; i++) e.pitch[i] = q[q_head].pitch[i];
        int32_t beat = 0, off = 0;
        int r = resolve(e.t, e.on, now, &beat, &off);
        if (r == 0) break;                            /* 앞 줄이 기다리면 뒤 줄도 기다림(순서 유지) */
        if (r > 0) print_event(&e, beat, off);
        else       print_dropped(&e, "no beat");
        q_head = (uint8_t)((q_head + 1) % QLEN);
    }
}

/* =========================================================== CPU 사용률 */
void link_busy_begin(void)
{
#ifndef LINK_HOST_TEST
    LINK_IRQ_SAVE();
    if (busy_depth++ == 0) busy_start = DWT->CYCCNT;
    LINK_IRQ_RESTORE();
#endif
}
void link_busy_end(void)
{
#ifndef LINK_HOST_TEST
    LINK_IRQ_SAVE();
    if (busy_depth && --busy_depth == 0) busy_cyc += DWT->CYCCNT - busy_start;
    LINK_IRQ_RESTORE();
#endif
}
void link_miss_add(uint32_t n) { miss_cnt += n; }

static void print_stat(uint32_t now)
{
#if LINK_STAT_MS > 0
    uint32_t dt = now - stat_t0;
    if (dt < LINK_STAT_MS) return;
    uint32_t cyc, miss;
    { LINK_IRQ_SAVE(); cyc = busy_cyc; busy_cyc = 0; miss = miss_cnt; miss_cnt = 0; LINK_IRQ_RESTORE(); }
    stat_t0 = now;
    uint32_t per_ms = SystemCoreClock / 1000u;
    uint32_t tenths = per_ms ? (uint32_t)(((uint64_t)cyc * 1000u) / ((uint64_t)per_ms * dt)) : 0;  /* 0.1% 단위 */
    /* I2C 한 번에 31바이트 안에 들어가게 짧게: #S,D,DMA_OFF,cpu=100.0,miss=9999 (31자) */
    if (miss > 9999u) miss = 9999u;
    if (tenths > 1000u) tenths = 1000u;
    char b[48]; int n = 0;
    b[n++] = '#'; b[n++] = 'S'; b[n++] = ','; b[n++] = (char)LINK_INST; b[n++] = ',';
    n += put_str(b + n, dma_on ? "DMA_ON,cpu=" : "DMA_OFF,cpu=");
    n += put_int(b + n, (int32_t)(tenths / 10), 0); b[n++] = '.';
    n += put_int(b + n, (int32_t)(tenths % 10), 0);
    n += put_str(b + n, ",miss=");  n += put_int(b + n, (int32_t)miss, 0);
    b[n++] = '\r'; b[n++] = '\n';
    send(b, n);
#else
    (void)now;
#endif
}

/* =========================================================== 모드 버튼 B1 */
WEAK void link_on_mode_change(uint8_t on) { (void)on; }
uint8_t link_dma_on(void) { return dma_on; }

static void led_update(void)
{
#if LINK_LED_ENABLE && !defined(LINK_HOST_TEST)
    if (dma_on) GPIOA->BRR = (1u << 5); else GPIOA->BSRR = (1u << 5);
#endif
}

static uint8_t read_b1(void)
{
#ifndef LINK_HOST_TEST
    return (GPIOC->IDR & (1u << 13)) ? 1 : 0;    /* 안 누르면 1, 누르면 0 */
#else
    return 1;
#endif
}

static void poll_button(uint32_t now)
{
    uint8_t r = read_b1();
    if (r != btn_raw) { btn_raw = r; btn_t = now; return; }
    if (r == btn_stable || (now - btn_t) < 30) return;          /* 30ms 동안 같아야 인정 */
    btn_stable = r;
    if (r != 0) return;                                           /* 누르는 순간만 */
    if (any_pulse && (now - last_pulse_t) < LINK_SONG_GAP_MS) {   /* 곡 도중에는 안 바꿈: 한 판 = 한 모드 */
        link_debug("MODE locked: song is playing");
        return;
    }
    dma_on = !dma_on;
    link_on_mode_change(dma_on);
    led_update();
    print_mode();
}

/* =========================================================== 하드웨어 초기화 */
#ifndef LINK_HOST_TEST
/* =========================================================== I2C1 마스터 → Uno (레지스터 직접, RM0008 26장)
 * 리맵 핀 PB8 = SCL, PB9 = SDA, 100kHz. 세 보드가 같은 버스의 마스터라 충돌하면 하드웨어 중재(ARLO)로 한쪽이 지고, 진 쪽은 잠깐 뒤 다시 보낸다.
 * 시간 재기는 DWT 사이클 카운터(인터럽트가 꺼져 있어도 동작) */
#if LINK_I2C_ENABLE
/* 진단 숫자: UART에만 2초마다 "#I2C,..." 줄로 찍는다 (I2C가 안 될 때 원인을 보려고) */
static uint32_t i2c_ok, i2c_fail, i2c_af, i2c_arlo, i2c_berr, i2c_to_busy, i2c_to_step, i2c_reinit;
static uint8_t  i2c_lines_low;   /* 초기화 때 SCL/SDA가 HIGH로 안 올라왔으면 1 (풀업 없음·GND와 붙음) */

static void cfg_pin(GPIO_TypeDef *g, uint32_t pin, uint32_t nibble);

static void i2c_delay_us(uint32_t us)
{
    uint32_t t0 = DWT->CYCCNT, n = SystemCoreClock / 1000000u * us;
    while ((DWT->CYCCNT - t0) < n) { }
}

/* F103 I2C 결함(ES096 "analog filter may provide wrong value, locking BUSY flag") 풀기 + 버스 복구:
 * 핀을 잠깐 GPIO 오픈드레인으로 바꿔 SCL을 9번 흔들고(누가 SDA를 잡고 있으면 놓게), STOP 모양을 만든 뒤
 * 다시 대체기능으로 돌리고 SWRST */
static void i2c_bus_recover(void)
{
    I2C1->CR1 &= ~I2C_CR1_PE;
    GPIOB->BSRR = (1u << 8) | (1u << 9);                /* 둘 다 놓기(HIGH) */
    cfg_pin(GPIOB, 8, 0x7);                             /* 범용 오픈드레인 50MHz */
    cfg_pin(GPIOB, 9, 0x7);
    i2c_delay_us(10);
    for (int i = 0; i < 9 && !(GPIOB->IDR & (1u << 9)); i++) {   /* SDA가 LOW면 SCL 클럭 9번 */
        GPIOB->BRR = (1u << 8); i2c_delay_us(5);
        GPIOB->BSRR = (1u << 8); i2c_delay_us(5);
    }
    GPIOB->BRR  = (1u << 9); i2c_delay_us(5);           /* SDA LOW → SCL HIGH 상태에서 SDA HIGH = STOP */
    GPIOB->BSRR = (1u << 8); i2c_delay_us(5);
    GPIOB->BSRR = (1u << 9); i2c_delay_us(5);
    i2c_lines_low = ((GPIOB->IDR & ((1u << 8) | (1u << 9))) != ((1u << 8) | (1u << 9)));
    cfg_pin(GPIOB, 8, 0xF);                             /* 대체기능 오픈드레인 50MHz */
    cfg_pin(GPIOB, 9, 0xF);
}

static void i2c_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN | RCC_APB2ENR_AFIOEN;
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
    AFIO->MAPR   |= AFIO_MAPR_I2C1_REMAP;               /* I2C1 → PB8/PB9 */
    i2c_bus_recover();
    I2C1->CR1 = I2C_CR1_SWRST;                          /* BUSY가 굳은 상태를 푼다 */
    I2C1->CR1 = 0;
    extern const uint8_t APBPrescTable[8];
    uint32_t pclk1 = SystemCoreClock >> APBPrescTable[(RCC->CFGR & RCC_CFGR_PPRE1) >> RCC_CFGR_PPRE1_Pos];
    uint32_t mhz = pclk1 / 1000000u;
    I2C1->CR2   = mhz;                                  /* FREQ */
    I2C1->CCR   = pclk1 / (2u * 100000u);               /* 표준 모드 100kHz */
    I2C1->TRISE = mhz + 1u;
    I2C1->CR1   = I2C_CR1_PE;
}

static uint8_t i2c_wait(volatile uint32_t *reg, uint32_t mask, uint32_t t0, uint32_t limit)
{
    while (!(*reg & mask)) {
        if (I2C1->SR1 & (I2C_SR1_ARLO | I2C_SR1_BERR | I2C_SR1_AF)) return 0;
        if ((DWT->CYCCNT - t0) > limit) return 0;
    }
    return 1;
}

static int i2c_write_once(const char *s, int n)
{
    const uint32_t limit = SystemCoreClock / 1000u * LINK_I2C_TIMEOUT_MS;
    const uint32_t t0 = DWT->CYCCNT;
    while (I2C1->SR2 & I2C_SR2_BUSY) {                  /* 다른 보드가 보내는 중 */
        if ((DWT->CYCCNT - t0) > limit) { i2c_to_busy++; goto fail; }
    }
    I2C1->CR1 |= I2C_CR1_START;
    if (!i2c_wait(&I2C1->SR1, I2C_SR1_SB, t0, limit)) goto fail;
    I2C1->DR = (uint8_t)(LINK_I2C_ADDR << 1);           /* 쓰기 */
    if (!i2c_wait(&I2C1->SR1, I2C_SR1_ADDR, t0, limit)) goto fail;
    (void)I2C1->SR2;                                    /* SR1 → SR2 읽기로 ADDR 지움 */
    for (int i = 0; i < n; i++) {
        if (!i2c_wait(&I2C1->SR1, I2C_SR1_TXE, t0, limit)) goto fail;
        I2C1->DR = (uint8_t)s[i];
    }
    if (!i2c_wait(&I2C1->SR1, I2C_SR1_BTF, t0, limit)) goto fail;
    I2C1->CR1 |= I2C_CR1_STOP;
    i2c_ok++;
    return 0;
fail:
    {
        uint32_t sr1 = I2C1->SR1;
        if (sr1 & I2C_SR1_AF) i2c_af++;                 /* NACK: Uno가 대답 안 함 (주소·SDA/SCL·Uno 코드) */
        else if (sr1 & I2C_SR1_ARLO) i2c_arlo++;        /* 다른 보드와 부딪힘 (정상적으로 생길 수 있음) */
        else if (sr1 & I2C_SR1_BERR) i2c_berr++;        /* 버스 오류: 잡음·배선 */
        else if (!(I2C1->SR2 & I2C_SR2_BUSY) || (DWT->CYCCNT - t0) > limit) i2c_to_step++;
        if (sr1 & I2C_SR1_ARLO) {                       /* 중재에서 짐: 버스는 다른 보드 것. STOP 안 보냄 */
            I2C1->SR1 = ~I2C_SR1_ARLO;
        } else {
            I2C1->SR1 = ~(I2C_SR1_AF | I2C_SR1_BERR);   /* NACK(Uno 없음)·버스 오류 지우고 STOP */
            I2C1->CR1 |= I2C_CR1_STOP;
        }
        uint32_t t1 = DWT->CYCCNT;
        while ((I2C1->CR1 & I2C_CR1_STOP) && (DWT->CYCCNT - t1) < limit) { }
        if (!(sr1 & I2C_SR1_ARLO) && (I2C1->SR2 & I2C_SR2_BUSY)) { i2c_reinit++; i2c_init(); }   /* 굳었으면 다시 */
    }
    return -1;
}

static void i2c_send(const char *s, int n)
{
    for (int k = 0; k < 3; k++) {                       /* 최대 3번 */
        if (i2c_write_once(s, n) == 0) return;
        uint32_t back = SystemCoreClock / 10000u * (uint32_t)(3 + (LINK_INST % 5));   /* 보드마다 다른 대기(0.3~0.7ms) */
        uint32_t t0 = DWT->CYCCNT;
        while ((DWT->CYCCNT - t0) < back) { }
    }
    i2c_fail++;
}

/* UART에만: #I2C,ok=..,fail=..,af=..,arlo=..,berr=..,busy=..,step=..,re=..,low=..,lines=SCL/SDA */
static void i2c_diag(uint32_t now)
{
    static uint32_t t;
    if ((now - t) < 2000u) return;
    t = now;
    char b[120]; int n = 0;
    n += put_str(b + n, "#I2C,ok=");  n += put_int(b + n, (int32_t)i2c_ok, 0);
    n += put_str(b + n, ",fail=");    n += put_int(b + n, (int32_t)i2c_fail, 0);
    n += put_str(b + n, ",af=");      n += put_int(b + n, (int32_t)i2c_af, 0);
    n += put_str(b + n, ",arlo=");    n += put_int(b + n, (int32_t)i2c_arlo, 0);
    n += put_str(b + n, ",berr=");    n += put_int(b + n, (int32_t)i2c_berr, 0);
    n += put_str(b + n, ",busy=");    n += put_int(b + n, (int32_t)i2c_to_busy, 0);
    n += put_str(b + n, ",step=");    n += put_int(b + n, (int32_t)i2c_to_step, 0);
    n += put_str(b + n, ",re=");      n += put_int(b + n, (int32_t)i2c_reinit, 0);
    n += put_str(b + n, ",low=");     n += put_int(b + n, (int32_t)i2c_lines_low, 0);
    n += put_str(b + n, ",lines=");
    b[n++] = (GPIOB->IDR & (1u << 8)) ? '1' : '0'; b[n++] = '/';
    b[n++] = (GPIOB->IDR & (1u << 9)) ? '1' : '0';
    b[n++] = '\r'; b[n++] = '\n';
    link_uart_write(b, n);
}
#else
__attribute__((unused)) static void i2c_send(const char *s, int n) { (void)s; (void)n; }
#endif

void link_beat_isr(void)
{
    if (EXTI->PR & (1u << LINK_BEAT_PIN)) {
        EXTI->PR = (1u << LINK_BEAT_PIN);          /* 1을 써서 지움 */
        on_pulse(link_millis());
    }
}
#if LINK_DEFINE_EXTI_HANDLER
void EXTI9_5_IRQHandler(void) { link_beat_isr(); }
#endif

WEAK uint32_t link_millis(void) { return HAL_GetTick ? HAL_GetTick() : 0u; }

WEAK void link_uart_write(const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        while (!(USART2->SR & USART_SR_TXE)) { }
        USART2->DR = (uint8_t)s[i];
    }
}

static void cfg_pin(GPIO_TypeDef *g, uint32_t pin, uint32_t nibble)
{
    volatile uint32_t *cr = pin < 8 ? &g->CRL : &g->CRH;
    uint32_t sh = (pin & 7u) * 4u;
    *cr = (*cr & ~(0xFu << sh)) | (nibble << sh);
}

void link_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPCEN | RCC_APB2ENR_AFIOEN;

#if LINK_INIT_USART2
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    cfg_pin(GPIOA, 2, 0xA);                         /* PA2 TX: 대체기능 푸시풀 2MHz */
    cfg_pin(GPIOA, 3, 0x4);                         /* PA3 RX: 입력 */
    {
        extern const uint8_t APBPrescTable[8];
        uint32_t pclk1 = SystemCoreClock >> APBPrescTable[(RCC->CFGR & RCC_CFGR_PPRE1) >> RCC_CFGR_PPRE1_Pos];
        USART2->BRR = (pclk1 + 57600u) / 115200u;
        USART2->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
    }
#endif

    /* B1 = PC13: 풀업 입력 (보드 풀업 + 내부 풀업). EXTI 설정은 건드리지 않음 */
    cfg_pin(GPIOC, 13, 0x8);
    GPIOC->BSRR = (1u << 13);

#if LINK_LED_ENABLE
    cfg_pin(GPIOA, 5, 0x2);                         /* LD2 = PA5: 출력 푸시풀 2MHz */
#endif
    led_update();

    /* 박자 입력: 풀다운 입력(CNF=10, MODE=00 -> 0x8, ODR=0). 선이 빠져도 가짜 박자 없음 */
    cfg_pin(LINK_BEAT_GPIO, LINK_BEAT_PIN, 0x8);
    LINK_BEAT_GPIO->BRR = (1u << LINK_BEAT_PIN);

    /* EXTI5~7은 AFIO_EXTICR2(EXTICR[1]), EXTI8~9는 EXTICR3(EXTICR[2]). 핀당 4비트 = 포트 번호 */
    {
        uint32_t idx = LINK_BEAT_PIN / 4u, sh = (LINK_BEAT_PIN % 4u) * 4u;
        AFIO->EXTICR[idx] = (AFIO->EXTICR[idx] & ~(0xFu << sh)) | ((uint32_t)LINK_BEAT_PORTSRC << sh);
    }
    EXTI->RTSR |=  (1u << LINK_BEAT_PIN);           /* 올라가는 순간 */
    EXTI->FTSR &= ~(1u << LINK_BEAT_PIN);
    EXTI->PR    =  (1u << LINK_BEAT_PIN);
    EXTI->IMR  |=  (1u << LINK_BEAT_PIN);
    NVIC_SetPriority(EXTI9_5_IRQn, 0);              /* 박자는 제일 먼저 */
    NVIC_EnableIRQ(EXTI9_5_IRQn);

    /* DWT 사이클 카운터 (CPU 사용률, I2C 시간 재기). 다른 코드가 쓰고 있을 수 있어 0으로 지우지 않음 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

#if LINK_I2C_ENABLE
    i2c_init();
#endif

    stat_t0 = link_millis();
    link_debug("ensemble_link start");
    print_mode();
}
#else
__attribute__((unused)) static void i2c_send(const char *s, int n) { (void)s; (void)n; }
void link_init(void) { stat_t0 = link_millis(); print_mode(); }
void link_beat_isr(void) {}
void link__pulse(uint32_t t) { on_pulse(t); }      /* 호스트 시험: 펄스 흉내 */
void link__set_mode(uint8_t on) { dma_on = on; print_mode(); }   /* 호스트 시험: B1 흉내 */
#endif

/* =========================================================== 메인 루프 */
void link_loop(void)
{
    uint32_t now = link_millis();
    if (new_song_flag) { new_song_flag = 0; print_mode(); }   /* 곡 시작마다 판 이름 다시 */
    drain_events();
    poll_button(now);
    print_stat(now);
#if LINK_I2C_ENABLE && LINK_UART_ENABLE && !defined(LINK_HOST_TEST)
    i2c_diag(now);
#endif
}
