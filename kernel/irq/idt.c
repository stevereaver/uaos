/* idt.c — UAOS x86_64 IDT initialisation and 8259A PIC driver */

#include "idt.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * I/O port helpers
 * ========================================================================= */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void io_wait(void)
{
    outb(0x80, 0x00);   /* write to unused port — ~1 µs delay */
}

/* =========================================================================
 * IDT entry (16 bytes in long mode)
 * ========================================================================= */

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;       /* kernel code segment = 0x08        */
    uint8_t  ist;            /* interrupt stack table index (0)   */
    uint8_t  type_attr;      /* 0x8E = present, DPL=0, 64-bit int */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} IdtEntry;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} IdtPtr;

static IdtEntry  g_idt[256];
static ISRHandler g_handlers[256];

/* Forward declarations for all 256 stubs (defined in idt_stubs.asm) */
#define DECL_STUB(n) extern void isr_stub_##n(void);
/* 0-31 */
DECL_STUB(0)  DECL_STUB(1)  DECL_STUB(2)  DECL_STUB(3)
DECL_STUB(4)  DECL_STUB(5)  DECL_STUB(6)  DECL_STUB(7)
DECL_STUB(8)  DECL_STUB(9)  DECL_STUB(10) DECL_STUB(11)
DECL_STUB(12) DECL_STUB(13) DECL_STUB(14) DECL_STUB(15)
DECL_STUB(16) DECL_STUB(17) DECL_STUB(18) DECL_STUB(19)
DECL_STUB(20) DECL_STUB(21) DECL_STUB(22) DECL_STUB(23)
DECL_STUB(24) DECL_STUB(25) DECL_STUB(26) DECL_STUB(27)
DECL_STUB(28) DECL_STUB(29) DECL_STUB(30) DECL_STUB(31)
/* 32-47 (PIC IRQs) */
DECL_STUB(32) DECL_STUB(33) DECL_STUB(34) DECL_STUB(35)
DECL_STUB(36) DECL_STUB(37) DECL_STUB(38) DECL_STUB(39)
DECL_STUB(40) DECL_STUB(41) DECL_STUB(42) DECL_STUB(43)
DECL_STUB(44) DECL_STUB(45) DECL_STUB(46) DECL_STUB(47)
/* 48-63 */
DECL_STUB(48) DECL_STUB(49) DECL_STUB(50) DECL_STUB(51)
DECL_STUB(52) DECL_STUB(53) DECL_STUB(54) DECL_STUB(55)
DECL_STUB(56) DECL_STUB(57) DECL_STUB(58) DECL_STUB(59)
DECL_STUB(60) DECL_STUB(61) DECL_STUB(62) DECL_STUB(63)
/* 64-127 */
DECL_STUB(64)  DECL_STUB(65)  DECL_STUB(66)  DECL_STUB(67)
DECL_STUB(68)  DECL_STUB(69)  DECL_STUB(70)  DECL_STUB(71)
DECL_STUB(72)  DECL_STUB(73)  DECL_STUB(74)  DECL_STUB(75)
DECL_STUB(76)  DECL_STUB(77)  DECL_STUB(78)  DECL_STUB(79)
DECL_STUB(80)  DECL_STUB(81)  DECL_STUB(82)  DECL_STUB(83)
DECL_STUB(84)  DECL_STUB(85)  DECL_STUB(86)  DECL_STUB(87)
DECL_STUB(88)  DECL_STUB(89)  DECL_STUB(90)  DECL_STUB(91)
DECL_STUB(92)  DECL_STUB(93)  DECL_STUB(94)  DECL_STUB(95)
DECL_STUB(96)  DECL_STUB(97)  DECL_STUB(98)  DECL_STUB(99)
DECL_STUB(100) DECL_STUB(101) DECL_STUB(102) DECL_STUB(103)
DECL_STUB(104) DECL_STUB(105) DECL_STUB(106) DECL_STUB(107)
DECL_STUB(108) DECL_STUB(109) DECL_STUB(110) DECL_STUB(111)
DECL_STUB(112) DECL_STUB(113) DECL_STUB(114) DECL_STUB(115)
DECL_STUB(116) DECL_STUB(117) DECL_STUB(118) DECL_STUB(119)
DECL_STUB(120) DECL_STUB(121) DECL_STUB(122) DECL_STUB(123)
DECL_STUB(124) DECL_STUB(125) DECL_STUB(126) DECL_STUB(127)
/* 128-191 */
DECL_STUB(128) DECL_STUB(129) DECL_STUB(130) DECL_STUB(131)
DECL_STUB(132) DECL_STUB(133) DECL_STUB(134) DECL_STUB(135)
DECL_STUB(136) DECL_STUB(137) DECL_STUB(138) DECL_STUB(139)
DECL_STUB(140) DECL_STUB(141) DECL_STUB(142) DECL_STUB(143)
DECL_STUB(144) DECL_STUB(145) DECL_STUB(146) DECL_STUB(147)
DECL_STUB(148) DECL_STUB(149) DECL_STUB(150) DECL_STUB(151)
DECL_STUB(152) DECL_STUB(153) DECL_STUB(154) DECL_STUB(155)
DECL_STUB(156) DECL_STUB(157) DECL_STUB(158) DECL_STUB(159)
DECL_STUB(160) DECL_STUB(161) DECL_STUB(162) DECL_STUB(163)
DECL_STUB(164) DECL_STUB(165) DECL_STUB(166) DECL_STUB(167)
DECL_STUB(168) DECL_STUB(169) DECL_STUB(170) DECL_STUB(171)
DECL_STUB(172) DECL_STUB(173) DECL_STUB(174) DECL_STUB(175)
DECL_STUB(176) DECL_STUB(177) DECL_STUB(178) DECL_STUB(179)
DECL_STUB(180) DECL_STUB(181) DECL_STUB(182) DECL_STUB(183)
DECL_STUB(184) DECL_STUB(185) DECL_STUB(186) DECL_STUB(187)
DECL_STUB(188) DECL_STUB(189) DECL_STUB(190) DECL_STUB(191)
/* 192-255 */
DECL_STUB(192) DECL_STUB(193) DECL_STUB(194) DECL_STUB(195)
DECL_STUB(196) DECL_STUB(197) DECL_STUB(198) DECL_STUB(199)
DECL_STUB(200) DECL_STUB(201) DECL_STUB(202) DECL_STUB(203)
DECL_STUB(204) DECL_STUB(205) DECL_STUB(206) DECL_STUB(207)
DECL_STUB(208) DECL_STUB(209) DECL_STUB(210) DECL_STUB(211)
DECL_STUB(212) DECL_STUB(213) DECL_STUB(214) DECL_STUB(215)
DECL_STUB(216) DECL_STUB(217) DECL_STUB(218) DECL_STUB(219)
DECL_STUB(220) DECL_STUB(221) DECL_STUB(222) DECL_STUB(223)
DECL_STUB(224) DECL_STUB(225) DECL_STUB(226) DECL_STUB(227)
DECL_STUB(228) DECL_STUB(229) DECL_STUB(230) DECL_STUB(231)
DECL_STUB(232) DECL_STUB(233) DECL_STUB(234) DECL_STUB(235)
DECL_STUB(236) DECL_STUB(237) DECL_STUB(238) DECL_STUB(239)
DECL_STUB(240) DECL_STUB(241) DECL_STUB(242) DECL_STUB(243)
DECL_STUB(244) DECL_STUB(245) DECL_STUB(246) DECL_STUB(247)
DECL_STUB(248) DECL_STUB(249) DECL_STUB(250) DECL_STUB(251)
DECL_STUB(252) DECL_STUB(253) DECL_STUB(254) DECL_STUB(255)

