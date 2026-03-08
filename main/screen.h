#ifndef SCREEN_H_
#define SCREEN_H_

esp_err_t screen_start(void * pvParameters);
void screen_button_press(void);
void screen_x402_show_paid(const char * amount);

#endif /* SCREEN_H_ */
