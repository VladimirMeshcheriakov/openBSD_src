/*	$OpenBSD: rktemp.c,v 1.12 2023/03/05 09:57:32 kettenis Exp $	*/
/*
 * Copyright (c) 2017 Mark Kettenis <kettenis@openbsd.org>
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
#include <sys/sensors.h>

#include <machine/intr.h>
#include <machine/bus.h>
#include <machine/fdt.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_clock.h>
#include <dev/ofw/ofw_misc.h>
#include <dev/ofw/ofw_pinctrl.h>
#include <dev/ofw/ofw_thermal.h>
#include <dev/ofw/fdt.h>

/* Registers */
#define TSADC_USER_CON			0x0000
#define  TSADC_USER_CON_INTER_PD_SOC_SHIFT	6
#define TSADC_AUTO_CON			0x0004
#define  TSADC_AUTO_CON_TSHUT_POLARITY	(1 << 8)
#define  TSADC_AUTO_CON_SRC3_EN		(1 << 7)
#define  TSADC_AUTO_CON_SRC2_EN		(1 << 6)
#define  TSADC_AUTO_CON_SRC1_EN		(1 << 5)
#define  TSADC_AUTO_CON_SRC0_EN		(1 << 4)
#define  TSADC_AUTO_CON_TSADC_Q_SEL	(1 << 1)
#define  TSADC_AUTO_CON_AUTO_EN		(1 << 0)
#define TSADC_INT_EN			0x0008
#define  TSADC_INT_EN_TSHUT_2CRU_EN_SRC3	(1 << 11)
#define  TSADC_INT_EN_TSHUT_2CRU_EN_SRC2	(1 << 10)
#define  TSADC_INT_EN_TSHUT_2CRU_EN_SRC1	(1 << 9)
#define  TSADC_INT_EN_TSHUT_2CRU_EN_SRC0	(1 << 8)
#define  TSADC_INT_EN_TSHUT_2GPIO_EN_SRC3	(1 << 7)
#define  TSADC_INT_EN_TSHUT_2GPIO_EN_SRC2	(1 << 6)
#define  TSADC_INT_EN_TSHUT_2GPIO_EN_SRC1	(1 << 5)
#define  TSADC_INT_EN_TSHUT_2GPIO_EN_SRC0	(1 << 4)
#define TSADC_INT_PD			0x000c
#define  TSADC_INT_PD_TSHUT_O_SRC0		(1 << 4)
#define  TSADC_INT_PD_TSHUT_O_SRC1		(1 << 5)
#define  TSADC_INT_PD_TSHUT_O_SRC2		(1 << 6)
#define  TSADC_INT_PD_TSHUT_O_SRC3		(1 << 7)
#define TSADC_DATA0			0x0020
#define TSADC_DATA1			0x0024
#define TSADC_DATA2			0x0028
#define TSADC_DATA3			0x002c
#define TSADC_COMP0_INT			0x0030
#define TSADC_COMP1_INT			0x0034
#define TSADC_COMP2_INT			0x0038
#define TSADC_COMP3_INT			0x003c
#define TSADC_COMP0_SHUT		0x0040
#define TSADC_COMP1_SHUT		0x0044
#define TSADC_COMP2_SHUT		0x0048
#define TSADC_COMP3_SHUT		0x004c
#define TSADC_HIGHT_INT_DEBOUNCE	0x0060
#define TSADC_HIGHT_TSHUT_DEBOUNCE	0x0064
#define TSADC_AUTO_PERIOD		0x0068
#define TSADC_AUTO_PERIOD_HT		0x006c

/* RK3568 */
#define RK3568_GRF_TSADC_CON		0x0600
#define  RK3568_GRF_TSADC_EN		(1 << 8)
#define  RK3568_GRF_TSADC_ANA_REG(idx)	(1 << (idx))

#define HREAD4(sc, reg)							\
	(bus_space_read_4((sc)->sc_iot, (sc)->sc_ioh, (reg)))
#define HWRITE4(sc, reg, val)						\
	bus_space_write_4((sc)->sc_iot, (sc)->sc_ioh, (reg), (val))

struct rktemp_entry {
	int32_t temp;
	int32_t code;
};

