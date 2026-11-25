/*
 * Controle de LED RGB pela UART - FRDM-KL25Z / Zephyr
 *
 * Pacote de 6 bytes:  ID | R | G | B | ACAO | CHECK
 * CHECK = ID ^ R ^ G ^ B ^ ACAO
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <stdint.h>
#include <stdbool.h>

/* ---------- Protocolo ---------- */
#define PKT_LEN        6
#define PKT_ID         0xA5   /* ID deste dispositivo (escolha livre) */
#define RX_TIMEOUT_MS  100    /* tempo max. entre bytes dentro de um pacote */

enum {
	ACT_ON       = 0x01,
	ACT_OFF      = 0x02,
	ACT_TOGGLE   = 0x03,
	ACT_BLINK_1S = 0x04,
	ACT_BLINK_2S = 0x05,
};

/* ---------- Hardware ---------- */
/* O flag GPIO_ACTIVE_LOW ja esta no devicetree da placa:
 * gpio_pin_set_dt(..., 1) = LED aceso, mesmo o hardware sendo ativo em nivel baixo. */
static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_ALIAS(led2),   gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(DT_ALIAS(led1),  gpios);

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(uart0));

/* ---------- Estado do LED (protegido por mutex) ---------- */
static struct {
	uint8_t r, g, b;
	bool on;
} led;

K_MUTEX_DEFINE(led_lock);
static struct k_work_delayable blink_work;
static uint32_t blink_ms;   /* 0 = nao esta piscando */

/* Fila que leva os bytes da ISR da UART ate a thread principal */
K_MSGQ_DEFINE(rx_q, sizeof(uint8_t), 64, 1);

/* ---------- LED ---------- */
/* Sem PWM: cada cor so liga/desliga. Valor > 0 significa "ligada". */
static void led_apply(void)
{
	gpio_pin_set_dt(&led_r, led.on && led.r > 0);
	gpio_pin_set_dt(&led_g, led.on && led.g > 0);
	gpio_pin_set_dt(&led_b, led.on && led.b > 0);
}

static void blink_handler(struct k_work *work)
{
	uint32_t period;

	k_mutex_lock(&led_lock, K_FOREVER);
	led.on = !led.on;
	led_apply();
	period = blink_ms;
	k_mutex_unlock(&led_lock);

	if (period) {
		k_work_reschedule(&blink_work, K_MSEC(period));
	}
}

static void stop_blink(void)
{
	/* Chamado SEM segurar o mutex (o handler tambem usa o mutex) */
	k_work_cancel_delayable_sync(&blink_work, &(struct k_work_sync){0});
	blink_ms = 0;
}

/* ---------- Validacao e execucao ---------- */
static uint8_t checksum_xor(const uint8_t *data, size_t len)
{
	uint8_t c = 0;

	for (size_t i = 0; i < len; i++) {
		c ^= data[i];
	}
	return c;
}

static void process_packet(const uint8_t *p)
{
	if (p[0] != PKT_ID) {
		return;                         /* ID errado */
	}
	if (checksum_xor(p, PKT_LEN - 1) != p[PKT_LEN - 1]) {
		return;                         /* CHECK incorreto */
	}

	uint8_t act = p[4];

	if (act < ACT_ON || act > ACT_BLINK_2S) {
		return;                         /* acao desconhecida */
	}

	stop_blink();                           /* qualquer comando novo cancela o pisca anterior */

	k_mutex_lock(&led_lock, K_FOREVER);
	led.r = p[1];
	led.g = p[2];
	led.b = p[3];

	switch (act) {
	case ACT_ON:
		led.on = true;
		break;
	case ACT_OFF:
		led.on = false;
		break;
	case ACT_TOGGLE:
		led.on = !led.on;
		break;
	case ACT_BLINK_1S:
		blink_ms = 1000;
		led.on = true;
		break;
	case ACT_BLINK_2S:
		blink_ms = 2000;
		led.on = true;
		break;
	}
	led_apply();
	uint32_t period = blink_ms;
	k_mutex_unlock(&led_lock);

	if (period) {
		k_work_reschedule(&blink_work, K_MSEC(period));
	}
}

/* ---------- UART (ISR) ---------- */
static void uart_cb(const struct device *dev, void *user_data)
{
	uint8_t byte;

	if (!uart_irq_update(dev)) {
		return;
	}
	while (uart_irq_rx_ready(dev)) {
		if (uart_fifo_read(dev, &byte, 1) != 1) {
			break;
		}
		k_msgq_put(&rx_q, &byte, K_NO_WAIT);  /* fila cheia: descarta o byte */
	}
}

/* ---------- main: maquina de estados do receptor ---------- */
int main(void)
{
	uint8_t pkt[PKT_LEN];
	size_t idx = 0;
	uint8_t b;

	if (!device_is_ready(uart) ||
	    !gpio_is_ready_dt(&led_r) ||
	    !gpio_is_ready_dt(&led_g) ||
	    !gpio_is_ready_dt(&led_b)) {
		return 0;
	}

	gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);

	k_work_init_delayable(&blink_work, blink_handler);

	uart_irq_callback_user_data_set(uart, uart_cb, NULL);
	uart_irq_rx_enable(uart);

	while (1) {
		/* Esperando o 1o byte: pode esperar para sempre.
		 * No meio de um pacote: so espera RX_TIMEOUT_MS. */
		k_timeout_t wait = (idx == 0) ? K_FOREVER : K_MSEC(RX_TIMEOUT_MS);

		if (k_msgq_get(&rx_q, &b, wait) != 0) {
			idx = 0;                /* timeout: pacote truncado, descarta */
			continue;
		}

		if (idx == 0 && b != PKT_ID) {
			continue;               /* lixo: ignora ate achar um ID valido */
		}

		pkt[idx++] = b;

		if (idx == PKT_LEN) {
			process_packet(pkt);
			idx = 0;
		}
	}
	return 0;
}