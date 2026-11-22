/* NUCLEO-F103RB, 1 Mohm piezo inputs, PAM8403 GF1002 amplifier.
 * PA0/A0 = kick, PA1/A1 = snare, PB6/D10 = filtered audio PWM.
 * Uses CMSIS register definitions, without HAL/LL peripheral calls.
 * See DRUM_README.txt for wiring and the current button/audio test mode.
 */
#include "stm32f103xb.h"
#include "drum_samples.h"
#include "ensemble_link.h"   /* [합주] B1 DMA 전환·박자 펄스·게임 줄 (설정: Core/Inc/ensemble_link_cfg.h) */
#include <stdbool.h>
#include <stdint.h>

/* Adjust these after observing the ADC peaks of the assembled pads. */
#define KICK_THRESHOLD          60U
#define SNARE_THRESHOLD         60U
#define RELEASE_THRESHOLD       20U
#define VELOCITY_FULL_SCALE   1600U
#define KICK_RETRIGGER_MS       35U
#define SNARE_RETRIGGER_MS      30U
#define MASTER_VOLUME_PERCENT  40U
#define SERIAL_DIAGNOSTICS       1U  /* [합주] 진단 줄은 # 로 시작 → 게임이 무시 */
#define DRUM_B1_TEST             0U  /* [합주] B1은 DMA ON/OFF 전환에 씀. 테스트음은 시리얼 T 키로 */
#define BUTTON_AUDIO_TEST_ONLY   0U  /* Piezo pads enabled; B1 test remains available. */

#define CORE_CLOCK_HZ      64000000U
#define APB1_CLOCK_HZ      32000000U
#define ADC_FRAME_HZ           8000U
#define PWM_CLOCK_HZ         250000U
#define PWM_PERIOD             256U
#define PWM_REPEAT               8U
#define AUDIO_HALF_SAMPLES       64U
#define AUDIO_HALF_WORDS (AUDIO_HALF_SAMPLES * PWM_REPEAT)
#define AUDIO_BUFFER_WORDS (2U * AUDIO_HALF_WORDS)
#define ADC_HALF_FRAMES          16U
#define ADC_BUFFER_WORDS (2U * ADC_HALF_FRAMES * 2U)
#define PEAK_WINDOW_FRAMES       16U  /* 2 ms, then determine velocity. */
#define RELEASE_QUIET_FRAMES      8U
#define STARTUP_IGNORE_FRAMES (ADC_FRAME_HZ / 2U)
#define VOICE_COUNT               4U
#define HIT_QUEUE_LENGTH         16U
#define FAULT_ADC_DMA             1U
#define FAULT_AUDIO_DMA           2U
#define FAULT_MISSED_ADC          4U
#define FAULT_MISSED_AUDIO        8U

_Static_assert(DRUM_SAMPLE_RATE == PWM_CLOCK_HZ / PWM_REPEAT,
               "PCM and PWM sample rates must match");
_Static_assert(RELEASE_THRESHOLD < KICK_THRESHOLD &&
               RELEASE_THRESHOLD < SNARE_THRESHOLD,
               "Release threshold must be below both hit thresholds");
_Static_assert(VELOCITY_FULL_SCALE > KICK_THRESHOLD &&
               VELOCITY_FULL_SCALE > SNARE_THRESHOLD,
               "Velocity full scale must exceed both thresholds");
_Static_assert(MASTER_VOLUME_PERCENT <= 100U, "Volume must be 0..100");

typedef struct {
    uint16_t threshold;
    uint16_t holdoff_frames;
    uint16_t cooldown;
    uint16_t peak;
    uint8_t capture_left;
    uint8_t quiet_frames;
    bool armed;
} Pad;

typedef struct {
    uint8_t pad;
    uint16_t gain;
} Hit;

typedef struct {
    const int16_t *samples;
    uint32_t length;
    uint32_t position;
    uint16_t gain;
} Voice;