/* RK3288 conversion table. */
const struct rktemp_entry rk3288_temps[] = {
	{ -40000, 3800 },
	{ -35000, 3792 },
	{ -30000, 3783 },
	{ -25000, 3774 },
	{ -20000, 3765 },
	{ -15000, 3756 },
	{ -10000, 3747 },
	{  -5000, 3737 },
	{      0, 3728 },
	{   5000, 3718 },
	{  10000, 3708 },
	{  15000, 3698 },
	{  20000, 3688 },
	{  25000, 3678 },
	{  30000, 3667 },
	{  35000, 3656 },
	{  40000, 3645 },
	{  45000, 3634 },
	{  50000, 3623 },
	{  55000, 3611 },
	{  60000, 3600 },
	{  65000, 3588 },
	{  70000, 3575 },
	{  75000, 3563 },
	{  80000, 3550 },
	{  85000, 3537 },
	{  90000, 3524 },
	{  95000, 3510 },
	{ 100000, 3496 },
	{ 105000, 3482 },
	{ 110000, 3467 },
	{ 115000, 3452 },
	{ 120000, 3437 },
	{ 125000, 3421 },
};

const char *const rk3288_names[] = { "", "CPU", "GPU" };

/* RK3328 conversion table. */
const struct rktemp_entry rk3328_temps[] = {
	{ -40000, 296 },
	{ -35000, 304 },
	{ -30000, 313 },
	{ -20000, 331 },
	{ -15000, 340 },
	{ -10000, 349 },
	{  -5000, 359 },
	{      0, 368 },
	{   5000, 378 },
	{  10000, 388 },
	{  15000, 398 },
	{  20000, 408 },
	{  25000, 418 },
	{  30000, 429 },
	{  35000, 440 },
	{  40000, 451 },
	{  45000, 462 },
	{  50000, 473 },
	{  55000, 485 },
	{  60000, 496 },
	{  65000, 508 },
	{  70000, 521 },
	{  75000, 533 },
	{  80000, 546 },
	{  85000, 559 },
	{  90000, 572 },
	{  95000, 586 },
	{ 100000, 600 },
	{ 105000, 614 },
	{ 110000, 629 },
	{ 115000, 644 },
	{ 120000, 659 },
	{ 125000, 675 },
};

const char *const rk3308_names[] = { "CPU", "GPU" };
const char *const rk3328_names[] = { "CPU" };

/* RK3399 conversion table. */
const struct rktemp_entry rk3399_temps[] = {
	{ -40000, 402 },
	{ -35000, 410 },
	{ -30000, 419 },
	{ -25000, 427 },
	{ -20000, 436 },
	{ -15000, 444 },
	{ -10000, 453 },
	{  -5000, 461 },
	{      0, 470 },
	{   5000, 478 },
	{  10000, 487 },
	{  15000, 496 },
	{  20000, 504 },
	{  25000, 513 },
	{  30000, 521 },
	{  35000, 530 },
	{  40000, 538 },
	{  45000, 547 },
	{  50000, 555 },
	{  55000, 564 },
	{  60000, 573 },
	{  65000, 581 },
	{  70000, 590 },
	{  75000, 599 },
	{  80000, 607 },
	{  85000, 616 },
	{  90000, 624 },
	{  95000, 633 },
	{ 100000, 642 },
	{ 105000, 650 },
	{ 110000, 659 },
	{ 115000, 668 },
	{ 120000, 677 },
	{ 125000, 685 },
};

const char *const rk3399_names[] = { "CPU", "GPU" };

/* RK3568 conversion table. */
const struct rktemp_entry rk3568_temps[] = {
	{ -40000, 1584 },
	{ -35000, 1620 },
	{ -30000, 1652 },
	{ -25000, 1688 },
	{ -20000, 1720 },
	{ -15000, 1756 },
	{ -10000, 1788 },
	{  -5000, 1824 },
	{      0, 1856 },
	{   5000, 1892 },
	{  10000, 1924 },
	{  15000, 1956 },
	{  20000, 1992 },
	{  25000, 2024 },
	{  30000, 2060 },
	{  35000, 2092 },
	{  40000, 2128 },
	{  45000, 2160 },
	{  50000, 2196 },
	{  55000, 2228 },
	{  60000, 2264 },
	{  65000, 2300 },
	{  70000, 2332 },
	{  75000, 2368 },
	{  80000, 2400 },
	{  85000, 2436 },
	{  90000, 2468 },
	{  95000, 2500 },
	{ 100000, 2536 },
	{ 105000, 2572 },
	{ 110000, 2604 },
	{ 115000, 2636 },
	{ 120000, 2672 },
	{ 125000, 2704 },
};

