/*
 * Clocks, as snoc.c asks for them: optional ones by the device tree's "clocks". The Mi 9T's icnss
 * node names none (its reference clock is the radio's own), so there are none to get or start.
 */
#ifndef _LX_LINUX_CLK_H_
#define _LX_LINUX_CLK_H_
struct device;
struct clk;
struct clk_bulk_data {
	const char *id;
	struct clk *clk;
};
static inline int
devm_clk_bulk_get_optional(struct device *dev, int n, struct clk_bulk_data *clks)
{
	return (0);
}
static inline int
clk_bulk_prepare_enable(int n, const struct clk_bulk_data *clks)
{
	return (0);
}
static inline void
clk_bulk_disable_unprepare(int n, const struct clk_bulk_data *clks)
{
}
#endif
