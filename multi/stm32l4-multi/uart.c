/*
 * Phoenix-RTOS
 *
 * STM32L4 UART driver
 *
 * Copyright 2017, 2018, 2020 Phoenix Systems
 * Author: Jan Sikorski, Aleksander Kaminski, Andrzej Glowinski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/threads.h>
#include <sys/pwman.h>
#include <sys/interrupt.h>
#include <sys/platform.h>

#include "stm32-multi.h"
#include "common.h"
#include "gpio.h"
#include "uart.h"
#include "rcc.h"

#define UART1_POS 0
#define UART2_POS (UART1_POS + UART1)
#define UART3_POS (UART2_POS + UART2)
#define UART4_POS (UART3_POS + UART3)
#define UART5_POS (UART4_POS + UART4)
#define LPUART1_POS (UART5_POS + UART5)

#define UART_CNT (UART1 + UART2 + UART3 + UART4 + UART5 + LPUART1)

struct {
	volatile unsigned int *base;
	unsigned int port;
	unsigned int baud;
	volatile int enabled;
	int bits;

	volatile char * volatile txbeg;
	volatile char * volatile txend;

	volatile char rxdfifo[64];
	volatile unsigned int rxdr;
	volatile unsigned int rxdw;
	volatile char * volatile rxbeg;
	volatile char * volatile rxend;
	volatile unsigned int *read;

	handle_t rxlock;
	handle_t rxcond;
	handle_t txlock;
	handle_t txcond;
	handle_t lock;
} uart_common[UART_CNT];


static const int uartConfig[] = { UART1, UART2, UART3, UART4, UART5, LPUART1 };


static const int uartPos[] = { UART1_POS, UART2_POS, UART3_POS, UART4_POS, UART5_POS, LPUART1_POS };


enum { cr1 = 0, cr2, cr3, brr, gtpr, rtor, rqr, isr, icr, rdr, tdr };


static int uart_txirq(unsigned int n, void *arg)
{
	int uart = (int)arg, release = -1;

	if (*(uart_common[uart].base + isr) & (1 << 7)) {
		/* Txd buffer empty */
		if (uart_common[uart].txbeg != uart_common[uart].txend) {
			*(uart_common[uart].base + tdr) = *(uart_common[uart].txbeg++);
		}
		else {
			*(uart_common[uart].base + cr1) &= ~(1 << 7);
			uart_common[uart].txbeg = NULL;
			uart_common[uart].txend = NULL;
			release = 1;
		}
	}

	return release;
}


static int uart_rxirq(unsigned int n, void *arg)
{
	int uart = (int)arg, release = -1;

	/* Clear wakeup from stop mode flag */
	if (n == lpuart1_irq)
		*(uart_common[uart].base + icr) |= 1 << 20;

	if (*(uart_common[uart].base + isr) & ((1 << 5) | (1 << 3))) {
		/* Clear overrun error bit */
		*(uart_common[uart].base + icr) |= (1 << 3);

		/* Rxd buffer not empty */
		uart_common[uart].rxdfifo[uart_common[uart].rxdw++] = *(uart_common[uart].base + rdr);
		uart_common[uart].rxdw %= sizeof(uart_common[uart].rxdfifo);

		if (uart_common[uart].rxdr == uart_common[uart].rxdw)
			uart_common[uart].rxdr = (uart_common[uart].rxdr + 1) % sizeof(uart_common[uart].rxdfifo);
	}

	if (uart_common[uart].rxbeg != NULL) {
		while (uart_common[uart].rxdr != uart_common[uart].rxdw && uart_common[uart].rxbeg != uart_common[uart].rxend) {
			*(uart_common[uart].rxbeg++) = uart_common[uart].rxdfifo[uart_common[uart].rxdr++];
			uart_common[uart].rxdr %= sizeof(uart_common[uart].rxdfifo);
			(*uart_common[uart].read)++;
		}

		if (uart_common[uart].rxbeg == uart_common[uart].rxend) {
			uart_common[uart].rxbeg = NULL;
			uart_common[uart].rxend = NULL;
			uart_common[uart].read = NULL;
		}
		release = 1;
	}

	return release;
}


