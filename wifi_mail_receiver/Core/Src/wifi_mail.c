/**
 * @file    wifi_mail.c
 * @brief   Wi-Fi AT-command driver and minimal SMTP client.
 *
 * Talks to the SPWF01SA module on the X-NUCLEO-IDW01M1 board (AT+S.* commands) over USART1:
 *   1. WiFi_Join()         - hardware reset, configure station mode, join the AP.
 *   2. WiFi_SendReadings() - open a TLS socket to the SMTP server (port 465) and run
 *                            EHLO -> AUTH LOGIN -> MAIL FROM -> RCPT TO -> DATA -> QUIT.
 *
 * Received bytes are collected by a ring buffer filled from the UART RX interrupt.
 * Credentials live in wifi_secrets.h (not tracked by git).
 */
#include "wifi_mail.h"
#include "main.h"

#include <stdio.h>
#include <string.h>

#if defined(__has_include)
#if !__has_include("wifi_secrets.h")
#error "wifi_secrets.h not found: copy wifi_secrets.example.h to wifi_secrets.h and fill in your credentials."
#endif
#endif
#include "wifi_secrets.h"

#define SMTP_HOST           "smtp.gmail.com"
#define SMTP_PORT           465
#define MAIL_SUBJECT        "STM32 sensor readings"

#define WIFI_RX_SIZE 512

extern UART_HandleTypeDef huart1;

static volatile uint8_t wifi_rx[WIFI_RX_SIZE];
static volatile uint16_t wifi_rx_head;
static volatile uint16_t wifi_rx_tail;
static uint8_t wifi_rx_byte;
static const char *wifi_err;
static char wifi_msg[16];

const char *WiFi_FailText(void)
{
  return wifi_err != NULL ? wifi_err : "";
}

static void wifi_rx_flush(void)
{
  wifi_rx_tail = wifi_rx_head;
}

static int wifi_pull(char *acc, int *len, int acc_sz, const char *token, const char *bad, uint32_t timeout)
{
  uint32_t t0 = HAL_GetTick();

  acc[*len] = '\0';
  while ((HAL_GetTick() - t0) < timeout)
  {
    uint16_t head = wifi_rx_head;
    while (wifi_rx_tail != head && *len < acc_sz - 1)
    {
      acc[*len] = (char)wifi_rx[wifi_rx_tail];
      *len += 1;
      wifi_rx_tail = (uint16_t)((wifi_rx_tail + 1U) % WIFI_RX_SIZE);
      head = wifi_rx_head;
    }
    acc[*len] = '\0';
    if (token != NULL && strstr(acc, token) != NULL)
    {
      return 1;
    }
    if (bad != NULL && strstr(acc, bad) != NULL)
    {
      return 0;
    }
    if (*len >= acc_sz - 1)
    {
      memmove(acc, acc + *len - 48, 48);
      *len = 48;
      acc[*len] = '\0';
    }
  }
  return 0;
}

static int wifi_cmd(const char *cmd, char *acc, int acc_sz, const char *token, uint32_t timeout, int fail_on_error)
{
  static char line[128];
  int n = snprintf(line, sizeof(line), "%s\r", cmd);
  int len = 0;

  if (n <= 0 || n >= (int)sizeof(line) || acc_sz < 16)
  {
    return 0;
  }
  wifi_rx_flush();
  if (HAL_UART_Transmit(&huart1, (uint8_t *)line, (uint16_t)n, 1000) != HAL_OK)
  {
    return 0;
  }
  return wifi_pull(acc, &len, acc_sz, token, fail_on_error ? "ERROR" : NULL, timeout);
}

static int wifi_wait(const char *token, const char *bad, uint32_t timeout)
{
  static char acc[160];
  int len = 0;
  return wifi_pull(acc, &len, (int)sizeof(acc), token, bad, timeout);
}

static void wifi_allow(uint16_t cts_pin)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  gpio.Pin = cts_pin;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &gpio);
  HAL_GPIO_WritePin(GPIOA, cts_pin, GPIO_PIN_RESET);

  gpio.Pin = (cts_pin == GPIO_PIN_11) ? GPIO_PIN_12 : GPIO_PIN_11;
  gpio.Mode = GPIO_MODE_INPUT;
  HAL_GPIO_Init(GPIOA, &gpio);
}

static void wifi_hw_reset(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();
  gpio.Pin = GPIO_PIN_12;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &gpio);

  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_RESET);
  HAL_Delay(50);
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_SET);
  HAL_Delay(2000);
}

static int wifi_at_ping(char *acc, int acc_sz)
{
  int i;

  for (i = 0; i < 6; i++)
  {
    int len = 0;

    wifi_rx_flush();
    if (HAL_UART_Transmit(&huart1, (uint8_t *)"AT\r", 3, 200) != HAL_OK)
    {
      return 0;
    }
    if (wifi_pull(acc, &len, acc_sz, "OK", NULL, 500))
    {
      return 1;
    }
  }
  return 0;
}