const char *const rk3568_names[] = { "CPU", "GPU" };

struct rktemp_softc {
	struct device		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	int			sc_node;

	const struct rktemp_entry *sc_temps;
	int			sc_ntemps;

	struct ksensor		sc_sensors[3];
	int			sc_nsensors;
	struct ksensordev	sc_sensordev;

	struct thermal_sensor	sc_ts;
};

int	rktemp_match(struct device *, void *, void *);
void	rktemp_attach(struct device *, struct device *, void *);

const struct cfattach rktemp_ca = {
	sizeof (struct rktemp_softc), rktemp_match, rktemp_attach
};

struct cfdriver rktemp_cd = {
	NULL, "rktemp", DV_DULL
};

void	rktemp_rk3568_init(struct rktemp_softc *);
int32_t rktemp_calc_code(struct rktemp_softc *, int32_t);
int32_t rktemp_calc_temp(struct rktemp_softc *, int32_t);
int	rktemp_valid(struct rktemp_softc *, int32_t);
void	rktemp_refresh_sensors(void *);
int32_t	rktemp_get_temperature(void *, uint32_t *);

int
rktemp_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;

	return (OF_is_compatible(faa->fa_node, "rockchip,rk3288-tsadc") ||
	    OF_is_compatible(faa->fa_node, "rockchip,rk3308-tsadc") ||
	    OF_is_compatible(faa->fa_node, "rockchip,rk3328-tsadc") ||
	    OF_is_compatible(faa->fa_node, "rockchip,rk3399-tsadc") ||
	    OF_is_compatible(faa->fa_node, "rockchip,rk3568-tsadc"));
}

