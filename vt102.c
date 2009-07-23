static void escseq(void);
static void escseq_cs(void);
static void escseq_g0(void);
static void escseq_g1(void);
static void escseq_g2(void);
static void escseq_g3(void);
static void csiseq(void);
static void csiseq_da(int c);
static void csiseq_dsr(int c);
static void modeseq(int c, int set);

/* comments taken from: http://www.ivarch.com/programs/termvt102.shtml */

static int readutf8(int c)
{
	int result;
	int l = 1;
	if (~c & 0xc0)
		return c;
	while (l < 6 && c & (0x40 >> l))
		l++;
	result = (0x3f >> l) & c;
	while (l--)
		result = (result << 6) | (readpty() & 0x3f);
	return result;
}

/* control sequences */
static void ctlseq(void)
{
	int c = readpty();
	switch (c) {
	case 0x09:	/* HT		horizontal tab to next tab stop */
		advance(0, 8 - col % 8, 0);
		break;
	case 0x0a:	/* LF		line feed */
	case 0x0b:	/* VT		line feed */
	case 0x0c:	/* FF		line feed */
		advance(1, (mode & MODE_AUTOCR) ? -col : 0, 1);
		break;
	case 0x08:	/* BS		backspace one column */
		advance(0, -1, 0);
		break;
	case 0x1b:	/* ESC		start escape sequence */
		escseq();
		break;
	case 0x0d:	/* CR		carriage return */
		advance(0, -col, 0);
		break;
	case 0x00:	/* NUL		ignored */
	case 0x07:	/* BEL		beep */
	case 0x7f:	/* DEL		ignored */
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
		insertchar(readutf8(c));
		break;
	}
}

#define ESCM(c)		(((c) & 0xf0) == 0x20)
#define ESCF(c)		((c) > 0x30 && (c) < 0x7f)