static int b64_encode(const char *in, char *out, int out_sz)
{
  static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int len = (int)strlen(in);
  int i = 0;
  int o = 0;

  while (i < len)
  {
    unsigned v;
    int n = 1;

    v = ((unsigned char)in[i++]) << 16;
    if (i < len)
    {
      v |= ((unsigned char)in[i++]) << 8;
      n++;
    }
    if (i < len)
    {
      v |= (unsigned char)in[i++];
      n++;
    }
    if (o + 4 >= out_sz)
    {
      return -1;
    }
    out[o++] = tab[(v >> 18) & 63U];
    out[o++] = tab[(v >> 12) & 63U];
    out[o++] = (n > 1) ? tab[(v >> 6) & 63U] : '=';
    out[o++] = (n > 2) ? tab[v & 63U] : '=';
  }
  out[o] = '\0';
  return o;
}

static int parse_num_after(const char *s, const char *key)
{
  const char *p = strstr(s, key);
  int v = 0;

  if (p == NULL)
  {
    return -1;
  }
  p += strlen(key);
  while (*p == ' ')
  {
    p++;
  }
  if (*p < '0' || *p > '9')
  {
    return -1;
  }
  while (*p >= '0' && *p <= '9')
  {
    v = v * 10 + (*p - '0');
    p++;
  }
  return v;
}

static int sock_pending(int id)
{
  char cmd[24];
  char acc[80];

  snprintf(cmd, sizeof(cmd), "AT+S.SOCKQ=%02d", id);
  if (!wifi_cmd(cmd, acc, (int)sizeof(acc), "OK", 2000, 1))
  {
    return -1;
  }
  return parse_num_after(acc, "DATALEN:");
}

static int sock_read(int id, char *out, int want, uint32_t timeout)
{
  char cmd[32];
  int cn = snprintf(cmd, sizeof(cmd), "AT+S.SOCKR=%02d,%d\r", id, want);
  uint32_t t0;
  int got = 0;

  if (cn <= 0 || cn >= (int)sizeof(cmd) || want <= 0)
  {
    return 0;
  }
  wifi_rx_flush();
  if (HAL_UART_Transmit(&huart1, (uint8_t *)cmd, (uint16_t)cn, 1000) != HAL_OK)
  {
    return 0;
  }
  t0 = HAL_GetTick();
  while (got < want && (HAL_GetTick() - t0) < timeout)
  {
    uint16_t head = wifi_rx_head;
    if (wifi_rx_tail != head)
    {
      out[got++] = (char)wifi_rx[wifi_rx_tail];
      wifi_rx_tail = (uint16_t)((wifi_rx_tail + 1U) % WIFI_RX_SIZE);
    }
  }
  out[got] = '\0';
  if (got != want)
  {
    return 0;
  }
  return wifi_wait("OK", "ERROR", 2000);
}

static int sock_write(int id, const char *data)
{
  static uint8_t buf[384];
  int dlen = (int)strlen(data);
  int n;

  if (dlen <= 0 || dlen > 240)
  {
    return 0;
  }
  n = snprintf((char *)buf, sizeof(buf), "AT+S.SOCKW=%02d,%d\r%s", id, dlen, data);
  if (n <= dlen || n >= (int)sizeof(buf) || buf[n - dlen - 1] != '\r')
  {
    return 0;
  }
  wifi_rx_flush();
  if (HAL_UART_Transmit(&huart1, buf, (uint16_t)n, 3000) != HAL_OK)
  {
    return 0;
  }
  return wifi_wait("OK", "ERROR", 8000);
}

static void sock_drain(int id);

static int smtp_wait_code(int id, const char *code, uint32_t timeout)
{
  static char acc[512];
  int n = 0;
  uint32_t t0 = HAL_GetTick();

  acc[0] = '\0';
  while ((HAL_GetTick() - t0) < timeout)
  {
    int pending = sock_pending(id);
    if (pending < 0)
    {
      return 0;
    }
    if (pending == 0)
    {
      HAL_Delay(150);
      continue;
    }
    if (pending > 120)
    {
      pending = 120;
    }
    if (n + pending >= (int)sizeof(acc) - 1)
    {
      memmove(acc, acc + (n > 64 ? n - 64 : 0), 64);
      n = 64;
    }
    if (!sock_read(id, acc + n, pending, 3000))
    {
      return 0;
    }
    n += pending;
    acc[n] = '\0';
    if (strstr(acc, code) != NULL)
    {
      sock_drain(id);
      return 1;
    }
  }
  return 0;
}

