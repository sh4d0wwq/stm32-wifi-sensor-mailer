#ifndef WIFI_MAIL_H
#define WIFI_MAIL_H

#include <stdint.h>

/** Start interrupt-driven reception from the Wi-Fi module (call once after UART init). */
void WiFi_StartRx(void);

/**
 * Reset the module, configure it as a DHCP station and join the access point.
 * @return 1 on success, 0 on failure (see WiFi_FailText()).
 */
int WiFi_Join(void);

/**
 * Send one e-mail with the sensor readings over SMTP/TLS.
 * Values are fixed-point with one decimal digit: 123 means 12.3.
 * @return 1 if the server accepted the message, 0 otherwise (see WiFi_FailText()).
 */
int WiFi_SendReadings(int pot10, int light10, int temp10);

/** Short (<= ~8 chars) description of the last failure, suitable for the OLED. */
const char *WiFi_FailText(void);

#endif