/* escape sequences */
static void escseq(void)
{
	int c = readpty();
	while (ESCM(c))
		c = readpty();
	switch(c) {
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
	case '7':	/* DECSC	save state (position, charset, attributes) */
		misc_save(&term->sav);
		break;
	case '8':	/* DECRC	restore most recently saved state */
		misc_load(&term->sav);
		break;
	case 'M':	/* RI		reverse line feed */
		advance(-1, 0, 1);
		break;
	case 'D':	/* IND		line feed */
		advance(1, 0, 1);
		break;
	case 'E':	/* NEL		newline */
		advance(1, -col, 1);
		break;
	case 'c':	/* RIS		reset */
	case 'H':	/* HTS		set tab stop at current column */
	case 'Z':	/* DECID	DEC private ID; return ESC [ ? 6 c (VT102) */
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

static int absrow(int r)
{
	return origin() ? top + r : r;
}

#define CSIP(c)			(((c) & 0xf0) == 0x30)
#define CSII(c)			(((c) & 0xf0) == 0x20)
#define CSIF(c)			((c) >= 0x40 && (c) < 0x80)

#define MAXCSIARGS	32
/* ECMA-48 CSI sequences */
static void csiseq(void)
{
	int args[MAXCSIARGS] = {0};
	int i;
	int n = 0;
	int c = readpty();
	int inter = 0;
	int priv = 0;

	if (strchr("<=>?", c)) {
		priv = c;
		c = readpty();
	}
	while (CSIP(c)) {
		int arg = 0;
		while (isdigit(c)) {
			arg = arg * 10 + (c - '0');
			c = readpty();
		}
		if (c == ';')
			c = readpty();
		args[n] = arg;
		n = n < ARRAY_SIZE(args) ? n + 1 : 0;
	}
	while (CSII(c)) {
		inter = c;
		c = readpty();
	}
	switch(c) {
	case 'H':	/* CUP		move cursor to row, column */
	case 'f':	/* HVP		move cursor to row, column */
		move_cursor(absrow(MAX(0, args[0] - 1)), MAX(0, args[1] - 1));
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
		advance(-MAX(1, args[0]), 0, 0);
		break;
	case 'e':	/* VPR		move cursor down */
	case 'B':	/* CUD		move cursor down */
		advance(MAX(1, args[0]), 0, 0);
		break;
	case 'a':	/* HPR		move cursor right */
	case 'C':	/* CUF		move cursor right */
		advance(0, MAX(1, args[0]), 0);
		break;
	case 'D':	/* CUB		move cursor left */
		advance(0, -MAX(1, args[0]), 0);
		break;
	case 'K':	/* EL		erase line */
		switch (args[0]) {
		case 0:
			kill_chars(col, pad_cols());
			break;
		case 1:
			kill_chars(0, col + 1);
			break;
		case 2:
			kill_chars(0, pad_cols());
			break;
		}
		break;
	case 'L':	/* IL		insert blank lines */
		if (row >= top && row < bot)
			insert_lines(MAX(1, args[0]));
		break;
	case 'M':	/* DL		delete lines */
		if (row >= top && row < bot)
			delete_lines(MAX(1, args[0]));
		break;
	case 'd':	/* VPA		move to row (current column) */
		move_cursor(absrow(MAX(1, args[0]) - 1), col);
		break;
	case 'm':	/* SGR		set graphic rendition */
		if (!n)
			setattr(0);
		for (i = 0; i < n; i++)
			setattr(args[i]);
		break;
	case 'r':	/* DECSTBM	set scrolling region to (top, bottom) rows
 */
		set_region(args[0], args[1]);
		break;
	case 'c':	/* DA		return ESC [ ? 6 c (VT102) */
		csiseq_da(priv == '?' ? args[0] | 0x80 : args[0]);
		break;
	case 'h':	/* SM		set mode */
		for (i = 0; i < n; i++)
			modeseq(priv == '?' ? args[i] | 0x80 : args[i], 1);
		break;
	case 'l':	/* RM		reset mode */
		for (i = 0; i < n; i++)
			modeseq(priv == '?' ? args[i] | 0x80 : args[i], 0);
		break;
	case 'P':	/* DCH		delete characters on current line */
		delete_chars(MIN(MAX(1, args[0]), pad_cols() - col));
		break;
	case '@':	/* ICH		insert blank characters */
		insert_chars(MAX(1, args[0]));
		break;
	case 'n':	/* DSR		device status report */
		csiseq_dsr(args[0]);
		break;
	case 'G':	/* CHA		move cursor to column in current row */
		advance(0, MAX(0, args[0] - 1) - col, 0);
		break;
	case 'X':	/* ECH		erase characters on current line */
		kill_chars(col, MIN(col + MAX(1, args[0]), pad_cols()));
		break;
	case '[':	/* IGN		ignored control sequence */
	case 'E':	/* CNL		move cursor down and to column 1 */
	case 'F':	/* CPL		move cursor up and to column 1 */
	case 'g':	/* TBC		clear tab stop (CSI 3 g = clear all stops) */
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

static void csiseq_da(int c)
{
	switch(c) {
	case 0x00:
		term_sendstr("\x1b[?6c");
		break;
	default:
		/* we don't care much about cursor shape */
		/* printf("csiseq_da <0x%x>\n", c); */
		break;
	}
}

static void csiseq_dsr(int c)
{
	char status[1 << 5];
	switch(c) {
	case 0x05:
		term_sendstr("\x1b[0n");
		break;
	case 0x06:
		snprintf(status, sizeof(status), "\x1b[%d;%dR",
			 (origin() ? row - top : row) + 1, col + 1);
		term_sendstr(status);
		break;
	default:
		printf("csiseq_dsr <0x%x>\n", c);
		break;
	}
}

/* ANSI/DEC specified modes for SM/RM ANSI Specified Modes */
static void modeseq(int c, int set)
{
	switch(c) {
	case 0x87:	/* DECAWM	Auto Wrap */
		mode = BIT_SET(mode, MODE_WRAP, set);
		break;
	case 0x99:	/* DECTCEM	Cursor on (set); Cursor off (reset) */
		mode = BIT_SET(mode, MODE_CURSOR, set);
		break;
	case 0x86:	/* DECOM	Sets relative coordinates (set); Sets absolute coordinates (reset) */
		mode = BIT_SET(mode, MODE_ORIGIN, set);
		break;
	case 0x14:	/* LNM		Line Feed / New Line Mode */
		mode = BIT_SET(mode, MODE_AUTOCR, set);
		break;
	case 0x04:	/* IRM		insertion/replacement mode (always reset) */
		break;
	case 0x00:	/* IGN		Error (Ignored) */
	case 0x01:	/* GATM		guarded-area transfer mode (ignored) */
	case 0x02:	/* KAM		keyboard action mode (always reset) */
	case 0x03:	/* CRM		control representation mode (always reset) */
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
/* DEC Private Modes: "?NUM" -> (NUM | 0x80) */
	case 0x80:	/* IGN		Error (Ignored) */
	case 0x81:	/* DECCKM	Cursorkeys application (set); Cursorkeys normal (reset) */
	case 0x82:	/* DECANM	ANSI (set); VT52 (reset) */
	case 0x83:	/* DECCOLM	132 columns (set); 80 columns (reset) */
	case 0x84:	/* DECSCLM	Jump scroll (set); Smooth scroll (reset) */
	case 0x85:	/* DECSCNM	Reverse screen (set); Normal screen (reset) */
	case 0x88:	/* DECARM	Auto Repeat */
	case 0x89:	/* DECINLM	Interlace */
	case 0x92:	/* DECPFF	Send FF to printer after print screen (set); No char after PS (reset) */
	case 0x93:	/* DECPEX	Print screen: prints full screen (set); prints scroll region (reset) */
	default:
		printf("modeseq <0x%x>: %d\n", c, set);
		break;
	}
}
