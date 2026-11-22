/* ensemble_link_cfg.h - 하프 보드 설정 (Core/Inc 에 둔다) */
#define LINK_INST          'H'
#define LINK_LED_ENABLE    1     /* LD2 켜짐 = DMA OFF (main.c의 "줄 끊김" LED 표시는 뺐음) */
#define LINK_INIT_USART2   0     /* CubeMX MX_USART2_UART_Init()이 이미 켬 */
#define LINK_LATENCY_MS    0     /* CdS 반응 지연을 재면 여기에 (확인 필요) */
#define LINK_I2C_ENABLE    1     /* Uno 하나만 노트북에: PB8(D15)=SCL, PB9(D14)=SDA → Uno A5/A4 */
#define LINK_UART_ENABLE   1     /* 노트북에 직접 꽂았을 때 디버그용으로 같은 줄 */