int uart_configure(int uart, char bits, char parity, unsigned int baud, char enable)
{
	int err = EOK, pos;
	int baseClk = (uart == lpuart1) ? (256 * 32768) : rcc_getCpufreq();
	unsigned int tcr1 = 0;
	char tbits = bits;

	if (uart < usart1 || uart > lpuart1 || !uartConfig[uart])
		return -EINVAL;

	pos = uartPos[uart];

	uart_common[pos].enabled = 0;
	condBroadcast(uart_common[pos].rxcond);

	mutexLock(uart_common[pos].txlock);
	mutexLock(uart_common[pos].rxlock);
	mutexLock(uart_common[pos].lock);

	dataBarier();

	uart_common[pos].txbeg = NULL;
	uart_common[pos].txend = NULL;

	uart_common[pos].rxbeg = NULL;
	uart_common[pos].rxend = NULL;
	uart_common[pos].read = NULL;
	uart_common[pos].rxdr = 0;
	uart_common[pos].rxdw = 0;

	if (uart == lpuart1)
		tcr1 |= (1 << 1);

	if (parity != uart_parnone) {
		tcr1 |= 1 << 10;
		tbits += 1; /* We need one extra for parity */
	}

	switch (tbits) {
	case 9:
		tcr1 &= ~(1 << 28);
		tcr1 |= (1 << 12);
		break;

	case 8:
		tcr1 &= ~((1 << 28) | (1 << 12));
		break;

	case 7:
		tcr1 &= ~(1 << 12);
		tcr1 |= (1 << 28);
		break;

	default:
		err = -1;
		break;
	}

	if (err == EOK) {
		uart_common[pos].bits = bits;

		*(uart_common[pos].base + cr1) &= ~1;
		dataBarier();
		*(uart_common[pos].base + cr1) = tcr1;

		uart_common[pos].baud = baud;
		*(uart_common[pos].base + brr) = baseClk / baud;

		if (parity == uart_parodd)
			*(uart_common[pos].base + cr1) |= 1 << 9;
		else
			*(uart_common[pos].base + cr1) &= ~(1 << 9);

		*(uart_common[pos].base + icr) = -1;
		(void)*(uart_common[pos].base + rdr);

		if (enable) {
			*(uart_common[pos].base + cr1) |= (1 << 5) | (1 << 3) | (1 << 2);
			dataBarier();
			*(uart_common[pos].base + cr1) |= 1;
			uart_common[pos].enabled = 1;
		}

		dataBarier();
	}

	mutexUnlock(uart_common[pos].lock);
	mutexUnlock(uart_common[pos].rxlock);
	mutexUnlock(uart_common[pos].txlock);

	return err;
}


int uart_write(int uart, void* buff, unsigned int bufflen)
{
	if (uart < usart1 || uart > lpuart1 || !uartConfig[uart])
		return -EINVAL;

	uart = uartPos[uart];

	if (!bufflen)
		return 0;

	if (!uart_common[uart].enabled)
		return -EIO;

	mutexLock(uart_common[uart].txlock);
	mutexLock(uart_common[uart].lock);

	keepidle(1);

	*(uart_common[uart].base + tdr) = *((unsigned char *)buff);
	uart_common[uart].txbeg = (void *)((unsigned char *)buff + 1);
	uart_common[uart].txend = (void *)((unsigned char *)buff + bufflen);
	*(uart_common[uart].base + cr1) |= 1 << 7;

	while (uart_common[uart].txbeg != uart_common[uart].txend)
		condWait(uart_common[uart].txcond, uart_common[uart].lock, 0);
	mutexUnlock(uart_common[uart].lock);

	keepidle(0);
	mutexUnlock(uart_common[uart].txlock);

	return bufflen;
}


