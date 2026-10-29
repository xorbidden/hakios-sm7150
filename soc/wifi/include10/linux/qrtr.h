/*
 * QRTR's socket address: the kernel's QMI helpers define it (bsd/sys/hakios.qmi.h, through our
 * linux/soc/qcom/qmi.h), and LinuxKPI's linux/qrtr.h would define it a second time.
 */
#ifndef _LX_LINUX_QRTR_H_
#define _LX_LINUX_QRTR_H_
#include <linux/soc/qcom/qmi.h>
#endif
