/*
 * wind.h - 관악기(STM32 #1) : NUCLEO-F103RB
 *
 *  입력 : 마이크(PA0, A0) + 조이스틱 X(PA1, A1), Y(PA4, A2), SW(PB0, A3)
 *         -> ADC1 3채널 scan, TIM3 TRGO(8kHz)로 자동 변환, DMA1 채널1 circular
 *  출력 : PB6(D10) TIM4_CH1 PWM, 사인파 표(64칸)를 DMA1 채널7이 CCR1에 계속 넣음
 *  로그 : USART2(PA2/PA3) 115200bps -> USB 케이블로 PC 시리얼 모니터
 *
 *  전제 : CubeMX 클럭 = HSI -> PLL 64MHz, APB1 /2(32MHz), APB2 /1(64MHz)
 */
#ifndef WIND_H
#define WIND_H

/* ===== 내일 테스트하면서 숫자만 바꾸면 되는 값 ===== */

#define SOUND_TEST      0      /* 1 = 센서 없이 도레미파솔 반복 재생(스피커 테스트용) */

#define BREATH_ON       200    /* 잡음 기준 + 이 값 넘으면 소리 켜짐 */
#define BREATH_OFF      100    /* 잡음 기준 + 이 값 아래로 내려가면 꺼짐 */
#define BREATH_FULL     2500   /* 입김 세기가 이 정도면 음량 100% */

#define JOY_DEAD        1200   /* 가운데에서 이만큼 벗어나야 방향으로 인정 (0~4095 기준) */

/* 조이스틱 방향 -> 음 번호(0=도,1=레,2=미,3=파,4=솔).
 * 순서: 가운데, 위, 오른쪽, 아래, 왼쪽
 * 상자에 붙인 방향이 달라 위/아래가 뒤집히면 여기 숫자만 바꾸기 */
#define DIR_NOTE_MAP    { 0, 1, 2, 3, 4 }

/* [합주] 0.1초 상태 줄(amp=.. vol=..). 혼자 조정할 때만 1. 합주·DMA 비교 때는 0
 * (폴링 모드에서 줄을 찍는 동안 샘플을 놓쳐 비교가 흐려짐. 1로 둘 거면 두 모드 같은 값으로) */
#ifndef WIND_LOG_STATUS
#define WIND_LOG_STATUS 0
#endif

void wind_init(void);   /* main.c USER CODE 2 에서 한 번 호출 */
void wind_loop(void);   /* main.c while(1) 안에서 계속 호출 */

#endif
