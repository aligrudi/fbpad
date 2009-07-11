static void escseq(void);
static void escseq_cs(void);
static void escseq_g0(void);
static void escseq_g1(void);
static void escseq_g2(void);
static void escseq_g3(void);
static void csiseq(void);
static void modeseq(int c, int set);

/* control sequences */
static void ctlseq(void)
{
	int c = readpty();
	switch (c) {
	case 0x09:	/* HT		horizontal tab to next tab stop */
		move_cursor(row, (col / 8 + 1) * 8);
		break;
	case 0x0a:	/* LF		line feed */
	case 0x0b:	/* VT		line feed */
	case 0x0c:	/* FF		line feed */
		move_cursor(row + 1, 0);
		break;
	case 0x08:	/* BS		backspace one column */
		move_cursor(row, col - 1);
		break;
	case 0x1b:	/* ESC		start escape sequence */
		escseq();
		break;
	case 0x0d:	/* CR		carriage return */
		move_cursor(row, 0);
		break;
	case 0x00:	/* NUL		ignored */
	case 0x07:	/* BEL		beep */
	case 0x3f:	/* DEL		ignored */
		break;
	case 0x05:	/* ENQ		trigger answerback message */
	case 0x0e:	/* SO		activate G1 character set & newline */
	case 0x0f:	/* SI		activate G0 character set */
	case 0x11:	/* XON		resume transmission */
	case 0x13:	/* XOFF		stop transmission, ignore characters */
	case 0x18:	/* CAN		interrupt escape sequence */
	case 0x1a:	/* SUB		interrupt escape sequence */
	case 0x9b:	/* CSI		equivalent to ESC [ */
		printf("ctlseq: <%d:%c>\n", c, c);
		break;
	default:
		term_put(c, row, col);
		advance(0, 1);
		break;
	}
}

/* escape sequences */
static void escseq(void)
{
	int c = readpty();
	switch(c) {
	case 'M':	/* RI		reverse line feed */
		scroll_screen(top, bot - top - 1, 1);
		break;
	case '[':	/* CSI		control sequence introducer */
		csiseq();
		break;
	case '%':	/* CS...	escseq_cs table */
		escseq_cs();
		break;
	case '(':	/* G0...	escseq_g0 table */
		escseq_g0();
		break;
	case ')':	/* G1...	escseq_g1 table */
		escseq_g1();
		break;
	case '*':	/* G2...	escseq_g2 table */
		escseq_g2();
		break;
	case '+':	/* G3...	escseq_g3 table */
		escseq_g3();
		break;
	case 'c':	/* RIS		reset */
	case 'D':	/* IND		line feed */
	case 'E':	/* NEL		newline */
	case 'H':	/* HTS		set tab stop at current column */
	case 'Z':	/* DECID	DEC private ID; return ESC [ ? 6 c (VT102) */
	case '7':	/* DECSC	save state (position, charset, attributes) */
	case '8':	/* DECRC	restore most recently saved state */
	case '#':	/* DECALN	("#8") DEC alignment test - fill screen with E's */
	case '>':	/* DECPNM	set numeric keypad mode */
	case '=':	/* DECPAM	set application keypad mode */
	case 'N':	/* SS2		select G2 charset for next char only */
	case 'O':	/* SS3		select G3 charset for next char only */
	case 'P':	/* DCS		device control string (ended by ST) */
	case 'X':	/* SOS		start of string */
	case '^':	/* PM		privacy message (ended by ST) */
	case '_':	/* APC		application program command (ended by ST) */
	case '\\':	/* ST		string terminator */
	case 'n':	/* LS2		invoke G2 charset */
	case 'o':	/* LS3		invoke G3 charset */
	case '|':	/* LS3R		invoke G3 charset as GR */
	case '}':	/* LS2R		invoke G2 charset as GR */
	case '~':	/* LS1R		invoke G1 charset as GR */
	case ']':	/* OSC		operating system command */
	case 'g':	/* BEL		alternate BEL */
	default:
		printf("escseq: <%d:%c>\n", c, c);
		break;
	}
}

static void escseq_cs(void)
{
	int c = readpty();
	switch(c) {
	case '@':	/* CSDFL	select default charset (ISO646/8859-1) */
	case 'G':	/* CSUTF8	select UTF-8 */
	case '8':	/* CSUTF8	select UTF-8 (obsolete) */
	default:
		printf("escseq_cs: <%d:%c>\n", c, c);
		break;
	}
}

