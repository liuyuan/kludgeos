# Implemented by Liu Yuan
# Reference: xv6
# Mar 29, 2009, modified Apr 2, 2009

#include <inc/mmu.h>
#include <inc/memlayout.h>

#define DEBUG 0

# Each non-boot CPU ("AP") is started up in response to a STARTUP
# IPI from the boot CPU.  Section B.4.2 of the Multi-Processor
# Specification says that the AP will start in real mode with CS:IP
# set to XY00:0000, where XY is an 8-bit value sent with the
# STARTUP. Thus this code must start at a 4096-byte boundary.
#
# Because this code sets DS to zero, it must sit
# at an address in the low 2^16 bytes.
#
# Bootothers (in kern/init.c) sends the STARTUPs, one at a time.
# It puts this code (start) at 0x7000.
# It puts the correct %esp in start-4,
# and the place to jump to in start-8.
#
# This code is identical to bootasm.S except:
#   - it does not need to enable A20
#   - it uses the address at start-4 for the %esp
#   - itjumps to the address at start-8 instead of calling bootmain
	
	.set PROT_MODE_CSEG, 0x8         # kernel code segment selector
	.set PROT_MODE_DSEG, 0x10        # kernel data segment selector
	.set CR0_PE_ON,      0x1         # protected mode enable flag

	.globl start
start:
	.code16			# Assemble for 16-bit mode
	cli			# Disable interrupts
	cld                     # String operations increment

	# Set up the important data segment registers (DS, ES, SS).
	xorw	%ax,%ax             # Segment number zero
	movw	%ax,%ds             # -> Data Segment
	movw	%ax,%es             # -> Extra Segment
	movw	%ax,%ss             # -> Stack Segment
	
#ifdef DEBUG
	# Print a diagnostic message	
	movw	$0xb874, %ax
	movw	%ax, %es
	movw	$(0x0400 + 'Y'), %es:(0x00)
#endif
	# Switch from real to protected mode, using a bootstrap GDT
	# and segment translation that makes virtual addresses 
	# identical to their physical addresses, so that the 
	# effective memory map does not change during the switch.
	lgdt	gdtdesc
	movl	%cr0, %eax
	orl	$CR0_PE_ON, %eax
	movl	%eax, %cr0

	# Jump to next instruction, but in 32-bit code segment.
	# Switches processor into 32-bit mode.
	ljmp    $PROT_MODE_CSEG, $protcseg

	.code32				# Assemble for 32-bit mode
protcseg:
	# Set up the protected-mode data segment registers
	movw    $PROT_MODE_DSEG, %ax    # Our data segment selector
	movw    %ax, %ds                # -> DS: Data Segment
	movw    %ax, %es                # -> ES: Extra Segment
	movw    %ax, %fs                # -> FS
	movw    %ax, %gs                # -> GS
	movw    %ax, %ss                # -> SS: Stack Segment
#ifdef DEBUG
	movw	$(0x0500 + 'E'), (0xb8742) # Diagnostic message
	# jmp	.			# Diagnosis: Halting it :)
#endif
	movl    start-4, %esp
	movl    start-8, %eax
	jmp     *%eax

# Bootstrap GDT
	.p2align 2                         	# force 4 byte alignment
gdt:
	SEG_NULL				# null seg
	SEG(STA_X|STA_R, 0, 0xffffffff)		# code seg
	SEG(STA_W, 0, 0xffffffff)	        # data seg

gdtdesc:
	.word   0x17                            # sizeof(gdt) - 1
	.long   gdt                  		# address gdt)
	