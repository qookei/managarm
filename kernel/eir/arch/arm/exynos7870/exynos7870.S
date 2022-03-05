.section .text.init
.global eirEntry
eirEntry:
	b .L1
	nop
	.quad 0x80000 // Image offset
	.quad 0 // Image size (0 = unbounded)
	.quad 2 // 4K pages
	.quad 0 // Reserved
	.quad 0 // Reserved
	.quad 0 // Reserved
	.quad 0x644d5241 // Magic
	.quad 0 // Reserved

.L1:
	mov x7, x0

	// Get current execution level
	mrs x0, currentel
	and x0, x0, #12
	cmp x0, #4 // EL1
	b.eq .L2
	// Halt if not in EL1
	b .halt

.L2:
	.extern eirStackTop
	//ldr x1, =eirStackTop
	ldr x1, =0xC0000000
	mov sp, x1

	mov x0, xzr
	orr x0, x0, #(1 << 29)
	orr x0, x0, #(1 << 28)
	orr x0, x0, #(1 << 23)
	orr x0, x0, #(1 << 22)
	orr x0, x0, #(1 << 20)
	orr x0, x0, #(1 << 11)
	orr x0, x0, #(1 << 12)
	orr x0, x0, #(1 << 3)
	orr x0, x0, #(1 << 2)
	msr sctlr_el1, x0

	// Load vector table
	.extern eirExcVectors
	ldr x0, =eirExcVectors
	//msr vbar_el1, x0

	msr daifset, #15
	isb
	dsb sy

	// Zero out BSS
	.extern eirBssStart
	.extern eirBssEnd
	ldr x5, =eirBssStart
	ldr x6, =eirBssEnd
.loop:
	cmp x5, x6
	b.eq .enter
	str xzr, [x5]
	add x5, x5, #8
	b .loop

.enter:
	.extern eirExynosMain
	mov x0, x7
	// x0 -> pointer to device tree binary
	bl eirExynosMain

.halt:
	wfe
	b .halt
