## The SoC addresses

No reference tree to compare with (devices/redwood/prebuilt/device-tree.dtb is missing).

| Argument | Value | Against the reference |
|---|---|---|
| `soc_ipa_base` | `0x1e40000` |  |
| `soc_gsi_base` | `0x1e04000` |  |
| `soc_ipcc` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_aoss` | `0xc300000` |  |
| `soc_rsc` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_apps_smmu` | `0x15000000` |  |
| `soc_cmd_db` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_mpss_mem` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `modem_ipa_fw_at` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_smem` | `0x86000000` |  |
| `soc_imem` | `0x14600000` |  |
| `soc_ipa_stream` | `0x520` |  |
| `soc_ipa_stream_uc` | `0x522` |  |
| `soc_pas_mpss` | `0x4` |  |
| `soc_pas_ipa` | `0xf` |  |
| `soc_ipa_fw` | `ipa_fws` |  |

`soc.args` is what `tools/soc-args.py` read out of the tree; `pull-soc.sh` makes it again from the phone on
adb. The run scripts add it to the kernel command line. Where each argument comes from in the tree:
`devices/redwood/README.md`.

