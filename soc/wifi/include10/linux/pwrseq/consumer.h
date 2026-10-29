/*
 * Power sequencing: for a radio whose supplies a sequencer owns (Linux's newer device trees). On the
 * Mi 9T the supplies are the icnss node's own regulators: there is no sequencer, and NULL is the
 * descriptor for "none", which Linux's pwrseq_power_on() and _off() take as nothing to do.
 */
#ifndef _LX_LINUX_PWRSEQ_CONSUMER_H_
#define _LX_LINUX_PWRSEQ_CONSUMER_H_
struct device;
struct pwrseq_desc;
static inline struct pwrseq_desc *
devm_pwrseq_get(struct device *dev, const char *target)
{
	return (NULL);
}
static inline int
pwrseq_power_on(struct pwrseq_desc *desc)
{
	return (0);
}
static inline int
pwrseq_power_off(struct pwrseq_desc *desc)
{
	return (0);
}
#endif