static void escseq_g0(void)
{
	int c = readpty();
	switch(c) {
	case '8':	/* G0DFL	G0 charset = default mapping (ISO8859-1) */
	case '0':	/* G0GFX	G0 charset = VT100 graphics mapping */
	case 'U':	/* G0ROM	G0 charset = null mapping (straight to ROM) */
	case 'K':	/* G0USR	G0 charset = user defined mapping */
	case 'B':	/* G0TXT	G0 charset = ASCII mapping */
	default:
		printf("escseq_g0: <%d:%c>\n", c, c);
		break;
	}
}

static void escseq_g1(void)
{
	int c = readpty();
	switch(c) {
	case '8':	/* G1DFL	G1 charset = default mapping (ISO8859-1) */
	case '0':	/* G1GFX	G1 charset = VT100 graphics mapping */
	case 'U':	/* G1ROM	G1 charset = null mapping (straight to ROM) */
	case 'K':	/* G1USR	G1 charset = user defined mapping */
	case 'B':	/* G1TXT	G1 charset = ASCII mapping */
	default:
		printf("escseq_g1: <%d:%c>\n", c, c);
		break;
	}
}

static void escseq_g2(void)
{
	int c = readpty();
	switch(c) {
	case '8':	/* G2DFL	G2 charset = default mapping (ISO8859-1) */
	case '0':	/* G2GFX	G2 charset = VT100 graphics mapping */
	case 'U':	/* G2ROM	G2 charset = null mapping (straight to ROM) */
	case 'K':	/* G2USR	G2 charset = user defined mapping */
	default:
		printf("escseq_g2: <%d:%c>\n", c, c);
		break;
	}
}

static void escseq_g3(void)
{
	int c = readpty();
	switch(c) {
	case '8':	/* G3DFL	G3 charset = default mapping (ISO8859-1) */
	case '0':	/* G3GFX	G3 charset = VT100 graphics mapping */
	case 'U':	/* G3ROM	G3 charset = null mapping (straight to ROM) */
	case 'K':	/* G3USR	G3 charset = user defined mapping */
	default:
		printf("escseq_g3: <%d:%c>\n", c, c);
		break;
	}
}

#define MAXCSIARGS	32
/* ECMA-48 CSI sequences */
static void csiseq(void)
{
	int args[MAXCSIARGS] = {0};
	int i;
	int n = 0;
	int c = 0;
	for (i = 0; i < ARRAY_SIZE(args) && !isalpha(c); i++) {
		int arg = 0;
		while (isdigit((c = readpty())))
			arg = arg * 10 + (c - '0');
		args[n++] = arg;
	}
	switch(c) {
	case 'H':	/* CUP		move cursor to row, column */
	case 'f':	/* HVP		move cursor to row, column */
		move_cursor(MAX(0, args[0] - 1), MAX(0, args[1] - 1));
		break;
	case 'J':	/* ED		erase display */
		switch(args[0]) {
		case 0:
			kill_chars(col, pad_cols());
			blank_rows(row + 1, pad_rows());
			break;
		case 1:
			kill_chars(0, col + 1);
			blank_rows(0, row - 1);
			break;
		case 2:
			term_blank();
			break;
		}
		break;
	case 'A':	/* CUU		move cursor up */
		advance(MAX(1, args[0]), 0);
		break;
	case 'B':	/* CUD		move cursor down */
		advance(MAX(1, args[0]), 0);
		break;
	case 'C':	/* CUF		move cursor right */
		advance(0, MAX(1, args[0]));
		break;
	case 'D':	/* CUB		move cursor left */
		move_cursor(0, -MAX(1, args[0]));
		break;
	case 'K':	/* EL		erase line */
		kill_chars(col, pad_cols());
		break;
	case 'L':	/* IL		insert blank lines */
		insert_lines(MAX(1, args[0]));
		break;
	case 'M':	/* DL		delete lines */
		delete_lines(MAX(1, args[0]));
		break;
	case 'd':	/* VPA		move to row (current column) */
		move_cursor(MAX(1, args[0]), col);
		break;
	case 'm':	/* SGR		set graphic rendition */
		setmode(0);
		for (i = 0; i < n; i++)
			setmode(args[i]);
		break;
	case 'h':	/* SM		set mode */
		modeseq(n <= 1 ? args[0] : 0x80 | args[1], 1);
		break;
	case 'l':	/* RM		reset mode */
		modeseq(n <= 1 ? args[0] : 0x80 | args[1], 0);
		break;
	case 'r':	/* DECSTBM	set scrolling region to (top, bottom) rows */
		top = MIN(pad_rows(), MAX(0, args[0] - 1));
		bot = MIN(pad_rows(), MAX(0, args[1] ? args[1] : pad_rows()));
		break;
	case '[':	/* IGN		ignored control sequence */
	case '@':	/* ICH		insert blank characters */
	case 'E':	/* CNL		move cursor down and to column 1 */
	case 'F':	/* CPL		move cursor up and to column 1 */
	case 'G':	/* CHA		move cursor to column in current row */
	case 'P':	/* DCH		delete characters on current line */
	case 'X':	/* ECH		erase characters on current line */
	case 'a':	/* HPR		move cursor right */
	case 'c':	/* DA		return ESC [ ? 6 c (VT102) */
	case 'e':	/* VPR		move cursor down */
	case 'g':	/* TBC		clear tab stop (CSI 3 g = clear all stops) */
	case 'n':	/* DSR		device status report */
	case 'q':	/* DECLL	set keyboard LEDs */
	case 's':	/* CUPSV	save cursor position */
	case 'u':	/* CUPRS	restore cursor position */
	case '`':	/* HPA		move cursor to column in current row */
	default:
		printf("csiseq: <%d:%c>:", c, c);
		for (i = 0; i < n; i++)
			printf(" %d", args[i]);
		printf("\n");
		break;
	}
}

