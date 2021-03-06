/*
 * Phoenix-RTOS
 *
 * STM32L4 SPI driver
 *
 * Copyright 2018, 2020 Phoenix Systems
 * Author: Aleksander Kaminski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#include <sys/threads.h>
#include <sys/msg.h>
#include <sys/pwman.h>
#include <sys/interrupt.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

#include "stm32-multi.h"
#include "common.h"
#include "rcc.h"
#include "spi.h"


#define SPI1_POS 0
#define SPI2_POS (SPI1_POS + SPI1)
#define SPI3_POS (SPI2_POS + SPI2)


struct {
	volatile uint16_t *base;
	volatile int ready;

	handle_t mutex;
	handle_t irqLock;
	handle_t cond;
	handle_t inth;
} spi_common[SPI1 + SPI2 + SPI3];


static const int spi2pctl[] = { pctl_spi1, pctl_spi2, pctl_spi3 };


static const int spiConfig[] = { SPI1, SPI2, SPI3 };


static const int spiPos[] = { SPI1_POS, SPI2_POS, SPI3_POS };


enum { cr1 = 0, cr2 = 2, sr = 4, dr = 6, crcpr = 8, rxcrcr = 10, txcrcr = 12 };


static int spi_irqHandler(unsigned int n, void *arg)
{
	*(spi_common[(int)arg].base + cr2) &= ~(1 << 7);
	spi_common[(int)arg].ready = 1;

	return 1;
}


static unsigned char _spi_readwrite(int spi, unsigned char txd)
{
	unsigned char rxd;

	spi_common[spi].ready = 0;

	/* Initiate transmission */
	*((uint8_t *)(spi_common[spi].base + dr)) = txd;
	*(spi_common[spi].base + cr2) |= 1 << 7;

	mutexLock(spi_common[spi].irqLock);
	while (!spi_common[spi].ready)
		condWait(spi_common[spi].cond, spi_common[spi].irqLock, 1);
	mutexUnlock(spi_common[spi].irqLock);

	rxd = *((uint8_t *)(spi_common[spi].base + dr));

	return rxd;
}


int spi_transaction(int spi, int dir, unsigned char cmd, unsigned int addr, unsigned char flags, unsigned char *ibuff, unsigned char *obuff, size_t bufflen)
{
	int i;

	if (spi < spi1 || spi > spi3 || !spiConfig[spi])
		return -EINVAL;

	spi = spiPos[spi];

	mutexLock(spi_common[spi].mutex);
	keepidle(1);

	if (flags & spi_cmd)
		_spi_readwrite(spi, cmd);

	if (flags & spi_address) {
		for (i = 0; i < 3; ++i) {
			_spi_readwrite(spi, (addr >> 16) & 0xff);
			addr <<= 8;
		}
	}

	if (flags & spi_dummy)
		_spi_readwrite(spi, 0);

	if (dir == spi_read) {
		for (i = 0; i < bufflen; ++i)
			ibuff[i] = _spi_readwrite(spi, 0);
	}
	else if (dir == spi_write) {
		for (i = 0; i < bufflen; ++i)
			_spi_readwrite(spi, obuff[i]);
	}
	else {
		for (i = 0; i < bufflen; ++i)
			ibuff[i] = _spi_readwrite(spi, obuff[i]);
	}

	keepidle(0);
	mutexUnlock(spi_common[spi].mutex);

	return bufflen;
}


int spi_configure(int spi, char mode, char bdiv, int enable)
{
	int pos;
	unsigned int t;

	if (spi < spi1 || spi > spi3 || !spiConfig[spi])
		return -EINVAL;

	pos = spiPos[spi];

	mutexLock(spi_common[pos].mutex);

	rcc_devClk(spi2pctl[spi], 1);
	*(spi_common[pos].base + cr1) &= ~(1 << 6);

	/* Set mode and baud div */
	t = *(spi_common[pos].base + cr1) & ~((0x7 << 3) | 0x3);
	*(spi_common[pos].base + cr1) = t | (((unsigned int)bdiv & 0x7) << 3) | (mode & 0x3);

	if (enable)
		*(spi_common[pos].base + cr1) |= 1 << 6;
	else
		rcc_devClk(spi2pctl[spi], 0);

	mutexUnlock(spi_common[pos].mutex);

	return EOK;
}


void spi_init(void)
{
	int i, spi;

	static const struct {
		unsigned int base;
		int irq;
	} spiinfo[3] = { { 0x40013000, spi1_irq }, { 0x40003800, spi2_irq }, { 0x40003c00, spi3_irq } };

	for (i = 0, spi = 0; spi < 3; ++spi) {
		if (!spiConfig[spi])
			continue;

		spi_common[i].base = (void *)spiinfo[spi].base;
		spi_common[i].ready = 1;

		mutexCreate(&spi_common[i].mutex);
		mutexCreate(&spi_common[i].irqLock);
		condCreate(&spi_common[i].cond);

		rcc_devClk(spi2pctl[spi], 1);

		/* Disable SPI */
		*(spi_common[i].base + cr1) &= ~(1 << 6);
		dataBarier();

		/* 1 MHz baudrate, master, mode 0 */
		*(spi_common[i].base + cr1) = (1 << 2);

		/* 8 bits, motorola frame format */
		*(spi_common[i].base + cr2) = (0x7 << 8) | (1 << 2);

		/* Enable SPI */
		*(spi_common[i].base + cr1) |= 1 << 6;

		interrupt(spiinfo[spi].irq, spi_irqHandler, (void *)i, spi_common[i].cond, &spi_common[i].inth);

		++i;
	}
}
