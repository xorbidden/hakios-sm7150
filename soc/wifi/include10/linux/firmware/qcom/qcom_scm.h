/*
 * The hypervisor's memory assignment, which ath10k's qmi.c uses to give the firmware's MSA regions to
 * the modem and the Wi-Fi. Done by the kernel (bsd/dev/hakios.adsp.c's hakios_adsp_assign()); the bus file
 * gives qcom_scm_assign_mem() on top of it.
 */
#ifndef _LX_LINUX_QCOM_SCM_H_
#define _LX_LINUX_QCOM_SCM_H_
#include <linux/types.h>
#define QCOM_SCM_VMID_HLOS		0x3
#define QCOM_SCM_VMID_MSS_MSA		0xf
#define QCOM_SCM_VMID_WLAN		0x18
#define QCOM_SCM_VMID_WLAN_CE		0x19
#define QCOM_SCM_PERM_READ		0x4
#define QCOM_SCM_PERM_WRITE		0x2
#define QCOM_SCM_PERM_EXEC		0x1
#define QCOM_SCM_PERM_RW		(QCOM_SCM_PERM_READ | QCOM_SCM_PERM_WRITE)
#define QCOM_SCM_PERM_RWX		(QCOM_SCM_PERM_RW | QCOM_SCM_PERM_EXEC)
struct qcom_scm_vmperm {
	int vmid;
	int perm;
};
int qcom_scm_assign_mem(phys_addr_t mem_addr, size_t mem_sz, u64 *src, const struct qcom_scm_vmperm *newvm,
    unsigned int dest_cnt);
#endif
