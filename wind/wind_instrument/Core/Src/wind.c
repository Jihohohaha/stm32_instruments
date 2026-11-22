/*
 * wind.c - 관악기(STM32 #1) 본체 코드 : NUCLEO-F103RB
 * 주변장치 설정은 전부 레지스터 직접 조작 (HAL은 클럭/HAL_GetTick만 사용)
 */
#include "main.h"
#include "wind.h"
#include <stdio.h>
#include <stdint.h>

/* ---------------- 상수 ---------------- */
#define ADC_CH          3                     /* 마이크, 조이X, 조이Y */
#define FRAMES          128                   /* 배열 전체 = 128프레임 (16ms) */
#define HALF_FRAMES     (FRAMES / 2)          /* 반쪽 = 64프레임 = 8ms */
#define WAVE_LEN        64                    /* 사인파 표 칸 수 */
#define CAL_HALVES      250                   /* 시작 보정 시간: 250 x 8ms = 2초 */

enum { DIR_CENTER, DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };
static const char *const dir_name[5]  = { "CENTER", "UP", "RIGHT", "DOWN", "LEFT" };
static const char *const note_name[5] = { "DO", "RE", "MI", "FA", "SOL" };
static const uint8_t dir_note[5] = DIR_NOTE_MAP;

/* TIM4 주기 = 1,000,000 / 주파수  (64MHz / 64칸 = 1MHz)
 * 도4 261.63Hz, 레 293.66, 미 329.63, 파 349.23, 솔 392.00 */
static const uint16_t note_period[5] = { 3822, 3405, 3034, 2863, 2551 };

/* 사인파 한 주기, -32767 ~ +32767 */
static const int16_t sine_q15[WAVE_LEN] = {
         0,   3212,   6393,   9512,  12539,  15446,  18204,  20787,
     23170,  25329,  27245,  28898,  30273,  31356,  32137,  32609,
     32767,  32609,  32137,  31356,  30273,  28898,  27245,  25329,
     23170,  20787,  18204,  15446,  12539,   9512,   6393,   3212,
         0,  -3212,  -6393,  -9512, -12539, -15446, -18204, -20787,
    -23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609,
    -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329,
    -23170, -20787, -18204, -15446, -12539,  -9512,  -6393,  -3212,
};

/* ---------------- 버퍼 ---------------- */
static volatile uint16_t adc_buf[FRAMES * ADC_CH];   /* [마이크, X, Y] x 128 */
static volatile uint16_t wave[WAVE_LEN];             /* DMA가 CCR1로 보내는 표 */

/* ---------------- 상태 (로그 출력용) ---------------- */
static volatile uint16_t breath_amp;     /* 반쪽 배열의 마이크 최댓값-최솟값 */
static volatile uint16_t noise_floor;    /* 시작 2초 동안 잰 잡음 기준 */
static volatile uint16_t joy_x, joy_y;   /* 반쪽 배열 평균 */
static volatile uint16_t joy_cx, joy_cy; /* 조이스틱 가운데 값 */
static volatile uint8_t  cur_dir, cur_note, sound_on, octave_up;
static volatile int32_t  volume;         /* 0~100 */
static volatile uint16_t cal_count;
static volatile uint32_t cal_amp_sum, cal_x_sum, cal_y_sum;

static uint16_t cur_period;

/* ===================================================================
 *  printf -> USART2 (CubeIDE 기본 syscalls.c 의 _write 가 이 함수를 부름)
 * =================================================================== */
int __io_putchar(int ch)
{
    while (!(USART2->SR & USART_SR_TXE)) { }
    USART2->DR = (uint8_t)ch;
    return ch;
}

/* ===================================================================
 *  소리: 음 높이(TIM4 주기)와 음량(표 크기)을 바꾸고 표를 다시 계산
 * =================================================================== */
