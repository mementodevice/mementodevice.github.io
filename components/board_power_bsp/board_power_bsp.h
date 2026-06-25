#ifndef BOARD_POWER_BSP_H
#define BOARD_POWER_BSP_H

class board_power_bsp_t
{
private:
    const uint8_t epd_power_pin;
    const uint8_t audio_power_pin;
    const uint8_t vbat_power_pin;

public:
    board_power_bsp_t(uint8_t _epd_power_pin,uint8_t _audio_power_pin,uint8_t _vbat_power_pin);
    ~board_power_bsp_t();

    void POWEER_EPD_ON();
    void POWEER_EPD_OFF();
    void POWEER_Audio_ON();
    void POWEER_Audio_OFF();
    void VBAT_POWER_ON();
    void VBAT_POWER_OFF();

    /* Deep sleep, waking on BOOT(GPIO0)/PWR(GPIO18) (ANY_LOW). If timer_wake_sec > 0,
     * also wake after that many seconds (used for the hourly countdown refresh). */
    void EnableDeepLowPowerMode(uint32_t timer_wake_sec = 0); //深度睡眠
};

#endif