static void sock_drain(int id)
{
  int idle = 0;
  int guard = 0;

  while (idle < 4 && guard < 30)
  {
    int n = sock_pending(id);
    char dump[100];

    guard++;
    if (n < 0)
    {
      HAL_Delay(40);
      continue;
    }
    if (n == 0)
    {
      idle++;
      HAL_Delay(60);
      continue;
    }
    idle = 0;
    if (n > 80)
    {
      n = 80;
    }
    if (!sock_read(id, dump, n, 2000))
    {
      HAL_Delay(40);
    }
  }
}

static int smtp_cmd(int id, const char *data, const char *expect, uint32_t timeout)
{
  int attempt;

  for (attempt = 0; attempt < 2; attempt++)
  {
    if (sock_write(id, data) && smtp_wait_code(id, expect, timeout))
    {
      return 1;
    }
    sock_drain(id);
  }
  return 0;
}

static void sock_close(int id)
{
  char cmd[24];
  char dump[80];
  uint32_t t0 = HAL_GetTick();

  while ((HAL_GetTick() - t0) < 800)
  {
    int n = sock_pending(id);
    if (n <= 0)
    {
      break;
    }
    if (n > 60)
    {
      n = 60;
    }
    if (!sock_read(id, dump, n, 1000))
    {
      break;
    }
  }
  snprintf(cmd, sizeof(cmd), "AT+S.SOCKC=%02d", id);
  wifi_cmd(cmd, dump, (int)sizeof(dump), "OK", 3000, 0);
}

static int sock_open(int *id)
{
  char cmd[96];
  char acc[160];

  wifi_cmd("AT+S.TLSCERT2=clean,all", acc, (int)sizeof(acc), "OK", 3000, 0);
  snprintf(cmd, sizeof(cmd), "AT+S.SOCKON=%s,%d,s", SMTP_HOST, SMTP_PORT);
  if (!wifi_cmd(cmd, acc, (int)sizeof(acc), "OK", 30000, 1))
  {
    snprintf(wifi_msg, sizeof(wifi_msg), "SOCK");
    return 0;
  }
  *id = parse_num_after(acc, "ID:");
  if (*id < 0)
  {
    snprintf(wifi_msg, sizeof(wifi_msg), "NO ID");
    return 0;
  }
  return 1;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    uint16_t next = (uint16_t)((wifi_rx_head + 1U) % WIFI_RX_SIZE);
    if (next != wifi_rx_tail)
    {
      wifi_rx[wifi_rx_head] = wifi_rx_byte;
      wifi_rx_head = next;
    }
    HAL_UART_Receive_IT(&huart1, &wifi_rx_byte, 1);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    HAL_UART_Receive_IT(&huart1, &wifi_rx_byte, 1);
  }
}

void WiFi_StartRx(void)
{
  wifi_rx_head = 0;
  wifi_rx_tail = 0;
  HAL_UART_Receive_IT(&huart1, &wifi_rx_byte, 1);
}

int WiFi_Join(void)
{
  static char cmd[128];
  static char acc[192];

  wifi_err = "";
  if (strcmp(WIFI_SSID, "WIFI_SSID") == 0 || strcmp(SMTP_USER, "user@gmail.com") == 0)
  {
    wifi_err = "NO CRED";
    return 0;
  }

  wifi_hw_reset();
  wifi_allow(GPIO_PIN_11);
  if (!wifi_at_ping(acc, (int)sizeof(acc)))
  {
    wifi_allow(GPIO_PIN_12);
    if (!wifi_at_ping(acc, (int)sizeof(acc)))
    {
      wifi_err = "NO AT";
      return 0;
    }
  }

  if (!wifi_cmd("AT+S.SCFG=localecho1,0", acc, (int)sizeof(acc), "OK", 1500, 1))
  {
    wifi_err = "ECHO";
    return 0;
  }
  if (!wifi_cmd("AT+S.SCFG=console1_hwfc,0", acc, (int)sizeof(acc), "OK", 1500, 1))
  {
    wifi_err = "HWFC";
    return 0;
  }
  if (!wifi_cmd("AT+S.SCFG=wifi_priv_mode,2", acc, (int)sizeof(acc), "OK", 1500, 1))
  {
    wifi_err = "PRIV";
    return 0;
  }
  if (!wifi_cmd("AT+S.SCFG=wifi_auth_type,0", acc, (int)sizeof(acc), "OK", 1500, 1))
  {
    wifi_err = "AUTH";
    return 0;
  }
  if (!wifi_cmd("AT+S.SCFG=wifi_mode,1", acc, (int)sizeof(acc), "OK", 1500, 1))
  {
    wifi_err = "MODE";
    return 0;
  }
  if (!wifi_cmd("AT+S.SCFG=ip_use_dhcp,1", acc, (int)sizeof(acc), "OK", 1500, 1))
  {
    wifi_err = "DHCP";
    return 0;
  }
  snprintf(cmd, sizeof(cmd), "AT+S.SSIDTXT=%s", WIFI_SSID);
  if (!wifi_cmd(cmd, acc, (int)sizeof(acc), "OK", 1500, 1))
  {
    wifi_err = "SSID";
    return 0;
  }
  snprintf(cmd, sizeof(cmd), "AT+S.SCFG=wifi_wpa_psk_text,%s", WIFI_PASSWORD);
  if (!wifi_cmd(cmd, acc, (int)sizeof(acc), "OK", 1500, 1))
  {
    wifi_err = "PSK";
    return 0;
  }
  if (!wifi_cmd("AT&W", acc, (int)sizeof(acc), "OK", 3000, 1))
  {
    wifi_err = "SAVE";
    return 0;
  }
  wifi_rx_flush();
  HAL_UART_Transmit(&huart1, (uint8_t *)"AT+CFUN=1\r", 10, 1000);
  {
    int len = 0;
    if (!wifi_pull(acc, &len, (int)sizeof(acc), "WiFi Up", NULL, 25000))
    {
      wifi_err = "NO UP";
      return 0;
    }
  }

  if (!wifi_cmd("AT", acc, (int)sizeof(acc), "OK", 2000, 1))
  {
    wifi_err = "CONSOLE";
    return 0;
  }
  if (strstr(acc, "AT\r") != NULL)
  {
    wifi_err = "ECHO ON";
    return 0;
  }
  return 1;
}