static Pad pads[2] = {
    {.threshold = KICK_THRESHOLD,
     .holdoff_frames = KICK_RETRIGGER_MS * ADC_FRAME_HZ / 1000U},
    {.threshold = SNARE_THRESHOLD,
     .holdoff_frames = SNARE_RETRIGGER_MS * ADC_FRAME_HZ / 1000U},
};
static Voice voices[VOICE_COUNT];
static volatile Hit hit_queue[HIT_QUEUE_LENGTH];
static volatile uint32_t hit_write;
static volatile uint32_t hit_read;
static volatile uint32_t manual_hits;
static volatile uint32_t milliseconds;
uint32_t link_millis(void) { return milliseconds; }   /* [합주] 모듈 시계 = 이 코드의 SysTick */
static volatile uint32_t led_until_ms;
static volatile uint16_t adc_buffer[ADC_BUFFER_WORDS];
static volatile uint16_t pwm_buffer[AUDIO_BUFFER_WORDS];

/* Watch these in CubeIDE's Live Expressions or read serial diagnostics. */
volatile uint16_t drum_adc_latest[2];
volatile uint16_t drum_adc_peak[2];
volatile uint16_t drum_last_hit_peak[2];
volatile uint32_t drum_hit_count[2];
volatile uint32_t drum_queue_drops;
volatile uint32_t drum_fault;
volatile uint32_t drum_adc_blocks;
volatile uint32_t drum_audio_blocks;
volatile uint32_t drum_adc_max_cycles;
volatile uint32_t drum_audio_max_cycles;
volatile uint32_t drum_button_count;
volatile uint32_t drum_test_count;
volatile uint32_t drum_play_count[2];
volatile uint16_t drum_pwm_min = 255U;
volatile uint16_t drum_pwm_max;

static void clock_init(void)
{
    RCC->CR |= RCC_CR_HSION;
    while ((RCC->CR & RCC_CR_HSIRDY) == 0U) { }
    RCC->CFGR &= ~RCC_CFGR_SW;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) { }
    RCC->CR &= ~RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) != 0U) { }
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;
    /* HSI/2 * 16 = 64 MHz. Timer clock = 64 MHz, ADC = 64/6 MHz. */
    RCC->CFGR = RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_ADCPRE_DIV6 |
                RCC_CFGR_PLLMULL16;
    RCC->CR |= RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) == 0U) { }
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) { }
    SystemCoreClock = CORE_CLOCK_HZ;
    /* Pause sampling and audio timing with the CPU at debugger breakpoints.
     * Otherwise DMA keeps wrapping while IRQs cannot run, latching fault 12.
     */
    DBGMCU->CR |= DBGMCU_CR_DBG_SLEEP |
                   DBGMCU_CR_DBG_TIM3_STOP | DBGMCU_CR_DBG_TIM4_STOP;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void delay_us(uint32_t us)
{
    const uint32_t started = DWT->CYCCNT;
    while ((uint32_t)(DWT->CYCCNT - started) < us * 64U) { }
}

static void gpio_uart_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPAEN |
                   RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPCEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    (void)RCC->APB1ENR;
    AFIO->MAPR = 0U;  /* Default TIM4, USART2 mapping, SWD remains enabled. */
    /* PA0/PA1 analog, PA2 UART TX, PA3 UART RX, PA5 LD2. */
    GPIOA->CRL = (GPIOA->CRL & ~0x00F0FFFFU) |
                  (0xAU << 8U) | (0x4U << 12U) | (0x2U << 20U);
    GPIOA->BRR = 1U << 5U;
    /* PB6 TIM4_CH1 alternate push-pull, 2 MHz GPIO output speed. */
    GPIOB->CRL = (GPIOB->CRL & ~(0xFU << 24U)) | (0xAU << 24U);
    /* B1 PC13, active low, internal pull-up in addition to board pull-up. */
    GPIOC->BSRR = 1U << 13U;
    GPIOC->CRH = (GPIOC->CRH & ~(0xFU << 20U)) | (0x8U << 20U);
    RCC->APB1RSTR |= RCC_APB1RSTR_USART2RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_USART2RST;
    USART2->BRR = (APB1_CLOCK_HZ + 115200U / 2U) / 115200U;
    USART2->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

static void uart_text(const char *text)
{
    while (*text != '\0') {
        while ((USART2->SR & USART_SR_TXE) == 0U) { }
        USART2->DR = (uint8_t)*text++;
    }
}

