/*	$OpenBSD: mvortc.c,v 1.1 2023/03/02 09:57:43 jmatthew Exp $	*/
/*
 * Copyright (c) 2022 Jonathan Matthew <jmatthew@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>

#include <machine/intr.h>
#include <machine/bus.h>
#include <machine/fdt.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/fdt.h>

#include <dev/clock_subr.h>

extern todr_chip_handle_t todr_handle;

/* Registers. */
#define RTC_STATUS		0x0000
#define RTC_TIME		0x000c
#define RTC_CONF_TEST		0x001c

#define RTC_TIMING_CTL		0x0000
#define  RTC_PERIOD_SHIFT	0
#define  RTC_PERIOD_MASK	(0x3ff << RTC_PERIOD_SHIFT)
#define  RTC_READ_DELAY_SHIFT 	26
#define  RTC_READ_DELAY_MASK 	(0x1f << RTC_READ_DELAY_SHIFT)


#define HREAD4(sc, reg)							\
	(bus_space_read_4((sc)->sc_iot, (sc)->sc_ioh, (reg)))
#define HWRITE4(sc, reg, val)						\
	bus_space_write_4((sc)->sc_iot, (sc)->sc_ioh, (reg), (val))

struct mvortc_softc {
	struct device		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	bus_space_handle_t	sc_soc_ioh;

	struct todr_chip_handle sc_todr;
};

int mvortc_match(struct device *, void *, void *);
void mvortc_attach(struct device *, struct device *, void *);

const struct cfattach	mvortc_ca = {
	sizeof (struct mvortc_softc), mvortc_match, mvortc_attach
};

struct cfdriver mvortc_cd = {
	NULL, "mvortc", DV_DULL
};

int	mvortc_gettime(struct todr_chip_handle *, struct timeval *);
int	mvortc_settime(struct todr_chip_handle *, struct timeval *);

int
mvortc_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;

	return OF_is_compatible(faa->fa_node, "marvell,armada-380-rtc");
}

void
mvortc_attach(struct device *parent, struct device *self, void *aux)
{
	struct mvortc_softc *sc = (struct mvortc_softc *)self;
	struct fdt_attach_args *faa = aux;
	uint32_t reg;

	if (faa->fa_nreg < 2) {
		printf(": no registers\n");
		return;
	}

	sc->sc_iot = faa->fa_iot;

	if (bus_space_map(sc->sc_iot, faa->fa_reg[0].addr,
	    faa->fa_reg[0].size, 0, &sc->sc_ioh)) {
		printf(": can't map registers\n");
		return;
	}

	if (bus_space_map(sc->sc_iot, faa->fa_reg[1].addr,
	    faa->fa_reg[1].size, 0, &sc->sc_soc_ioh)) {
		bus_space_unmap(sc->sc_iot, sc->sc_ioh, faa->fa_reg[0].size);
		printf(": can't map soc registers\n");
		return;
	}

	/* Magic to make bus access actually work. */
	reg = bus_space_read_4(sc->sc_iot, sc->sc_soc_ioh, RTC_TIMING_CTL);
	reg &= ~RTC_PERIOD_MASK;
	reg |= (0x3ff << RTC_PERIOD_SHIFT);
	reg &= ~RTC_READ_DELAY_MASK;
	reg |= (0x1f << RTC_READ_DELAY_SHIFT);
	bus_space_write_4(sc->sc_iot, sc->sc_soc_ioh, RTC_TIMING_CTL, reg);
	printf("\n");

	sc->sc_todr.cookie = sc;
	sc->sc_todr.todr_gettime = mvortc_gettime;
	sc->sc_todr.todr_settime = mvortc_settime;
	todr_handle = &sc->sc_todr;
}

uint32_t
mvortc_read(struct mvortc_softc *sc, int reg)
{
	uint32_t sample, mode;
	uint32_t samples[100];
	int counts[100];
	int i, j, last;

	memset(samples, 0, sizeof(samples));
	memset(counts, 0, sizeof(counts));
	last = 0;
	for (i = 0; i < nitems(samples); i++) {
		sample = HREAD4(sc, reg);

		for (j = 0; j < last; j++) {
			if (samples[j] == sample)
				break;
		}

		if (j < last) {
			counts[j]++;
		} else {
			samples[last] = sample;
			counts[last] = 1;
			last++;
		}
	}

	j = 0;
	mode = 0;
	for (i = 0; i < last; i++) {
		if (counts[i] > mode) {
			mode = counts[i];
			j = i;
		}
	}

	return samples[j];
}

void
mvortc_write(struct mvortc_softc *sc, int reg, uint32_t val)
{
	HWRITE4(sc, RTC_STATUS, 0);
	HWRITE4(sc, RTC_STATUS, 0);
	HWRITE4(sc, reg, val);
	delay(5);
}

int
mvortc_gettime(struct todr_chip_handle *handle, struct timeval *tv)
{
	struct mvortc_softc *sc = handle->cookie;

	tv->tv_sec = mvortc_read(sc, RTC_TIME);
	tv->tv_usec = 0;
	return 0;
}

int
mvortc_settime(struct todr_chip_handle *handle, struct timeval *tv)
{
	struct mvortc_softc *sc = handle->cookie;
	uint32_t reg;

	reg = mvortc_read(sc, RTC_CONF_TEST);
	if (reg & 0xff) {
		mvortc_write(sc, RTC_CONF_TEST, 0);
		delay(500);
		mvortc_write(sc, RTC_TIME, 0);
		mvortc_write(sc, RTC_STATUS, 0x03);
	}

	mvortc_write(sc, RTC_TIME, tv->tv_sec);
	return 0;
}