/* Table of all 256 stub addresses for easy iteration */
static void (*const stub_table[256])(void) = {
    isr_stub_0,   isr_stub_1,   isr_stub_2,   isr_stub_3,
    isr_stub_4,   isr_stub_5,   isr_stub_6,   isr_stub_7,
    isr_stub_8,   isr_stub_9,   isr_stub_10,  isr_stub_11,
    isr_stub_12,  isr_stub_13,  isr_stub_14,  isr_stub_15,
    isr_stub_16,  isr_stub_17,  isr_stub_18,  isr_stub_19,
    isr_stub_20,  isr_stub_21,  isr_stub_22,  isr_stub_23,
    isr_stub_24,  isr_stub_25,  isr_stub_26,  isr_stub_27,
    isr_stub_28,  isr_stub_29,  isr_stub_30,  isr_stub_31,
    isr_stub_32,  isr_stub_33,  isr_stub_34,  isr_stub_35,
    isr_stub_36,  isr_stub_37,  isr_stub_38,  isr_stub_39,
    isr_stub_40,  isr_stub_41,  isr_stub_42,  isr_stub_43,
    isr_stub_44,  isr_stub_45,  isr_stub_46,  isr_stub_47,
    isr_stub_48,  isr_stub_49,  isr_stub_50,  isr_stub_51,
    isr_stub_52,  isr_stub_53,  isr_stub_54,  isr_stub_55,
    isr_stub_56,  isr_stub_57,  isr_stub_58,  isr_stub_59,
    isr_stub_60,  isr_stub_61,  isr_stub_62,  isr_stub_63,
    isr_stub_64,  isr_stub_65,  isr_stub_66,  isr_stub_67,
    isr_stub_68,  isr_stub_69,  isr_stub_70,  isr_stub_71,
    isr_stub_72,  isr_stub_73,  isr_stub_74,  isr_stub_75,
    isr_stub_76,  isr_stub_77,  isr_stub_78,  isr_stub_79,
    isr_stub_80,  isr_stub_81,  isr_stub_82,  isr_stub_83,
    isr_stub_84,  isr_stub_85,  isr_stub_86,  isr_stub_87,
    isr_stub_88,  isr_stub_89,  isr_stub_90,  isr_stub_91,
    isr_stub_92,  isr_stub_93,  isr_stub_94,  isr_stub_95,
    isr_stub_96,  isr_stub_97,  isr_stub_98,  isr_stub_99,
    isr_stub_100, isr_stub_101, isr_stub_102, isr_stub_103,
    isr_stub_104, isr_stub_105, isr_stub_106, isr_stub_107,
    isr_stub_108, isr_stub_109, isr_stub_110, isr_stub_111,
    isr_stub_112, isr_stub_113, isr_stub_114, isr_stub_115,
    isr_stub_116, isr_stub_117, isr_stub_118, isr_stub_119,
    isr_stub_120, isr_stub_121, isr_stub_122, isr_stub_123,
    isr_stub_124, isr_stub_125, isr_stub_126, isr_stub_127,
    isr_stub_128, isr_stub_129, isr_stub_130, isr_stub_131,
    isr_stub_132, isr_stub_133, isr_stub_134, isr_stub_135,
    isr_stub_136, isr_stub_137, isr_stub_138, isr_stub_139,
    isr_stub_140, isr_stub_141, isr_stub_142, isr_stub_143,
    isr_stub_144, isr_stub_145, isr_stub_146, isr_stub_147,
    isr_stub_148, isr_stub_149, isr_stub_150, isr_stub_151,
    isr_stub_152, isr_stub_153, isr_stub_154, isr_stub_155,
    isr_stub_156, isr_stub_157, isr_stub_158, isr_stub_159,
    isr_stub_160, isr_stub_161, isr_stub_162, isr_stub_163,
    isr_stub_164, isr_stub_165, isr_stub_166, isr_stub_167,
    isr_stub_168, isr_stub_169, isr_stub_170, isr_stub_171,
    isr_stub_172, isr_stub_173, isr_stub_174, isr_stub_175,
    isr_stub_176, isr_stub_177, isr_stub_178, isr_stub_179,
    isr_stub_180, isr_stub_181, isr_stub_182, isr_stub_183,
    isr_stub_184, isr_stub_185, isr_stub_186, isr_stub_187,
    isr_stub_188, isr_stub_189, isr_stub_190, isr_stub_191,
    isr_stub_192, isr_stub_193, isr_stub_194, isr_stub_195,
    isr_stub_196, isr_stub_197, isr_stub_198, isr_stub_199,
    isr_stub_200, isr_stub_201, isr_stub_202, isr_stub_203,
    isr_stub_204, isr_stub_205, isr_stub_206, isr_stub_207,
    isr_stub_208, isr_stub_209, isr_stub_210, isr_stub_211,
    isr_stub_212, isr_stub_213, isr_stub_214, isr_stub_215,
    isr_stub_216, isr_stub_217, isr_stub_218, isr_stub_219,
    isr_stub_220, isr_stub_221, isr_stub_222, isr_stub_223,
    isr_stub_224, isr_stub_225, isr_stub_226, isr_stub_227,
    isr_stub_228, isr_stub_229, isr_stub_230, isr_stub_231,
    isr_stub_232, isr_stub_233, isr_stub_234, isr_stub_235,
    isr_stub_236, isr_stub_237, isr_stub_238, isr_stub_239,
    isr_stub_240, isr_stub_241, isr_stub_242, isr_stub_243,
    isr_stub_244, isr_stub_245, isr_stub_246, isr_stub_247,
    isr_stub_248, isr_stub_249, isr_stub_250, isr_stub_251,
    isr_stub_252, isr_stub_253, isr_stub_254, isr_stub_255,
};