static void uart_number(uint32_t value)
{
    char digits[11];
    uint32_t count = 0U;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) {
        while ((USART2->SR & USART_SR_TXE) == 0U) { }
        USART2->DR = (uint8_t)digits[--count];
    }
}

static void latch_fault(uint32_t fault)
{
    /* Keep the PWM carrier at its silent midpoint when a DMA deadline fails. */
    drum_fault |= fault;
    TIM3->CR1 &= ~TIM_CR1_CEN;
    TIM4->DIER = 0U;
    DMA1_Channel1->CCR &= ~DMA_CCR_EN;
    DMA1_Channel7->CCR &= ~DMA_CCR_EN;
    TIM4->CCR1 = PWM_PERIOD / 2U;
}

static void hit_push(uint8_t pad, uint16_t peak)
{
    const uint32_t write = hit_write;
    const uint32_t next = (write + 1U) % HIT_QUEUE_LENGTH;
    if (next == hit_read) {
        ++drum_queue_drops;
        return;
    }
    const uint32_t limited = peak > VELOCITY_FULL_SCALE ?
                             VELOCITY_FULL_SCALE : peak;
    hit_queue[write].pad = pad;
    hit_queue[write].gain = (uint16_t)(64U +
        (limited - pads[pad].threshold) * 192U /
        (VELOCITY_FULL_SCALE - pads[pad].threshold));
    __DMB();
    hit_write = next;
    drum_last_hit_peak[pad] = peak;
    ++drum_hit_count[pad];
    /* [합주] 게임 줄: 세기 0~100 = gain과 같은 비율. 2ms 피크 창 지연은 cfg의 LINK_LATENCY_MS로 뺌 */
    link_note_on(pad == 0U ? "KICK" : "SNARE",
                 (uint8_t)((limited - pads[pad].threshold) * 100U /
                           (VELOCITY_FULL_SCALE - pads[pad].threshold)));
}

static void pad_update(uint8_t index, uint16_t value)
{
    Pad *pad = &pads[index];
    if (pad->cooldown != 0U) {
        --pad->cooldown;
    }
    if (value <= RELEASE_THRESHOLD) {
        if (pad->quiet_frames < RELEASE_QUIET_FRAMES) {
            ++pad->quiet_frames;
        }
    } else {
        pad->quiet_frames = 0U;
    }
    if (pad->capture_left != 0U) {
        if (value > pad->peak) {
            pad->peak = value;
        }
        if (--pad->capture_left == 0U) {
            hit_push(index, pad->peak);
        }
        return;
    }
    if (pad->cooldown == 0U &&
        pad->quiet_frames >= RELEASE_QUIET_FRAMES) {
        pad->armed = true;
    }
    if (pad->armed && value >= pad->threshold) {
        pad->armed = false;
        pad->peak = value;
        pad->capture_left = PEAK_WINDOW_FRAMES - 1U;
        pad->cooldown = pad->holdoff_frames;
    }
}

static void adc_process(uint32_t offset)
{
    static uint32_t settling = STARTUP_IGNORE_FRAMES;
    for (uint32_t frame = 0U; frame < ADC_HALF_FRAMES; ++frame) {
        for (uint32_t pad = 0U; pad < 2U; ++pad) {
            const uint16_t value = adc_buffer[offset + 2U * frame + pad];
            drum_adc_latest[pad] = value;
            if (value > drum_adc_peak[pad]) {
                drum_adc_peak[pad] = value;
            }
            if (settling == 0U && BUTTON_AUDIO_TEST_ONLY == 0U) {
                pad_update((uint8_t)pad, value);
            }
        }
        if (settling != 0U) {
            --settling;
        }
    }
    ++drum_adc_blocks;
}

static void voice_start(uint8_t pad, uint16_t gain)
{
    /* Two voices per pad allow a short tail to overlap the next hit. */
    uint32_t chosen = pad * 2U;
    if (voices[chosen].position < voices[chosen].length &&
        voices[chosen + 1U].position >= voices[chosen + 1U].length) {
        ++chosen;
    } else if (voices[chosen].position < voices[chosen].length &&
               voices[chosen + 1U].position > voices[chosen].position) {
        ++chosen;
    }
    voices[chosen].samples = pad == 0U ? kick_pcm : snare_pcm;
    voices[chosen].length = pad == 0U ? KICK_PCM_LENGTH : SNARE_PCM_LENGTH;
    voices[chosen].position = 0U;
    voices[chosen].gain = gain;
    ++drum_play_count[pad];
    led_until_ms = milliseconds + 250U;
}

