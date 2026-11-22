/* ensemble_link_cfg.h - 드럼 보드 설정 (Core/Inc 에 둔다) */
#define LINK_INST          'D'
#define LINK_LED_ENABLE    0     /* LD2는 main.c가 직접 켬(= DMA OFF 표시, 고장이면 깜빡임) */
#define LINK_INIT_USART2   0     /* main.c의 gpio_uart_init()이 이미 켬 */
#define LINK_LATENCY_MS    2     /* 타격 뒤 2ms 피크 창(PEAK_WINDOW_FRAMES 16 / 8kHz)을 오차에서 뺌 */
#define LINK_I2C_ENABLE    1     /* Uno 하나만 노트북에: PB8(D15)=SCL, PB9(D14)=SDA → Uno A5/A4 */
#define LINK_UART_ENABLE   1     /* 노트북에 직접 꽂았을 때 디버그용으로 같은 줄 */