static void sound_update(uint16_t period, int32_t vol)
{
    if (period != cur_period) {
        cur_period = period;
        TIM4->ARR = period - 1;          /* ARPE=1 이라 다음 주기부터 적용 */
    }
    int32_t mid = period / 2;
    int32_t amp = mid * vol / 100 * 9 / 10;   /* 최대 90%까지만 */
    for (int i = 0; i < WAVE_LEN; i++) {
        wave[i] = (uint16_t)(mid + (sine_q15[i] * amp) / 32768);
    }
}

/* ===================================================================
 *  반쪽 배열(64프레임) 처리: 입김 세기, 조이스틱 방향 -> 소리
 * =================================================================== */
static void process_half(const volatile uint16_t *p)
{
    uint16_t mn = 4095, mx = 0;
    uint32_t sx = 0, sy = 0;

    for (int i = 0; i < HALF_FRAMES; i++) {
        uint16_t m = p[i * ADC_CH + 0];
        if (m < mn) mn = m;
        if (m > mx) mx = m;
        sx += p[i * ADC_CH + 1];
        sy += p[i * ADC_CH + 2];
    }
    breath_amp = mx - mn;
    joy_x = sx / HALF_FRAMES;
    joy_y = sy / HALF_FRAMES;
    octave_up = !(GPIOB->IDR & GPIO_IDR_IDR0);   /* SW 누르면 0 -> 한 옥타브 위 */

    /* 1) 시작 2초: 불지 말고, 조이스틱 건드리지 말고 기준값 측정 */
    if (cal_count < CAL_HALVES) {
        cal_amp_sum += breath_amp;
        cal_x_sum   += joy_x;
        cal_y_sum   += joy_y;
        if (++cal_count == CAL_HALVES) {
            noise_floor = cal_amp_sum / CAL_HALVES;
            joy_cx      = cal_x_sum   / CAL_HALVES;
            joy_cy      = cal_y_sum   / CAL_HALVES;
        }
        sound_update(note_period[0], 0);
        return;
    }

#if SOUND_TEST
    /* 센서 없이 스피커 확인: 0.5초마다 도레미파솔 */
    static uint16_t t;
    t++;
    cur_note = (t / 62) % 5;
    sound_on = 1;
    sound_update(note_period[cur_note], 60);
    return;
#endif

    /* 2) 입김: 켜짐/꺼짐 기준을 다르게 둬서 깜빡임 방지 */
    if (!sound_on && breath_amp > noise_floor + BREATH_ON)  sound_on = 1;
    if ( sound_on && breath_amp < noise_floor + BREATH_OFF) sound_on = 0;

    int32_t target = 0;
    if (sound_on) {
        int32_t span = BREATH_FULL - BREATH_OFF;
        target = ((int32_t)breath_amp - noise_floor - BREATH_OFF) * 100 / span;
        if (target < 10)  target = 10;
        if (target > 100) target = 100;
    }
    volume += (target - volume) / 4;          /* 갑자기 튀지 않게 부드럽게 */
    if (target == 0 && volume < 4) volume = 0;

    /* 3) 조이스틱: 가운데에서 많이 벗어난 축의 방향으로 판정 */
    int32_t dx = (int32_t)joy_x - joy_cx;
    int32_t dy = (int32_t)joy_y - joy_cy;
    int32_t ax = dx < 0 ? -dx : dx;
    int32_t ay = dy < 0 ? -dy : dy;
    if (ax < JOY_DEAD && ay < JOY_DEAD) cur_dir = DIR_CENTER;
    else if (ax > ay)                   cur_dir = dx > 0 ? DIR_RIGHT : DIR_LEFT;
    else                                cur_dir = dy > 0 ? DIR_UP : DIR_DOWN;
    cur_note = dir_note[cur_dir];

    uint16_t period = note_period[cur_note];
    if (octave_up) period /= 2;
    sound_update(period, volume);
}