static void audio_fill(uint32_t offset)
{
    uint16_t minimum = 255U;
    uint16_t maximum = 0U;
    /* Only this IRQ owns playback state. ADC publishes hits through the queue. */
    while (hit_read != hit_write) {
        const uint32_t read = hit_read;
        __DMB();
        const uint8_t pad = hit_queue[read].pad;
        const uint16_t gain = hit_queue[read].gain;
        __DMB();
        hit_read = (read + 1U) % HIT_QUEUE_LENGTH;
        voice_start(pad, gain);
    }
    const uint32_t manual = manual_hits;
    manual_hits = 0U;
    for (uint32_t pad = 0U; pad < 2U; ++pad) {
        if ((manual & (1U << pad)) != 0U) {
            voice_start((uint8_t)pad, 192U);
        }
    }
    for (uint32_t sample = 0U; sample < AUDIO_HALF_SAMPLES; ++sample) {
        int32_t mixed = 0;
        for (uint32_t voice = 0U; voice < VOICE_COUNT; ++voice) {
            Voice *v = &voices[voice];
            if (v->position < v->length) {
                mixed += (int32_t)v->samples[v->position++] * v->gain / 256;
            }
        }
        mixed = mixed * (int32_t)MASTER_VOLUME_PERCENT / 100;
        if (mixed > 32767) { mixed = 32767; }
        if (mixed < -32767) { mixed = -32767; }
        const uint16_t duty = (uint16_t)(128 + mixed / 256);
        if (duty < minimum) { minimum = duty; }
        if (duty > maximum) { maximum = duty; }
        for (uint32_t repeat = 0U; repeat < PWM_REPEAT; ++repeat) {
            pwm_buffer[offset + sample * PWM_REPEAT + repeat] = duty;
        }
    }
    if (minimum < drum_pwm_min) { drum_pwm_min = minimum; }
    if (maximum > drum_pwm_max) { drum_pwm_max = maximum; }
    __DMB();
    ++drum_audio_blocks;
}

static void adc_dma_init(void)
{
    RCC->AHBENR |= RCC_AHBENR_DMA1EN;
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
    RCC->APB2RSTR |= RCC_APB2RSTR_ADC1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_ADC1RST;
    RCC->APB1RSTR |= RCC_APB1RSTR_TIM3RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_TIM3RST;
    /* TIM3 update event -> TRGO -> one two-channel ADC sequence at 8 kHz. */
    TIM3->PSC = 63U;
    TIM3->ARR = 124U;
    TIM3->EGR = TIM_EGR_UG;
    TIM3->SR = 0U;
    TIM3->CR2 = TIM_CR2_MMS_1;
    DMA1_Channel1->CCR = 0U;
    DMA1->IFCR = DMA_IFCR_CGIF1;
    DMA1_Channel1->CPAR = (uint32_t)&ADC1->DR;
    DMA1_Channel1->CMAR = (uint32_t)adc_buffer;
    DMA1_Channel1->CNDTR = ADC_BUFFER_WORDS;
    DMA1_Channel1->CCR = DMA_CCR_MINC | DMA_CCR_CIRC |
        DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0 | DMA_CCR_PL_1 |
        DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_TEIE;
    ADC1->CR1 = ADC_CR1_SCAN;
    /* 71.5 ADC cycles per input, allows the series 22 kohm input to settle. */
    ADC1->SMPR2 = (6U << 0U) | (6U << 3U);
    ADC1->SQR1 = 1U << 20U; /* Two conversions. */
    ADC1->SQR3 = 0U | (1U << 5U); /* PA0 then PA1. */
    ADC1->CR2 = ADC_CR2_EXTSEL_2 | ADC_CR2_DMA | ADC_CR2_ADON;
    delay_us(10U);
    ADC1->CR2 |= ADC_CR2_RSTCAL;
    while ((ADC1->CR2 & ADC_CR2_RSTCAL) != 0U) { }
    ADC1->CR2 |= ADC_CR2_CAL;
    while ((ADC1->CR2 & ADC_CR2_CAL) != 0U) { }
    DMA1_Channel1->CCR |= DMA_CCR_EN;
    ADC1->CR2 |= ADC_CR2_EXTTRIG;
    NVIC_ClearPendingIRQ(DMA1_Channel1_IRQn);
    NVIC_SetPriority(DMA1_Channel1_IRQn, 1U);
    NVIC_EnableIRQ(DMA1_Channel1_IRQn);
}