void
rktemp_attach(struct device *parent, struct device *self, void *aux)
{
	struct rktemp_softc *sc = (struct rktemp_softc *)self;
	struct fdt_attach_args *faa = aux;
	const char *const *names;
	uint32_t mode, polarity, temp;
	uint32_t auto_con, int_en;
	uint32_t inter_pd_soc;
	int auto_period, auto_period_ht;
	int i;

	if (faa->fa_nreg < 1) {
		printf(": no registers\n");
		return;
	}

	sc->sc_iot = faa->fa_iot;
	sc->sc_node = faa->fa_node;

	if (bus_space_map(sc->sc_iot, faa->fa_reg[0].addr,
	    faa->fa_reg[0].size, 0, &sc->sc_ioh)) {
		printf(": can't map registers\n");
		return;
	}

	printf("\n");

	if (OF_is_compatible(sc->sc_node, "rockchip,rk3288-tsadc")) {
		sc->sc_temps = rk3288_temps;
		sc->sc_ntemps = nitems(rk3288_temps);
		sc->sc_nsensors = 3;
		names = rk3288_names;
		inter_pd_soc = 13;
		auto_period = 250;	/* 250 ms */
		auto_period_ht = 50;	/* 50 ms */
	} else if (OF_is_compatible(sc->sc_node, "rockchip,rk3308-tsadc")) {
		sc->sc_temps = rk3328_temps;
		sc->sc_ntemps = nitems(rk3328_temps);
		sc->sc_nsensors = 2;
		names = rk3308_names;
		inter_pd_soc = 13;
		auto_period = 1875;	/* 2.5 ms */
		auto_period_ht = 1875;	/* 2.5 ms */
	} else if (OF_is_compatible(sc->sc_node, "rockchip,rk3328-tsadc")) {
		sc->sc_temps = rk3328_temps;
		sc->sc_ntemps = nitems(rk3328_temps);
		sc->sc_nsensors = 1;
		names = rk3328_names;
		inter_pd_soc = 13;
		auto_period = 1875;	/* 2.5 ms */
		auto_period_ht = 1875;	/* 2.5 ms */
	} else if (OF_is_compatible(sc->sc_node, "rockchip,rk3399-tsadc")) {
		sc->sc_temps = rk3399_temps;
		sc->sc_ntemps = nitems(rk3399_temps);
		sc->sc_nsensors = 2;
		names = rk3399_names;
		inter_pd_soc = 13;
		auto_period = 1875;	/* 2.5 ms */
		auto_period_ht = 1875;	/* 2.5 ms */
	} else {
		sc->sc_temps = rk3568_temps;
		sc->sc_ntemps = nitems(rk3568_temps);
		sc->sc_nsensors = 2;
		names = rk3568_names;
		inter_pd_soc = 63;	/* 97 us */
		auto_period = 1622;	/* 2.5 ms */
		auto_period_ht = 1622;	/* 2.5 ms */
	}

	pinctrl_byname(sc->sc_node, "init");

	clock_set_assigned(sc->sc_node);
	clock_enable(sc->sc_node, "tsadc");
	clock_enable(sc->sc_node, "apb_pclk");

	/* Reset the TS-ADC controller block. */
	reset_assert(sc->sc_node, "tsadc-apb");
	delay(10);
	reset_deassert(sc->sc_node, "tsadc-apb");

	mode = OF_getpropint(sc->sc_node, "rockchip,hw-tshut-mode", 1);
	polarity = OF_getpropint(sc->sc_node, "rockchip,hw-tshut-polarity", 0);
	temp = OF_getpropint(sc->sc_node, "rockchip,hw-tshut-temp", 95000);

	HWRITE4(sc, TSADC_USER_CON,
	    inter_pd_soc << TSADC_USER_CON_INTER_PD_SOC_SHIFT);
	HWRITE4(sc, TSADC_AUTO_PERIOD, auto_period);
	HWRITE4(sc, TSADC_AUTO_PERIOD_HT, auto_period_ht);
	HWRITE4(sc, TSADC_HIGHT_INT_DEBOUNCE, 4);
	HWRITE4(sc, TSADC_HIGHT_TSHUT_DEBOUNCE, 4);

	if (OF_is_compatible(sc->sc_node, "rockchip,rk3568-tsadc"))
		rktemp_rk3568_init(sc);

	auto_con = HREAD4(sc, TSADC_AUTO_CON);
	auto_con |= TSADC_AUTO_CON_TSADC_Q_SEL;
	if (polarity)
		auto_con |= TSADC_AUTO_CON_TSHUT_POLARITY;
	HWRITE4(sc, TSADC_AUTO_CON, auto_con);

	/* Set shutdown limit. */
	for (i = 0; i < sc->sc_nsensors; i++) {
		HWRITE4(sc, TSADC_COMP0_SHUT + i * 4,
		    rktemp_calc_code(sc, temp));
		auto_con |= (TSADC_AUTO_CON_SRC0_EN << i);
	}
	HWRITE4(sc, TSADC_AUTO_CON, auto_con);

	/* Clear shutdown output status. */
	for (i = 0; i < sc->sc_nsensors; i++)
		HWRITE4(sc, TSADC_INT_PD, (TSADC_INT_PD_TSHUT_O_SRC0 << i));

	/* Configure mode. */
	int_en = HREAD4(sc, TSADC_INT_EN);
	for (i = 0; i < sc->sc_nsensors; i++) {
		if (mode)
			int_en |= (TSADC_INT_EN_TSHUT_2GPIO_EN_SRC0 << i);
		else
			int_en |= (TSADC_INT_EN_TSHUT_2CRU_EN_SRC0 << i);
	}
	HWRITE4(sc, TSADC_INT_EN, int_en);

	pinctrl_byname(sc->sc_node, "default");

	/* Finally turn on the ADC. */
	auto_con |= TSADC_AUTO_CON_AUTO_EN;
	HWRITE4(sc, TSADC_AUTO_CON, auto_con);

	/* Register sensors. */
	strlcpy(sc->sc_sensordev.xname, sc->sc_dev.dv_xname,
	    sizeof(sc->sc_sensordev.xname));
	for (i = 0; i < sc->sc_nsensors; i++) {
		strlcpy(sc->sc_sensors[i].desc, names[i],
		    sizeof(sc->sc_sensors[i].desc));
		sc->sc_sensors[i].type = SENSOR_TEMP;
		sc->sc_sensors[i].flags = SENSOR_FINVALID;
		sensor_attach(&sc->sc_sensordev, &sc->sc_sensors[i]);
	}
	sensordev_install(&sc->sc_sensordev);
	sensor_task_register(sc, rktemp_refresh_sensors, 5);

	sc->sc_ts.ts_node = sc->sc_node;
	sc->sc_ts.ts_cookie = sc;
	sc->sc_ts.ts_get_temperature = rktemp_get_temperature;
	thermal_sensor_register(&sc->sc_ts);
}