/* DMA1 채널1 (ADC) : 반 찼을 때 / 다 찼을 때 */
void DMA1_Channel1_IRQHandler(void)
{
    uint32_t isr = DMA1->ISR;
    if (isr & DMA_ISR_HTIF1) {
        DMA1->IFCR = DMA_IFCR_CHTIF1;
        process_half(&adc_buf[0]);                       /* 앞쪽 반 */
    }
    if (isr & DMA_ISR_TCIF1) {
        DMA1->IFCR = DMA_IFCR_CTCIF1;
        process_half(&adc_buf[HALF_FRAMES * ADC_CH]);    /* 뒤쪽 반 */
    }
}

/* ===================================================================
 *  초기화 (전부 레지스터)
 * =================================================================== */
static void clock_enable(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN |
                    RCC_APB2ENR_AFIOEN | RCC_APB2ENR_ADC1EN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN | RCC_APB1ENR_TIM4EN |
                    RCC_APB1ENR_USART2EN;
    RCC->AHBENR  |= RCC_AHBENR_DMA1EN;

    /* ADC 클럭은 최대 14MHz -> 64MHz / 6 = 10.7MHz */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_ADCPRE) | RCC_CFGR_ADCPRE_DIV6;
}

static void gpio_init(void)
{
    /* GPIOA: 핀 하나당 4비트(CNF 2 + MODE 2)
     * PA0, PA1, PA4 = 0x0 아날로그 입력
     * PA2 (USART2 TX) = 0xA 대체기능 푸시풀 2MHz
     * PA3 (USART2 RX) = 0x4 입력 */
    GPIOA->CRL &= ~((0xFu << 0) | (0xFu << 4) | (0xFu << 8) |
                    (0xFu << 12) | (0xFu << 16));
    GPIOA->CRL |=  (0xAu << 8) | (0x4u << 12);

    /* GPIOB
     * PB0 (조이스틱 SW) = 0x8 풀업/풀다운 입력, ODR=1 이면 풀업
     * PB6 (TIM4_CH1 소리) = 0xB 대체기능 푸시풀 50MHz */
    GPIOB->CRL &= ~((0xFu << 0) | (0xFu << 24));
    GPIOB->CRL |=  (0x8u << 0) | (0xBu << 24);
    GPIOB->ODR |=  GPIO_ODR_ODR0;
}

static void usart2_init(void)
{
    USART2->BRR = 278;                 /* 32MHz / 115200 = 277.8 */
    USART2->CR1 = USART_CR1_TE | USART_CR1_UE;
    setvbuf(stdout, NULL, _IONBF, 0);  /* printf 바로 출력 */
}

