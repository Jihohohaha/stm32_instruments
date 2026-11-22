/* ensemble_link_cfg.h - 관악기 보드 설정 (Core/Inc) */
#define LINK_INST          'W'
#define LINK_LED_ENABLE    1     /* LD2 켜짐 = DMA OFF (관악기 v1은 LD2를 안 씀) */
#define LINK_INIT_USART2   0     /* wind.c usart2_init()이 이미 켬 */
#define LINK_LATENCY_MS    4     /* 입김 감지는 반 버퍼(8ms) 단위 → 가운데 4ms를 뺌 (추정, 재서 조정) */
#define LINK_I2C_ENABLE    1     /* Uno 하나 구조: PB8(D15)=SCL, PB9(D14)=SDA → Uno A5/A4 */
#define LINK_UART_ENABLE   1
