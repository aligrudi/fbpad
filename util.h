#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#define LIMIT(n, a, b)	((n) < (a) ? (a) : ((n) > (b) ? (b) : (n)))