/* =========================================================================
 * IDT entry helpers
 * ========================================================================= */

static void idt_set_entry(int vec, void (*handler)(void))
{
    uint64_t addr = (uint64_t)(uintptr_t)handler;
    g_idt[vec].offset_low  = (uint16_t)(addr & 0xFFFF);
    g_idt[vec].selector    = 0x08;          /* kernel code segment */
    g_idt[vec].ist         = 0;
    g_idt[vec].type_attr   = 0x8E;          /* present, 64-bit interrupt gate */
    g_idt[vec].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    g_idt[vec].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    g_idt[vec].zero        = 0;
}

/* =========================================================================
 * IDT_Init
 * ========================================================================= */

void IDT_Init(void)
{
    for (int i = 0; i < 256; i++) {
        g_handlers[i] = NULL;
        idt_set_entry(i, stub_table[i]);
    }

    IdtPtr idtp;
    idtp.limit = (uint16_t)(sizeof(g_idt) - 1);
    idtp.base  = (uint64_t)(uintptr_t)g_idt;

    __asm__ volatile ("lidt %0" :: "m"(idtp));
}

void IDT_SetHandler(uint8_t vector, ISRHandler handler)
{
    g_handlers[vector] = handler;
}

void IDT_SetRawHandler(uint8_t vector, void (*handler)(void))
{
    idt_set_entry(vector, handler);
}

