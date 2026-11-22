/*
 * ensemble_link.h - 센서 악기 합주: STM32 악기 보드 공통 연결 모듈 (NUCLEO-F103RB)
 *
 *  하는 일 (소리·센서 코드는 건드리지 않는다)
 *   1) B1(PC13) 버튼으로 DMA ON/OFF 전환 -> 시리얼에 MODE,DMA_ON / MODE,DMA_OFF, LD2(PA5) 켜짐 = DMA OFF
 *   2) Uno 박자 펄스를 PB5(EXTI5, 올라가는 순간)로 받아 박 번호를 센다
 *   3) 악기가 "쳤다/불었다"를 알려 주면 박 번호 + 오차ms를 계산해 리듬게임 줄로 보낸다
 *        D,ON,KICK,88,22,-3      (악기,이벤트,음,세기,박번호,오차ms)
 *   4) (선택) CPU 사용률을 DWT로 재서 1초마다 #S 줄   예) #S,D,DMA_OFF,cpu=12.3,miss=40
 *   줄은 I2C로 Uno에(Uno가 USB 한 줄로 노트북에), 그리고 디버그용으로 USART2에도 보낸다
 *
 *  보드 코드에 넣을 것 (자세한 건 README):
 *    main.c  USER CODE 2   : link_init();          (USART2 초기화 뒤)
 *    main.c  while(1)      : link_loop();
 *    감지한 곳(ISR 가능)    : link_note_on("KICK", 88);   관악기만 link_note_off("MI");
 *    DMA/폴링 분기          : if (link_dma_on()) { ...DMA 방식... } else { ...폴링 방식... }
 *    모드가 바뀔 때 할 일    : void link_on_mode_change(uint8_t dma_on) { ... }  (선택)
 */
#ifndef ENSEMBLE_LINK_H
#define ENSEMBLE_LINK_H

#include <stdint.h>

/* 보드마다 다른 값은 같은 폴더(Core/Inc)의 ensemble_link_cfg.h 에 적는다 (이 파일은 그대로 둔다).
 * 예) #define LINK_INST 'H'   — 하프 */
#if defined(__has_include)
#if __has_include("ensemble_link_cfg.h")
#include "ensemble_link_cfg.h"
#endif
#endif

/* ================= 기본값 (ensemble_link_cfg.h 에서 바꾼다) ================= */

#ifndef LINK_INST
#define LINK_INST            'D'    /* 'W' 관악기, 'H' 하프, 'D' 드럼 */
#endif

#ifndef LINK_START_DMA_ON
#define LINK_START_DMA_ON    1      /* 켜자마자 DMA ON(1) / OFF(0) */
#endif

/* 박자 입력 핀: 5~9번 핀만 (EXTI9_5 인터럽트 하나로 받음). 기본 PB5 = D4 */
#ifndef LINK_BEAT_GPIO
#define LINK_BEAT_GPIO       GPIOB
#endif
#ifndef LINK_BEAT_PORTSRC
#define LINK_BEAT_PORTSRC    1      /* AFIO_EXTICR 값: A=0, B=1, C=2 */
#endif
#ifndef LINK_BEAT_PIN
#define LINK_BEAT_PIN        5
#endif

/* CubeMX가 stm32f1xx_it.c 에 EXTI9_5_IRQHandler 를 이미 만들었으면 0으로 두고,
 * 그 함수의 USER CODE 안에서 link_beat_isr(); 를 부른다 (안 그러면 "multiple definition" 오류) */
#ifndef LINK_DEFINE_EXTI_HANDLER
#define LINK_DEFINE_EXTI_HANDLER 1
#endif

#ifndef LINK_LED_ENABLE
#define LINK_LED_ENABLE      1      /* LD2(PA5) 켜짐 = DMA OFF. PA5를 센서에 쓰면 0 */
#endif

#ifndef LINK_INIT_USART2
#define LINK_INIT_USART2     0      /* 보드 코드가 USART2를 이미 켜면 0. 1이면 여기서 115200bps로 켬 */
#endif

#ifndef LINK_LATENCY_MS
#define LINK_LATENCY_MS      0      /* 센서 감지 고정 지연(ms). 재서 넣으면 오차에서 뺀다 */
#endif

#ifndef LINK_STAT_MS
#define LINK_STAT_MS         1000   /* #STAT 줄 간격(ms). 0이면 안 찍음 */
#endif

/* ---- 줄 보내는 길 ----
 * I2C: Uno 하나만 노트북에 꽂는 구조. 이 보드가 I2C 마스터가 되어 Uno(슬레이브 0x08)에 한 줄씩 보낸다.
 *      핀: I2C1 리맵 PB8 = SCL(D15), PB9 = SDA(D14). 둘 다 5V 내성(FT) 핀이라 Uno 5V 풀업에 직접 연결 가능
 * UART: USART2(ST-LINK USB). 노트북에 직접 꽂았을 때 디버그용으로 같은 줄을 같이 찍는다 */
#ifndef LINK_I2C_ENABLE
#define LINK_I2C_ENABLE      1
#endif
#ifndef LINK_UART_ENABLE
#define LINK_UART_ENABLE     1
#endif
#ifndef LINK_I2C_ADDR
#define LINK_I2C_ADDR        0x08   /* Uno Wire.begin(0x08) */
#endif
#ifndef LINK_I2C_TIMEOUT_MS
#define LINK_I2C_TIMEOUT_MS  2      /* 한 단계(START·주소·바이트 하나) 최대 대기. Uno가 없으면 이만큼 기다리고 포기 */
#endif

#ifndef LINK_DEFAULT_BEAT_MS
#define LINK_DEFAULT_BEAT_MS 667    /* 박 간격을 아직 못 쟀을 때 쓰는 값 (90 BPM) */
#endif
#ifndef LINK_SONG_GAP_MS
#define LINK_SONG_GAP_MS     2000   /* 이만큼 쉬었다 오는 펄스 = 새 곡의 0번 박 */
#endif
#ifndef LINK_MIN_BEAT_MS
#define LINK_MIN_BEAT_MS     150    /* 이보다 짧은 간격의 펄스는 잡음으로 무시 */
#endif

/* ================= 함수 ================= */

void     link_init(void);                                 /* USART2 초기화 뒤 한 번 */
void     link_loop(void);                                 /* while(1) 안에서 계속 */

uint8_t  link_dma_on(void);                               /* 1 = DMA ON, 0 = DMA OFF(폴링) */
void     link_on_mode_change(uint8_t dma_on);             /* weak: 보드에서 다시 정의하면 전환 때 불림 */

void     link_note_on(const char *pitch, uint8_t vel);    /* "DO".."SOL" / "KICK","SNARE", 세기 0~100 */
void     link_note_off(const char *pitch);                /* 관악기 유지 끝 (하프·드럼은 안 불러도 됨) */

void     link_busy_begin(void);                           /* CPU 사용률: 센서 일 시작 */
void     link_busy_end(void);                             /* CPU 사용률: 센서 일 끝 */
void     link_miss_add(uint32_t n);                       /* 폴링이 늦어 놓친 샘플 수 더하기 */

void     link_debug(const char *msg);                     /* "#" 붙여서 출력 (게임은 무시) */
void     link_beat_isr(void);                             /* EXTI9_5 핸들러 안에서 부르기 */
uint32_t link_millis(void);                               /* weak: 기본 HAL_GetTick() */
void     link_uart_write(const char *s, int n);           /* weak: 기본 USART2 레지스터 직접 */

#endif
