#ifndef ADC_BSP_H
#define ADC_BSP_H

#ifdef __cplusplus
extern "C" {
#endif

void adc_bsp_init(void);
void adc_example(void* parmeter);
void adc_get_value(float *value,int *data);

#ifdef __cplusplus
}
#endif

#endif