static void audio_dma_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM4EN;
    RCC->APB1RSTR |= RCC_APB1RSTR_TIM4RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_TIM4RST;
    for (uint32_t i = 0U; i < AUDIO_BUFFER_WORDS; ++i) {
        pwm_buffer[i] = PWM_PERIOD / 2U;
    }
    TIM4->PSC = 0U;
    TIM4->ARR = PWM_PERIOD - 1U;
    TIM4->CCR1 = PWM_PERIOD / 2U;
    TIM4->CCMR1 = (6U << 4U) | TIM_CCMR1_OC1PE;
    TIM4->CCER = TIM_CCER_CC1E;
    TIM4->CR1 = TIM_CR1_ARPE;
    TIM4->EGR = TIM_EGR_UG;
    TIM4->SR = 0U;
    /* TIM4 update DMA request is mapped to DMA1 channel 7 on STM32F103RB. */
    DMA1_Channel7->CCR = 0U;
    DMA1->IFCR = DMA_IFCR_CGIF7;
    DMA1_Channel7->CPAR = (uint32_t)&TIM4->CCR1;
    DMA1_Channel7->CMAR = (uint32_t)pwm_buffer;
    DMA1_Channel7->CNDTR = AUDIO_BUFFER_WORDS;
    DMA1_Channel7->CCR = DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_CIRC |
        DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0 | DMA_CCR_PL |
        DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_TEIE | DMA_CCR_EN;
    TIM4->DIER = TIM_DIER_UDE;
    NVIC_ClearPendingIRQ(DMA1_Channel7_IRQn);
    NVIC_SetPriority(DMA1_Channel7_IRQn, 0U);
    NVIC_EnableIRQ(DMA1_Channel7_IRQn);
}

static void adc_dma_isr_body(void)
{
    const uint32_t started = DWT->CYCCNT;
    const uint32_t flags = DMA1->ISR;
    DMA1->IFCR = DMA_IFCR_CGIF1;
    if ((flags & DMA_ISR_TEIF1) != 0U) {
        latch_fault(FAULT_ADC_DMA);
        return;
    }
    if ((flags & (DMA_ISR_HTIF1 | DMA_ISR_TCIF1)) ==
                 (DMA_ISR_HTIF1 | DMA_ISR_TCIF1)) {
        latch_fault(FAULT_MISSED_ADC);
        return;
    }
    if ((flags & DMA_ISR_HTIF1) != 0U) { adc_process(0U); }
    if ((flags & DMA_ISR_TCIF1) != 0U) { adc_process(ADC_HALF_FRAMES * 2U); }
    const uint32_t elapsed = DWT->CYCCNT - started;
    if (elapsed > drum_adc_max_cycles) { drum_adc_max_cycles = elapsed; }
    if (elapsed >= CORE_CLOCK_HZ / ADC_FRAME_HZ * ADC_HALF_FRAMES) {
        latch_fault(FAULT_MISSED_ADC);
    }
}

void DMA1_Channel1_IRQHandler(void)   /* [합주] DMA ON: 센서 처리 시간을 #STAT cpu 에 넣음 */
{
    link_busy_begin();
    adc_dma_isr_body();
    link_busy_end();
}

/* ===== [합주] DMA OFF(폴링): DMA가 하던 "ADC 값 옮기기"만 CPU가 직접 =====
 * 같은 것: TIM3 8kHz 주기, 같은 adc_buffer 자리, 같은 adc_process()(타격 판정)
 * 다른 것: DMA1 채널1 대신, 메인 루프가 TIM3 넘침 플래그를 보고 채널 0·1을 하나씩 SWSTART → EOC 기다림
 * 소리(TIM4 + DMA1 채널7)는 두 모드 모두 그대로 */
