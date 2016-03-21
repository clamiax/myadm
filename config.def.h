/* See LICENSE file for copyright and license details. */

#define FLDSEP " | "
#define MAXCOLSZ 19

static const char *dbhost = "";
static const char *dbuser = "";
static const char *dbpass = "";

static void (*welcome)(const Arg *arg) = databases;

#if defined CTRL && defined _AIX
  #undef CTRL
#endif
#ifndef CTRL
  #define CTRL(k)   ((k) & 0x1F)
#endif
#define CTRL_ALT(k) ((k) + (129 - 'a'))

static Key keys[] = {
	/* mode          key           function        argument */
        { NULL,          'Q',          quit,           {.i = 1} },
        { NULL,          'q',          viewprev,       {0} },
        { NULL,          'k',          itempos,        {.i = -1} },
        { NULL,          KEY_UP,       itempos,        {.i = -1} },
        { NULL,          'j',          itempos,        {.i = +1} },
        { NULL,          KEY_DOWN,     itempos,        {.i = +1} },
        { NULL,          'I',          reload,         {0} },
        { "databases",   'q',          quit,           {.i = 0} },
        { "databases",   '\n',         tables,         {0} },
        { "databases",   ' ',          tables,         {0} },
        { "tables",      '\n',         records,        {.i = 500} },
        { "tables",      ' ',          records,        {.i = 500} },
};
