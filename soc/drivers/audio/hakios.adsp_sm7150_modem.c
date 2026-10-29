/*
 * The Mi 9T's modem (MPSS), beside the audio DSP: hakios.adsp_sm7150.c compiled a second time, as the modem's.
 * Everything that file keeps is its own (static), so this copy is a second instance with state of its own; only
 * what it offers the rest of the kernel is renamed. Boot-arg adsp_modem=1 starts it (the Wi-Fi's firmware runs on
 * the modem); adsp=1 is the audio DSP's. It registers its stop and quiesce with loader_return itself.
 */
#define HAKIOS_SM7150_MODEM_COPY 1
#define hakios_adsp_apr                hakios_adsp_modem_apr
#define hakios_adsp_apr_events         hakios_adsp_modem_apr_events
#define hakios_adsp_apr_events_second  hakios_adsp_modem_apr_events_second
#define hakios_adsp_assign             hakios_adsp_modem_assign
#define hakios_adsp_fw_file            hakios_adsp_modem_fw_file
#define hakios_adsp_fw_free            hakios_adsp_modem_fw_free
#define hakios_adsp_given              hakios_adsp_modem_given
#define hakios_adsp_init               hakios_adsp_modem_init
#define hakios_adsp_quiesce            hakios_adsp_modem_quiesce
#define hakios_adsp_stop               hakios_adsp_modem_stop
#include "hakios.adsp_sm7150.c"