static volatile uint8_t adc_polling;
static uint32_t poll_frame, poll_base, poll_last_cyc;

void link_on_mode_change(uint8_t on)   /* B1을 누르면 모듈이 부름 (곡 도중에는 안 부름) */
{
    const uint32_t previous = __get_PRIMASK();
    __disable_irq();
    if (on) {
        adc_polling = 0U;
        ADC1->CR2 &= ~ADC_CR2_EXTTRIG;
        DMA1_Channel1->CCR &= ~DMA_CCR_EN;
        DMA1->IFCR = DMA_IFCR_CGIF1;
        DMA1_Channel1->CNDTR = ADC_BUFFER_WORDS;      /* 버퍼 처음부터 */
        ADC1->CR1 = ADC_CR1_SCAN;
        ADC1->SQR1 = 1U << 20U;                       /* 두 채널 */
        ADC1->SQR3 = 0U | (1U << 5U);                 /* PA0 then PA1 */
        ADC1->CR2 = ADC_CR2_EXTSEL_2 | ADC_CR2_DMA | ADC_CR2_ADON;   /* TIM3 TRGO */
        NVIC_ClearPendingIRQ(DMA1_Channel1_IRQn);
        DMA1_Channel1->CCR |= DMA_CCR_EN;
        ADC1->CR2 |= ADC_CR2_EXTTRIG;
    } else {
        ADC1->CR2 &= ~ADC_CR2_EXTTRIG;                /* TIM3이 ADC를 깨우지 않게 */
        DMA1_Channel1->CCR &= ~DMA_CCR_EN;
        DMA1->IFCR = DMA_IFCR_CGIF1;
        NVIC_ClearPendingIRQ(DMA1_Channel1_IRQn);
        ADC1->CR1 = 0U;                               /* scan 끔: 한 채널씩 */
        ADC1->SQR1 = 0U;
        ADC1->CR2 = ADC_CR2_EXTSEL | ADC_CR2_ADON;    /* EXTSEL=111: SWSTART */
        ADC1->CR2 |= ADC_CR2_EXTTRIG;
        (void)ADC1->DR;
        TIM3->SR = ~TIM_SR_UIF;
        poll_frame = 0U; poll_base = 0U; poll_last_cyc = DWT->CYCCNT;
        adc_polling = 1U;
    }
    __set_PRIMASK(previous);
}

static uint16_t adc_poll_one(uint32_t channel)
{
    ADC1->SQR3 = channel;
    ADC1->CR2 |= ADC_CR2_SWSTART;
    while ((ADC1->SR & ADC_SR_EOC) == 0U) { }
    return (uint16_t)ADC1->DR;
}

static void adc_poll_service(void)    /* DMA OFF일 때 메인 루프에서 계속 부름 */
{
    if ((TIM3->SR & TIM_SR_UIF) == 0U) { return; }
    TIM3->SR = ~TIM_SR_UIF;
    const uint32_t period = CORE_CLOCK_HZ / ADC_FRAME_HZ;     /* 8000사이클 = 125us */
    const uint32_t now = DWT->CYCCNT;
    const uint32_t gap = now - poll_last_cyc;
    poll_last_cyc = now;
    if (gap > period + period / 2U) { link_miss_add(gap / period - 1U); }   /* 늦게 와서 놓친 프레임 */
    link_busy_begin();
    adc_buffer[poll_base + 2U * poll_frame + 0U] = adc_poll_one(0U);
    adc_buffer[poll_base + 2U * poll_frame + 1U] = adc_poll_one(1U);
    if (++poll_frame == ADC_HALF_FRAMES) {                   /* 반 버퍼가 차면 DMA 때와 같은 처리 */
        adc_process(poll_base);
        poll_frame = 0U;
        poll_base = poll_base == 0U ? ADC_HALF_FRAMES * 2U : 0U;
    }
    link_busy_end();
}

