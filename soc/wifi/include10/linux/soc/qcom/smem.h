/*
 * The shared memory's items, for ath10k's record of the firmware's version in the image table
 * (qmi.c's ath10k_qmi_add_wlan_ver_smem(), informative only): given by the bus file, on the kernel's
 * reading of the shared memory, or not at all.
 */
#ifndef _LX_LINUX_QCOM_SMEM_H_
#define _LX_LINUX_QCOM_SMEM_H_
#include <linux/types.h>
#define QCOM_SMEM_HOST_ANY	-1
void *qcom_smem_get(unsigned int host, unsigned int item, size_t *size);
#endif
