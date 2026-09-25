/* Orbis augmentation to the pinned ps5-pie.ld (do not modify the original).
 * libunwind (pulled via libc++abi for static destructors) expects the
 * standard eh_frame bounds symbols, which the app script omits because
 * probe apps never unwind. Provide them for real so unwinding would work. */
PROVIDE(__eh_frame_start = ADDR(.eh_frame));
PROVIDE(__eh_frame_end = ADDR(.eh_frame) + SIZEOF(.eh_frame));
PROVIDE(__eh_frame_hdr_start = ADDR(.eh_frame_hdr));
PROVIDE(__eh_frame_hdr_end = ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr));