/* ANSI/DEC specified modes for SM/RM ANSI Specified Modes */
static void modeseq(int c, int set)
{
	switch(c) {
	case 0x87:	/* DECAWM	Auto Wrap */
		nowrap = !set;
		break;
	case 0x99:	/* DECTCEM	Cursor on (set); Cursor off (reset) */
		nocursor = !set;
		break;
	case 0x00:	/* IGN		Error (Ignored) */
	case 0x01:	/* GATM		guarded-area transfer mode (ignored) */
	case 0x02:	/* KAM		keyboard action mode (always reset) */
	case 0x03:	/* CRM		control representation mode (always reset) */
	case 0x04:	/* IRM		insertion/replacement mode (always reset) */
	case 0x05:	/* SRTM		status-reporting transfer mode */
	case 0x06:	/* ERM		erasure mode (always set) */
	case 0x07:	/* VEM		vertical editing mode (ignored) */
	case 0x0a:	/* HEM		horizontal editing mode */
	case 0x0b:	/* PUM		positioning unit mode */
	case 0x0c:	/* SRM		send/receive mode (echo on/off) */
	case 0x0d:	/* FEAM		format effector action mode */
	case 0x0e:	/* FETM		format effector transfer mode */
	case 0x0f:	/* MATM		multiple area transfer mode */
	case 0x10:	/* TTM		transfer termination mode */
	case 0x11:	/* SATM		selected area transfer mode */
	case 0x12:	/* TSM		tabulation stop mode */
	case 0x13:	/* EBM		editing boundary mode */
	case 0x14:	/* LNM		Line Feed / New Line Mode */
/* DEC Private Modes: "?NUM" -> (NUM | 0x80) */
	case 0x80:	/* IGN		Error (Ignored) */
	case 0x81:	/* DECCKM	Cursorkeys application (set); Cursorkeys normal (reset) */
	case 0x82:	/* DECANM	ANSI (set); VT52 (reset) */
	case 0x83:	/* DECCOLM	132 columns (set); 80 columns (reset) */
	case 0x84:	/* DECSCLM	Jump scroll (set); Smooth scroll (reset) */
	case 0x85:	/* DECSCNM	Reverse screen (set); Normal screen (reset) */
	case 0x86:	/* DECOM	Sets relative coordinates (set); Sets absolute coordinates (reset) */
	case 0x88:	/* DECARM	Auto Repeat */
	case 0x89:	/* DECINLM	Interlace */
	case 0x92:	/* DECPFF	Send FF to printer after print screen (set); No char after PS (reset) */
	case 0x93:	/* DECPEX	Print screen: prints full screen (set); prints scroll region (reset) */
	default:
		printf("modeseq <0x%x>: %d\n", c, set);
		break;
	}
}
