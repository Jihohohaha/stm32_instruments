# 관악기 (STM32 #1, NUCLEO-F103RB)

마이크 입김 세기 + 조이스틱 5방향(가운데 도 / 위 레 / 오른쪽 미 / 아래 파 / 왼쪽 솔) → 사인파 PWM 소리.
주변장치는 레지스터로 직접 설정한다(HAL은 클럭과 `HAL_GetTick`만).

| 기능 | 자원 |
| --- | --- |
| 마이크 / 조이 X / 조이 Y | PA0 / PA1 / PA4 → ADC1 scan, TIM3 TRGO 8kHz, DMA1 채널1 circular |
| 조이 SW | PB0 입력 풀업 (한 옥타브 위) |
| 소리 | PB6 TIM4_CH1 PWM, DMA1 채널7이 사인파 표를 CCR1에 → RC 1kΩ+100nF → PAM8403 |
| 로그 | USART2 PA2/PA3 115200 (USB) |
| 클럭 | HSI → PLL **64MHz** (기획안의 72MHz 아님) |

## 버전

| 버전 | 내용 | 기여 |
| --- | --- | --- |
| v1 (`wind_instrument/`, 지금 올라간 것) | ADC 3채널 + DMA, 입김으로 소리 켜고 끄기, 조이스틱 5음, SW 옥타브, 0.1초 상태 로그 | 팀원(공동 담당) 구현. 실제 연결해서 소리·음 변경 확인 |
| v2 (올릴 예정) | v1 + DMA ON/OFF 비교(B1, `#STAT` 줄) + 박자 펄스(PB5 EXTI) 기준 이벤트 줄 | AI 제안, 박지호 검증 예정 |

## 열린 것

- `.ioc`에는 FreeRTOS가 켜져 있는데 생성된 `main.c`에는 없다. 이 상태로 CubeMX 코드를 다시 생성하면 FreeRTOS가 들어가 `wind_loop`가 안 돌 수 있다. 다시 생성하기 전에 CubeMX에서 FreeRTOS를 끌지 결정.
- v1의 로그 줄(`amp=... vol=...`)은 시리얼 형식의 이벤트 줄이 아니다. 게임 연결은 v2부터.