void DMA1_Channel7_IRQHandler(void)
{
    const uint32_t started = DWT->CYCCNT;
    const uint32_t flags = DMA1->ISR;
    DMA1->IFCR = DMA_IFCR_CGIF7;
    if ((flags & DMA_ISR_TEIF7) != 0U) {
        latch_fault(FAULT_AUDIO_DMA);
        return;
    }
    if ((flags & (DMA_ISR_HTIF7 | DMA_ISR_TCIF7)) ==
                 (DMA_ISR_HTIF7 | DMA_ISR_TCIF7)) {
        latch_fault(FAULT_MISSED_AUDIO);
        return;
    }
    if ((flags & DMA_ISR_HTIF7) != 0U) { audio_fill(0U); }
    if ((flags & DMA_ISR_TCIF7) != 0U) { audio_fill(AUDIO_HALF_WORDS); }
    const uint32_t elapsed = DWT->CYCCNT - started;
    if (elapsed > drum_audio_max_cycles) { drum_audio_max_cycles = elapsed; }
    if (elapsed >= CORE_CLOCK_HZ / PWM_CLOCK_HZ * AUDIO_HALF_WORDS) {
        latch_fault(FAULT_MISSED_AUDIO);
    }
}

void SysTick_Handler(void)
{
    ++milliseconds;
}

static void request_manual_hits(uint32_t mask)
{
    const uint32_t previous = __get_PRIMASK();
    __disable_irq();
    manual_hits |= mask;
    __set_PRIMASK(previous);
}

static void report_hit_events(void)
{
    static uint32_t reported[2];
    uint32_t count[2];
    uint16_t peak[2];
    const uint32_t previous = __get_PRIMASK();
    __disable_irq();
    for (uint32_t pad = 0U; pad < 2U; ++pad) {
        count[pad] = drum_hit_count[pad];
        peak[pad] = drum_last_hit_peak[pad];
    }
    __set_PRIMASK(previous);
    for (uint32_t pad = 0U; pad < 2U; ++pad) {
        if (count[pad] == reported[pad]) { continue; }
        const uint32_t limited = peak[pad] > VELOCITY_FULL_SCALE ?
                                 VELOCITY_FULL_SCALE : peak[pad];
        const uint32_t gain = 64U + (limited - pads[pad].threshold) * 192U /
            (VELOCITY_FULL_SCALE - pads[pad].threshold);
        uart_text(pad == 0U ? "#HIT KICK N=" : "#HIT SNARE N=");
        uart_number(count[pad]);
        uart_text(" DELTA="); uart_number(count[pad] - reported[pad]);
        uart_text(" PEAK="); uart_number(peak[pad]);
        uart_text(" GAIN="); uart_number(gain);
        uart_text("/256 SAT=");
        uart_number(peak[pad] >= VELOCITY_FULL_SCALE ? 1U : 0U);
        uart_text("\r\n");
        reported[pad] = count[pad];
    }
}

static void report_status(void)
{
    uint16_t latest[2];
    uint16_t peak[2];
    uint16_t pwm_min;
    uint16_t pwm_max;
    const uint32_t previous = __get_PRIMASK();
    __disable_irq();
    for (uint32_t pad = 0U; pad < 2U; ++pad) {
        latest[pad] = drum_adc_latest[pad];
        peak[pad] = drum_adc_peak[pad];
        drum_adc_peak[pad] = 0U;
    }
    pwm_min = drum_pwm_min;
    pwm_max = drum_pwm_max;
    drum_pwm_min = 255U;
    drum_pwm_max = 0U;
    __set_PRIMASK(previous);
    uart_text("#ADC="); uart_number(latest[0]);
    uart_text(","); uart_number(latest[1]);
    uart_text(" PEAK="); uart_number(peak[0]);
    uart_text(","); uart_number(peak[1]);
    uart_text(" HITS="); uart_number(drum_hit_count[0]);
    uart_text(","); uart_number(drum_hit_count[1]);
    uart_text(" FAULT="); uart_number(drum_fault);
    uart_text(" DROP="); uart_number(drum_queue_drops);
    uart_text(" BTN="); uart_number((GPIOC->IDR >> 13U) & 1U);
    uart_text(" PRESS="); uart_number(drum_button_count);
    uart_text(" TEST="); uart_number(drum_test_count);
    uart_text(" PLAY="); uart_number(drum_play_count[0]);
    uart_text(","); uart_number(drum_play_count[1]);
    uart_text(" PWM="); uart_number(pwm_min);
    uart_text(","); uart_number(pwm_max);
    uart_text(" ADMA="); uart_number(drum_audio_blocks);
    uart_text("\r\n");
}

