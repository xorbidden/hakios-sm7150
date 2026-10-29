/*
 * ACPI, which a phone has not got: as much as ath10k's mac.c names (its WRDD regulatory lookup, a
 * laptop's), with no table to find anything in -- acpi_get_handle() always fails, and mac.c then
 * keeps the regulatory domain the firmware has. In place of LinuxKPI's, which brings in ACPICA.
 */
#ifndef _LX_LINUX_ACPI_H_
#define _LX_LINUX_ACPI_H_
#include <linux/types.h>

typedef void *acpi_handle;
typedef u32 acpi_status;
typedef char *acpi_string;
#define AE_NOT_FOUND		0x0005
#define ACPI_FAILURE(s)		((s) != 0)
#define ACPI_ALLOCATE_BUFFER	((size_t)-1)
#define ACPI_TYPE_INTEGER	0x01
#define ACPI_TYPE_PACKAGE	0x04
#define ACPI_HANDLE(dev)	((acpi_handle)NULL)

union acpi_object {
	u32 type;
	struct { u32 type; u64 value; } integer;
	struct { u32 type; u32 count; union acpi_object *elements; } package;
};
struct acpi_buffer {
	size_t length;
	void *pointer;
};

static inline acpi_status
acpi_get_handle(acpi_handle parent, acpi_string path, acpi_handle *out)
{
	return (AE_NOT_FOUND);
}
static inline acpi_status
acpi_evaluate_object(acpi_handle h, acpi_string path, void *args, struct acpi_buffer *out)
{
	return (AE_NOT_FOUND);
}
#endif
