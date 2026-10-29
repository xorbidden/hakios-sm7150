/*
 * Notices of the modem's restarts. The modem is started and stopped by the kernel (hakios.adsp.c,
 * adsp_modem=1), which tells the bus file; snoc.c's notifier is kept there and called from it.
 */
#ifndef _LX_LINUX_QCOM_RPROC_H_
#define _LX_LINUX_QCOM_RPROC_H_
#include <linux/types.h>
struct notifier_block;
enum qcom_ssr_notify_type {
	QCOM_SSR_BEFORE_POWERUP,
	QCOM_SSR_AFTER_POWERUP,
	QCOM_SSR_BEFORE_SHUTDOWN,
	QCOM_SSR_AFTER_SHUTDOWN,
};
struct qcom_ssr_notify_data {
	const char *name;
	bool crashed;
};
void *qcom_register_ssr_notifier(const char *name, struct notifier_block *nb);
int qcom_unregister_ssr_notifier(void *notify, struct notifier_block *nb);
#endif