int uart_read(int uart, void* buff, unsigned int count, char mode, unsigned int timeout)
{
	int i, err;
	volatile unsigned int read = 0;
	char mask = 0x7f;

	if (uart < usart1 || uart > lpuart1 || !uartConfig[uart])
		return -EINVAL;

	uart = uartPos[uart];

	if (!uart_common[uart].enabled)
		return -EIO;

	if (!count)
		return 0;

	mutexLock(uart_common[uart].rxlock);
	mutexLock(uart_common[uart].lock);

	uart_common[uart].read = &read;
	uart_common[uart].rxend = (char *)buff + count;

	/* This field works as trigger for rx interrupt to store data in buffer
	 * instead of FIFO */
	uart_common[uart].rxbeg = buff;

	/* Provoke UART exception to fire so that existing data from
	 * rxdfifo is copied into buff. The handler will clear this
	 * bit. */

	*(uart_common[uart].base + cr1) |= 1 << 7;

	while (uart_common[uart].rxbeg != uart_common[uart].rxend) {
		err = condWait(uart_common[uart].rxcond, uart_common[uart].lock, timeout);

		if (mode == uart_mnblock || (timeout && err == -ETIME) || !uart_common[uart].enabled) {
			uart_common[uart].rxbeg = NULL;
			uart_common[uart].rxend = NULL;
			uart_common[uart].read = NULL;
			break;
		}
	}

	if (uart_common[uart].bits < 8) {
		if (uart_common[uart].bits == 6)
			mask = 0x3f;

		for (i = 0; i < read; ++i)
			((char *)buff)[i] &= mask;
	}
	mutexUnlock(uart_common[uart].lock);

	mutexUnlock(uart_common[uart].rxlock);

	return read;
}


int uart_init(void)
{
	int i, uart;
	const struct {
		volatile uint32_t *base;
		int dev;
		unsigned irq;
	} info[] = {
		{ (void *)0x40013800, pctl_usart1, usart1_irq },
		{ (void *)0x40004400, pctl_usart2, usart2_irq },
		{ (void *)0x40004800, pctl_usart3, usart3_irq },
		{ (void *)0x40004c00, pctl_uart4, uart4_irq },
		{ (void *)0x40005000, pctl_uart5, uart5_irq },
		{ (void *)0x40008000, pctl_lpuart1, lpuart1_irq }
	};

	for (i = 0, uart = 0; uart < sizeof(info) / sizeof(info[0]); ++uart) {
		if (!uartConfig[uart])
			continue;

		rcc_devClk(info[uart].dev, 1);

		mutexCreate(&uart_common[i].rxlock);
		condCreate(&uart_common[i].rxcond);
		mutexCreate(&uart_common[i].txlock);
		condCreate(&uart_common[i].txcond);

		mutexCreate(&uart_common[i].lock);

		uart_common[i].base = info[uart].base;

		uart_common[i].txbeg = NULL;
		uart_common[i].txend = NULL;

		uart_common[i].rxbeg = NULL;
		uart_common[i].rxend = NULL;
		uart_common[i].read = NULL;
		uart_common[i].rxdr = 0;
		uart_common[i].rxdw = 0;

		if (uart == lpuart1) {
			/* Enable clock and wakeup from STOP mode */
			*(uart_common[i].base + cr3) |= (1 << 23) | (1 << 22) | (0x3 << 20);
		}

		/* Set up UART to 9600,8,n,1 16-bit oversampling */
		uart_configure(uart, 8, uart_parnone, 9600, 1);

		interrupt(info[uart].irq, uart_rxirq, (void *)i, uart_common[i].rxcond, NULL);
		interrupt(info[uart].irq, uart_txirq, (void *)i, uart_common[i].txcond, NULL);

		uart_common[i].enabled = 1;

		++i;
	}

	return EOK;
}
