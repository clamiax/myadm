/* See LICENSE file for copyright and license details. */

#define FLDSEP " | "
#define MAXCOLSZ 19

static const char *dbhost = "";
static const char *dbuser = "";
static const char *dbpass = "";

static Mode modes[] = {
	/* name         function */
	{ "databases",  databases }, /* first entry is default */
	{ "tables",     tables },
	{ "records",    records },
	{ "text",       text },
};

static Key keys[] = {
	/* mode          modkey        function        argument */
        { NULL,          L"Q",         quit,           {.i = 1} },
        { NULL,          L"q",         viewprev,       {0} },
        { NULL,          L"k",         itempos,        {.i = -1} },
        { NULL,          L"j",         itempos,        {.i = +1} },
        { NULL,          L"I",         reload,         {0} },
        { NULL,          L"$",         apply,          {.i = 1} },
        { "databases",   L"q",         quit,           {.i = 0} },
        { "databases",   L"ENTER",     usedb,          {.v = &modes[1]} },
        { "databases",   L"SPACE",     usedb,          {.v = &modes[1]} },
        { "tables",      L"ENTER",     setmode,        {.v = &modes[2]} },
        { "tables",      L"SPACE",     setmode,        {.v = &modes[2]} },
        { "records",     L"d",         flagas,         {.v = "D"} },
        { "records",     L"t",         flagas,         {.v = "*"} },
};