int WiFi_SendReadings(int pot10, int light10, int temp10)
{
  static char line[160];
  static char b64[120];
  static char body[384];
  int id = -1;
  int ok = 1;
  int bn;

  wifi_err = "";
  if (!sock_open(&id))
  {
    wifi_err = wifi_msg;
    return 0;
  }
  if (!smtp_wait_code(id, "220", 15000))
  {
    wifi_err = "NO 220";
    ok = 0;
  }
  if (ok && !smtp_cmd(id, "EHLO localhost\r\n", "250", 10000))
  {
    wifi_err = "EHLO";
    ok = 0;
  }
  if (ok && !smtp_cmd(id, "AUTH LOGIN\r\n", "334", 10000))
  {
    wifi_err = "AUTH";
    ok = 0;
  }
  if (ok && (b64_encode(SMTP_USER, b64, (int)sizeof(b64)) < 0 ||
             snprintf(line, sizeof(line), "%s\r\n", b64) >= (int)sizeof(line) ||
             !smtp_cmd(id, line, "334", 10000)))
  {
    wifi_err = "USER";
    ok = 0;
  }
  if (ok && (b64_encode(SMTP_APP_PASSWORD, b64, (int)sizeof(b64)) < 0 ||
             snprintf(line, sizeof(line), "%s\r\n", b64) >= (int)sizeof(line) ||
             !smtp_cmd(id, line, "235", 10000)))
  {
    wifi_err = "PASS";
    ok = 0;
  }
  if (ok && (snprintf(line, sizeof(line), "MAIL FROM:<%s>\r\n", SMTP_USER) >= (int)sizeof(line) ||
             !smtp_cmd(id, line, "250", 10000)))
  {
    wifi_err = "FROM";
    ok = 0;
  }
  if (ok && (snprintf(line, sizeof(line), "RCPT TO:<%s>\r\n", MAIL_TO) >= (int)sizeof(line) ||
             !smtp_cmd(id, line, "250", 10000)))
  {
    wifi_err = "RCPT";
    ok = 0;
  }
  if (ok && !smtp_cmd(id, "DATA\r\n", "354", 10000))
  {
    wifi_err = "DATA";
    ok = 0;
  }
  if (ok)
  {
    bn = snprintf(body, sizeof(body),
                  "From: <%s>\r\n"
                  "To: <%s>\r\n"
                  "Subject: " MAIL_SUBJECT "\r\n"
                  "\r\n"
                  "POT %d.%d%%\r\n"
                  "LDR %d.%d%%\r\n"
                  "TEMP %d.%d C\r\n"
                  ".\r\n",
                  SMTP_USER, MAIL_TO,
                  pot10 / 10, pot10 % 10,
                  light10 / 10, light10 % 10,
                  temp10 / 10, temp10 % 10);
    if (bn <= 0 || bn >= (int)sizeof(body) || !smtp_cmd(id, body, "250", 15000))
    {
      wifi_err = "BODY";
      ok = 0;
    }
  }
  if (ok)
  {
    smtp_cmd(id, "QUIT\r\n", "221", 5000);
  }
  sock_close(id);
  return ok;
}