/* =========================================================================
 * ISR_Dispatch — called from isr_common in idt_stubs.asm
 * ========================================================================= */

/* Debug: memory-mapped mailbox at a known physical address.
 * We write a magic sequence here so the event loop can detect
 * whether ISR_Dispatch was entered at all. */
static volatile uint32_t *const g_isr_mailbox = (volatile uint32_t *)0x90000;

void ISR_Dispatch(uint64_t vector, uint64_t error_code)
{
    /* Write a rotating magic value to the mailbox */
    static volatile uint32_t mailbox_seq = 0;
    uint32_t seq = mailbox_seq++;
    g_isr_mailbox[0] = 0xDEADBEEF;
    g_isr_mailbox[1] = (uint32_t)vector;
    g_isr_mailbox[2] = seq;

    if (g_handlers[vector]) {
        g_handlers[vector](vector, error_code);
    } else if (vector < 32) {
        /* Unhandled CPU exception — halt */
        __asm__ volatile ("cli; hlt");
    }
    /* Send EOI to PIC for all hardware IRQs (vectors 32-47) */
    if (vector >= 32 && vector < 48) {
        PIC_SendEOI((int)(vector - 32));
    }
}

/* =========================================================================
 * 8259A PIC driver
 * ========================================================================= */

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20

#define ICW1_ICW4  0x01
#define ICW1_INIT  0x10
#define ICW4_8086  0x01

void PIC_Init(void)
{
    /* Save masks */
    uint8_t m1 = inb(PIC1_DATA);
    uint8_t m2 = inb(PIC2_DATA);
    (void)m1; (void)m2;

    /* Start initialisation sequence (cascade mode) */
    outb(PIC1_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, 0x20); io_wait();   /* IRQ0-7  → vectors 32-39  */
    outb(PIC2_DATA, 0x28); io_wait();   /* IRQ8-15 → vectors 40-47  */

    /* ICW3: cascade wiring */
    outb(PIC1_DATA, 0x04); io_wait();   /* master: IRQ2 has slave    */
    outb(PIC2_DATA, 0x02); io_wait();   /* slave:  cascade identity 2 */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Mask all IRQs initially */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void PIC_UnmaskIRQ(int irq)
{
    uint16_t port;
    uint8_t  val;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
        /* Also unmask IRQ2 on master (cascade line) */
        val = inb(PIC1_DATA) & ~(1 << 2);
        outb(PIC1_DATA, val);
    }
    val = inb(port) & (uint8_t)(~(1 << irq));
    outb(port, val);
}

void PIC_MaskIRQ(int irq)
{
    uint16_t port;
    if (irq < 8) port = PIC1_DATA;
    else         { port = PIC2_DATA; irq -= 8; }
    uint8_t val = inb(port) | (uint8_t)(1 << irq);
    outb(port, val);
}

void PIC_SendEOI(int irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}