static void adc_dma_init(void)
{
    /* DMA1 채널1: ADC1->DR -> adc_buf, 16비트, circular, 반/전체 인터럽트 */
    DMA1_Channel1->CCR   = 0;
    DMA1_Channel1->CPAR  = (uint32_t)&ADC1->DR;
    DMA1_Channel1->CMAR  = (uint32_t)adc_buf;
    DMA1_Channel1->CNDTR = FRAMES * ADC_CH;
    DMA1_Channel1->CCR   = DMA_CCR_PL_1 | DMA_CCR_MSIZE_0 | DMA_CCR_PSIZE_0 |
                           DMA_CCR_MINC | DMA_CCR_CIRC |
                           DMA_CCR_HTIE | DMA_CCR_TCIE;
    DMA1_Channel1->CCR  |= DMA_CCR_EN;
    NVIC_SetPriority(DMA1_Channel1_IRQn, 1);
    NVIC_EnableIRQ(DMA1_Channel1_IRQn);

    /* ADC1: scan 모드, 채널 순서 0 -> 1 -> 4 */
    ADC1->CR1   = ADC_CR1_SCAN;
    ADC1->SQR1  = (ADC_CH - 1) << ADC_SQR1_L_Pos;          /* 변환 3개 */
    ADC1->SQR3  = (0u << 0) | (1u << 5) | (4u << 10);       /* SQ1=0, SQ2=1, SQ3=4 */
    /* 샘플링 시간 55.5사이클(101): 채널 0, 1, 4 */
    ADC1->SMPR2 = (5u << 0) | (5u << 3) | (5u << 12);

    /* 전원 켜고 -> 보정(CAL) */
    ADC1->CR2 = ADC_CR2_ADON;
    for (volatile int i = 0; i < 1000; i++) { }
    ADC1->CR2 |= ADC_CR2_RSTCAL;
    while (ADC1->CR2 & ADC_CR2_RSTCAL) { }
    ADC1->CR2 |= ADC_CR2_CAL;
    while (ADC1->CR2 & ADC_CR2_CAL) { }

    /* DMA 사용 + 외부 트리거 = TIM3 TRGO (EXTSEL=100) */
    ADC1->CR2 |= ADC_CR2_DMA | ADC_CR2_EXTTRIG | ADC_CR2_EXTSEL_2;

    /* TIM3: 64MHz / 64 / 125 = 8kHz, 업데이트마다 TRGO */
    TIM3->PSC = 63;
    TIM3->ARR = 124;
    TIM3->CR2 = TIM_CR2_MMS_1;          /* MMS=010: update -> TRGO */
    TIM3->EGR = TIM_EGR_UG;
    TIM3->CR1 = TIM_CR1_CEN;
}

static void sound_init(void)
{
    cur_period = note_period[0];
    sound_update(cur_period, 0);

    /* DMA1 채널7: wave[] -> TIM4->CCR1, 메모리->주변장치, circular */
    DMA1_Channel7->CCR   = 0;
    DMA1_Channel7->CPAR  = (uint32_t)&TIM4->CCR1;
    DMA1_Channel7->CMAR  = (uint32_t)wave;
    DMA1_Channel7->CNDTR = WAVE_LEN;
    DMA1_Channel7->CCR   = DMA_CCR_PL | DMA_CCR_MSIZE_0 | DMA_CCR_PSIZE_0 |
                           DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_DIR;
    DMA1_Channel7->CCR  |= DMA_CCR_EN;

    /* TIM4 CH1 PWM: 주기마다 DMA 요청(UDE) */
    TIM4->PSC   = 0;
    TIM4->ARR   = cur_period - 1;
    TIM4->CCR1  = cur_period / 2;
    TIM4->CCMR1 = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1PE; /* PWM1 */
    TIM4->CCER  = TIM_CCER_CC1E;
    TIM4->DIER  = TIM_DIER_UDE;
    TIM4->CR1   = TIM_CR1_ARPE;
    TIM4->EGR   = TIM_EGR_UG;
    TIM4->CR1  |= TIM_CR1_CEN;
}

void wind_init(void)
{
    clock_enable();
    gpio_init();
    usart2_init();
    printf("\r\n=== wind instrument start ===\r\n");
    printf("calibrating 2s: don't blow, don't touch joystick\r\n");
    sound_init();
    adc_dma_init();
}

/* 0.1초마다 상태 출력 */
void wind_loop(void)
{
    static uint32_t last;
    static uint8_t cal_printed;

    if (HAL_GetTick() - last < 100) return;
    last = HAL_GetTick();

    if (cal_count < CAL_HALVES) return;
    if (!cal_printed) {
        cal_printed = 1;
        printf("cal done: noise=%u  joy center x=%u y=%u\r\n",
               noise_floor, joy_cx, joy_cy);
    }
    printf("amp=%4u (on>%4u) vol=%3ld | x=%4u y=%4u sw=%u | %-6s -> %s %s\r\n",
           breath_amp, noise_floor + BREATH_ON, (long)volume,
           joy_x, joy_y, octave_up,
           dir_name[cur_dir], note_name[cur_note],
           sound_on ? "ON" : "off");
}