int main(void)
{
    __disable_irq();
    clock_init();
    gpio_uart_init();
    /* [합주] 박자 입력 PB5(EXTI5), B1, I2C, MODE 줄. 타이머·DMA를 켜기 전에 해야 한다:
     * 이 안에서 UART·I2C로 몇 ms 보내는 동안 인터럽트가 꺼져 있으면 DMA 반 버퍼(2ms)를 놓쳐 FAULT=12가 걸린다 */
    link_init();
    NVIC_SetPriorityGrouping(0U);
    adc_dma_init();
    audio_dma_init();
    NVIC_SetPriority(SysTick_IRQn, 2U);
    SysTick->LOAD = CORE_CLOCK_HZ / 1000U - 1U;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk |
                    SysTick_CTRL_ENABLE_Msk;
    TIM4->CR1 |= TIM_CR1_CEN;
    TIM3->CR1 |= TIM_CR1_CEN;
    __enable_irq();
    uart_text("#DRUM PIEZO ENABLED F103RB ADC1/DMA1_CH1 TIM4_UP/DMA1_CH7\r\n"
              "#B1 = DMA ON/OFF. T: kick/snare/both test. K/S/B: manual hits.\r\n");

    uint32_t last_button_ms = milliseconds;
    uint32_t button_changed_ms = milliseconds;
    uint32_t last_report_ms = milliseconds;
    uint32_t next_demo_ms = 0U;
    uint8_t demo_stage = 0U;
    bool button_raw = false;
    bool button_stable = false;
    for (;;) {
        const uint32_t now = milliseconds;
        if (DRUM_B1_TEST != 0U && now != last_button_ms) {
            last_button_ms = now;
            const bool pressed = (GPIOC->IDR & (1U << 13U)) == 0U;
            if (pressed != button_raw) {
                button_raw = pressed;
                button_changed_ms = now;
            }
            if (button_raw != button_stable &&
                (uint32_t)(now - button_changed_ms) >= 25U) {
                button_stable = button_raw;
                if (button_stable) {
                    ++drum_button_count;
                    ++drum_test_count;
                    demo_stage = 1U;
                    next_demo_ms = now;
                    uart_text("#B1 PRESS: start drum test\r\n");
                }
            }
        }
        if ((USART2->SR & USART_SR_RXNE) != 0U) {
            const uint8_t command = (uint8_t)USART2->DR;
            switch (command) {
            case 'K': case 'k': request_manual_hits(1U); break;
            case 'S': case 's': request_manual_hits(2U); break;
            case 'B': case 'b': request_manual_hits(3U); break;
            case 'T': case 't':
                ++drum_test_count;
                demo_stage = 1U; next_demo_ms = now;
                uart_text("#UART T: start drum test\r\n");
                break;
            default: break;
            }
        }
        if (demo_stage != 0U && (int32_t)(now - next_demo_ms) >= 0) {
            uart_text(demo_stage == 1U ? "#TEST KICK\r\n" :
                      demo_stage == 2U ? "#TEST SNARE\r\n" : "#TEST BOTH\r\n");
            request_manual_hits(demo_stage); /* 1=kick, 2=snare, 3=both. */
            demo_stage = demo_stage == 3U ? 0U : (uint8_t)(demo_stage + 1U);
            next_demo_ms = now + 700U;
        }
        const bool led_on = drum_fault != 0U ? (now / 150U) % 2U != 0U :
                            link_dma_on() == 0U;   /* [합주] LD2 켜짐 = DMA OFF (원래: 칠 때 0.25초) */
        GPIOA->BSRR = led_on ? 1U << 5U : 1U << 21U;
        if (SERIAL_DIAGNOSTICS != 0U) { report_hit_events(); }
        if (SERIAL_DIAGNOSTICS != 0U &&
            (uint32_t)(now - last_report_ms) >= 500U) {
            last_report_ms = now;
            report_status();
        }
        link_loop();                     /* [합주] 게임 줄 보내기, B1, #STAT */
        if (adc_polling != 0U) {
            adc_poll_service();          /* [합주] DMA OFF: CPU가 ADC를 직접 */
        } else {
            __WFI();
        }
    }
}
