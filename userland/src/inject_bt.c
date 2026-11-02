/*
 * libhakios.bt.dylib: inside BlueTool only (DEVICE_INJECT, devices/davinci/userland.sh; BT_JOBS=1).
 *
 * BlueTool sets up the iPhone X's Bluetooth chip, a Broadcom one, from scripts built into it, or from
 * /etc/bluetool/<model>.<sequence>.script when there is one -- but it looks for that file only when MobileGestalt's
 * ReleaseType is "Vendor" or "Internal" (runCommandForCurrentConfig, 2 November). The Mi 9T's chip is the WCN3990, a
 * Qualcomm one, patched and reset by the kernel (bt=6); our scripts only open its port and reset it. So here, and for
 * BlueTool alone, ReleaseType is "Vendor"; every other answer is MobileGestalt's own.
 */
#include <CoreFoundation/CoreFoundation.h>

extern CFTypeRef MGCopyAnswer(CFStringRef question, CFDictionaryRef options);

static CFTypeRef
our_MGCopyAnswer(CFStringRef question, CFDictionaryRef options)
{
	if (question != NULL && CFEqual(question, CFSTR("ReleaseType"))) {
		return CFRetain(CFSTR("Vendor"));
	}
	return MGCopyAnswer(question, options);
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} mg_interpose __attribute__((section("__DATA,__interpose"))) = {
	(const void *)our_MGCopyAnswer, (const void *)MGCopyAnswer
};
