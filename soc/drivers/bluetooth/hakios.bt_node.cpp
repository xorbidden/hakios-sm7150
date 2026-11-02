/*
 * HakiosBluetoothNode: the registry entry by which Darwin's bluetoothd finds the Bluetooth controller (bt=6).
 *
 * bluetoothd looks for it with IOServiceNameMatching("bluetooth") and reads its properties (local-mac-address,
 * transport-speed, ...) as a phone's device tree has them; without one it says "Unable to match bluetooth service in
 * device tree" and exits, and launchd starts it again every 10 s (2 November). On the iPhone X the node is
 * /arm-io/uart1/bluetooth: a controller on a UART, transport-encoding 3 (H4BC) at 3 Mbaud. This one has the same
 * properties but transport-encoding 1, plain H4: with 3, bluetoothd asks the UART's kernel control for a Skywalk
 * channel (getsockopt 0x40105412, os_channel_create); with 1 it reads and writes the control's socket, as BlueTool
 * does (hakios.bt_uart.c). And the address of our controller, the WCN3990, as its HCI_Read_BD_ADDR gives it.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>

extern "C" void HakiosBluetoothNodeStart(const uint8_t * address);

/* a property as a device tree has it: the bytes, as data */
static void
property(IOService * node, const char * name, const void * bytes, unsigned length)
{
	OSData * data = OSData::withBytes(bytes, length);

	if (data != NULL) {
		node->setProperty(name, data);
		data->release();
	}
}

static void
property32(IOService * node, const char * name, uint32_t value)
{
	property(node, name, &value, sizeof(value));
}

/* Once, when the controller is up; address as HCI gives it (least significant byte first). */
void
HakiosBluetoothNodeStart(const uint8_t * address)
{
	static const char compatible[] = "bluetooth,n88";
	static const char type[] = "bluetooth";
	IOService * platform = IOService::getPlatform();
	IOService * node = OSTypeAlloc(IOService);
	uint8_t mac[6];

	if (platform == NULL || node == NULL || !node->init()) {
		OSSafeReleaseNULL(node);
		IOLog("bt: 6: cannot make the bluetooth node\n");
		return;
	}
	for (unsigned i = 0; i < 6; i++) {
		mac[i] = address[5 - i];        /* (a device tree's address is written most significant byte first) */
	}
	property(node, "compatible", compatible, sizeof(compatible));
	property(node, "device_type", type, sizeof(type));
	property(node, "local-mac-address", mac, sizeof(mac));
	property32(node, "transport-encoding", 1);         /* plain H4: bluetoothd's 3, H4BC, is a Skywalk channel */
	property32(node, "transport-speed", 3000000);
	property32(node, "vendor-id", 0x05ac);
	property32(node, "product-id", 0x12a0);
	property32(node, "supported-profiles", 0x2ffb);
	property32(node, "coex", 2);
	if (!node->attach(platform)) {
		node->release();
		IOLog("bt: 6: the bluetooth node cannot be attached\n");
		return;
	}
	node->setName("bluetooth");
	if (!node->start(platform)) {
		node->detach(platform);
		node->release();
		IOLog("bt: 6: the bluetooth node did not start\n");
		return;
	}
	node->registerService();
	IOLog("bt: 6: the bluetooth node is there for bluetoothd: %02x:%02x:%02x:%02x:%02x:%02x, H4 (transport 1) at 3 Mbaud\n", mac[0],
	    mac[1], mac[2], mac[3], mac[4], mac[5]);
}
