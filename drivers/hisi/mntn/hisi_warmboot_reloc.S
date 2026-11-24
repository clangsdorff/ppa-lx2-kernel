/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/kexec.h>
#include <asm/page.h>
#include <asm/sysreg.h>

/*
 * hisi_warmboot_relocate(desc_phys, dtb_phys, kernel_entry_phys)
 *   x0 = physical address of terminator-ended warmboot_seg_desc array
 *   x1 = physical address of prepared FDT (0xba800000)
 *   x2 = physical address of kernel Image entry (0x00080000)
 */
ENTRY(hisi_warmboot_relocate)
	mov	x18, x0
	mov	x17, x1
	mov	x16, x2
	raw_dcache_line_size x15, x0

	mrs	x0, CurrentEL
	cmp	x0, #CurrentEL_EL2
	b.ne	1f
	mrs	x0, sctlr_el2
	ldr	x1, =SCTLR_ELx_FLAGS
	bic	x0, x0, x1
	pre_disable_mmu_workaround
	msr	sctlr_el2, x0
	isb
1:

.Lseg_loop:
	ldr	x13, [x18, #0]
	ldr	x12, [x18, #8]
	ldr	x11, [x18, #16]
	add	x18, x18, #24
	cbz	x12, .Ldone

.Lpage_loop:
	ldr	x10, [x11], #8

	mov	x0, x13
	add	x9, x0, #PAGE_SIZE
	sub	x1, x15, #1
	bic	x0, x0, x1
2:	dc	ivac, x0
	add	x0, x0, x15
	cmp	x0, x9
	b.lo	2b
	dsb	sy

	mov	x20, x13
	mov	x21, x10
	add	x9, x13, #PAGE_SIZE
3:	ldp	x0, x1, [x21, #0]
	ldp	x2, x3, [x21, #16]
	ldp	x4, x5, [x21, #32]
	ldp	x6, x7, [x21, #48]
	add	x21, x21, #64
	stp	x0, x1, [x20, #0]
	stp	x2, x3, [x20, #16]
	stp	x4, x5, [x20, #32]
	stp	x6, x7, [x20, #48]
	add	x20, x20, #64
	cmp	x20, x9
	b.lo	3b

	mov	x0, x13
	sub	x1, x15, #1
	bic	x0, x0, x1
4:	dc	civac, x0
	add	x0, x0, x15
	cmp	x0, x9
	b.lo	4b
	dsb	sy

	add	x13, x13, #PAGE_SIZE
	subs	x12, x12, #1
	b.ne	.Lpage_loop
	b	.Lseg_loop

.Ldone:
	dsb	sy
	ic	ialluis
	ic	iallu
	dsb	sy
	isb

	mov	x0, x17
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x16
ENDPROC(hisi_warmboot_relocate)

.ltorg

.align 3
.Lcopy_end:

.globl hisi_warmboot_relocate_size
hisi_warmboot_relocate_size:
	.quad	.Lcopy_end - hisi_warmboot_relocate