void
rktemp_rk3568_init(struct rktemp_softc *sc)
{
	struct regmap *rm;
	uint32_t grf;
	int i;

	grf = OF_getpropint(sc->sc_node, "rockchip,grf", 0);
	rm = regmap_byphandle(grf);
	if (rm == 0)
		return;
	
	regmap_write_4(rm, RK3568_GRF_TSADC_CON,
	    RK3568_GRF_TSADC_EN << 16 | RK3568_GRF_TSADC_EN);
	delay(15);
	for (i = 0; i <= 2; i++) {
		regmap_write_4(rm, RK3568_GRF_TSADC_CON,
		    RK3568_GRF_TSADC_ANA_REG(i) << 16 |
		    RK3568_GRF_TSADC_ANA_REG(i));
	}
	delay(100);
}

int32_t
rktemp_calc_code(struct rktemp_softc *sc, int32_t temp)
{
	const int n = sc->sc_ntemps;
	int32_t code0, delta_code;
	int32_t temp0, delta_temp;
	int i;

	if (temp <= sc->sc_temps[0].temp)
		return sc->sc_temps[0].code;
	if (temp >= sc->sc_temps[n - 1].temp)
		return sc->sc_temps[n - 1].code;

	for (i = 1; i < n; i++) {
		if (temp < sc->sc_temps[i].temp)
			break;
	}

	code0 = sc->sc_temps[i - 1].code;
	temp0 = sc->sc_temps[i - 1].temp;
	delta_code = sc->sc_temps[i].code - code0;
	delta_temp = sc->sc_temps[i].temp - temp0;

	return code0 + (temp - temp0) * delta_code / delta_temp;
}

int32_t
rktemp_calc_temp(struct rktemp_softc *sc, int32_t code)
{
	const int n = sc->sc_ntemps;
	int32_t code0, delta_code;
	int32_t temp0, delta_temp;
	int i;

	/* Handle both negative and positive temperature coefficients. */
	if (sc->sc_temps[0].code > sc->sc_temps[1].code) {
		if (code >= sc->sc_temps[0].code)
			return sc->sc_temps[0].code;
		if (code <= sc->sc_temps[n - 1].code)
			return sc->sc_temps[n - 1].temp;

		for (i = 1; i < n; i++) {
			if (code > sc->sc_temps[i].code)
				break;
		}
	} else {
		if (code <= sc->sc_temps[0].code)
			return sc->sc_temps[0].temp;
		if (code >= sc->sc_temps[n - 1].code)
			return sc->sc_temps[n - 1].temp;

		for (i = 1; i < n; i++) {
			if (code < sc->sc_temps[i].code)
				break;
		}
	}

	code0 = sc->sc_temps[i - 1].code;
	temp0 = sc->sc_temps[i - 1].temp;
	delta_code = sc->sc_temps[i].code - code0;
	delta_temp = sc->sc_temps[i].temp - temp0;

	return temp0 + (code - code0) * delta_temp / delta_code;
}

int
rktemp_valid(struct rktemp_softc *sc, int32_t code)
{
	const int n = sc->sc_ntemps;

	if (sc->sc_temps[0].code > sc->sc_temps[1].code) {
		if (code > sc->sc_temps[0].code)
			return 0;
		if (code < sc->sc_temps[n - 1].code)
			return 0;
	} else {
		if (code < sc->sc_temps[0].code)
			return 0;
		if (code > sc->sc_temps[n - 1].code)
			return 0;
	}
	return 1;
}

void
rktemp_refresh_sensors(void *arg)
{
	struct rktemp_softc *sc = arg;
	int32_t code, temp;
	int i;

	for (i = 0; i < sc->sc_nsensors; i++) {
		code = HREAD4(sc, TSADC_DATA0 + i * 4);
		temp = rktemp_calc_temp(sc, code);
		sc->sc_sensors[i].value = 273150000 + 1000 * temp;
		if (rktemp_valid(sc, code))
			sc->sc_sensors[i].flags &= ~SENSOR_FINVALID;
		else
			sc->sc_sensors[i].flags |= SENSOR_FINVALID;
	}
}

int32_t
rktemp_get_temperature(void *cookie, uint32_t *cells)
{
	struct rktemp_softc *sc = cookie;
	uint32_t idx = cells[0];
	int32_t code;

	if (idx >= sc->sc_nsensors)
		return THERMAL_SENSOR_MAX;

	code = HREAD4(sc, TSADC_DATA0 + idx * 4);
	if (rktemp_valid(sc, code))
		return rktemp_calc_temp(sc, code);
	else
		return THERMAL_SENSOR_MAX;
}
