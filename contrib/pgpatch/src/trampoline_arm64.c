/*-------------------------------------------------------------------------
 *
 * trampoline_arm64.c
 *		Inline code patching on macOS/arm64 for pgpatch.
 *
 * We make the target's (originally r-x, code-signed) text page writable via
 * mach_vm_protect with VM_PROT_COPY (copy-on-write, so we may write even
 * though the page's maximum protection excludes WRITE), overwrite the prologue
 * with an absolute-jump stub, restore r-x protection, and flush the icache.
 * At no point is a page simultaneously writable and executable, so this is
 * compatible with the hardened runtime and needs no special entitlements.
 *
 * This uses "replace" semantics: the hook is called instead of the original,
 * and we never branch back into the original body, so the overwritten
 * instructions need no relocation.  Wrapping (calling through to the original)
 * would require relocating any PC-relative instructions in the stolen bytes.
 *
 *-------------------------------------------------------------------------
 */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <libkern/OSCacheControl.h>
#include <string.h>

#include "trampoline.h"

/*
 * arm64 absolute-jump stub (16 bytes), position independent:
 *   LDR  X16, #8      ; 0x58000050  -> load the 8 bytes at PC+8 into X16
 *   BR   X16          ; 0xD61F0200  -> branch to it
 *   .quad <target>    ; absolute 64-bit destination
 */
static void
build_stub(uint8_t *buf, void *dest)
{
	uint32_t	ldr = 0x58000050u;
	uint32_t	br = 0xD61F0200u;
	uint64_t	d = (uint64_t) dest;

	memcpy(buf + 0, &ldr, 4);
	memcpy(buf + 4, &br, 4);
	memcpy(buf + 8, &d, 8);
}

static bool
write_code(void *dst, const void *src, size_t len)
{
	mach_port_t self = mach_task_self();
	mach_vm_address_t addr = (mach_vm_address_t) dst;

	if (mach_vm_protect(self, addr, len, FALSE,
						VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
		return false;

	memcpy(dst, src, len);

	if (mach_vm_protect(self, addr, len, FALSE,
						VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS)
		return false;

	sys_icache_invalidate(dst, len);
	return true;
}

bool
pgpatch_arch_patch(void *target, void *hook, uint8_t *saved)
{
	uint8_t		stub[PGPATCH_STUB_LEN];

	memcpy(saved, target, PGPATCH_STUB_LEN);
	build_stub(stub, hook);
	return write_code(target, stub, PGPATCH_STUB_LEN);
}

bool
pgpatch_arch_unpatch(void *target, const uint8_t *saved)
{
	return write_code(target, saved, PGPATCH_STUB_LEN);
}
